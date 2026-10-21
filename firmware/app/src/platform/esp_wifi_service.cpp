#include "esp_wifi_service.h"

#include <Arduino.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <new>

#include "prefs.h"

namespace {

constexpr const char* kHostname = "soccer-display";
constexpr const char* kNvsNamespace = "wifi";  // 4 characters, NVS limit 15
constexpr const char* kNvsKey = "nets";        // a valid prefs key (checked in the constructor)
constexpr int kWorkerCore = 0;
constexpr UBaseType_t kWorkerPriority = 3;  // below the Wi-Fi driver task, above idle
constexpr uint32_t kWorkerStackBytes = 6144;
constexpr int kCommandQueueLen = 8;
constexpr int kEventQueueLen = 16;
constexpr uint32_t kRssiPeriodMs = 2000;
constexpr uint16_t kMaxScanRecords = 40;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------- NVS blob

class NvsWifiBlob : public WifiBlobStorage {
 public:
  bool load(std::string& out) override {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;  // nothing saved yet
    if (err != ESP_OK) {
      Serial.printf("[wifi] cannot open the saved-network store: %s\n", esp_err_to_name(err));
      return false;
    }
    size_t len = 0;
    err = nvs_get_blob(h, kNvsKey, nullptr, &len);
    if (err == ESP_OK && (len == 0 || len > WifiStore::kMaxSerializedBytes)) {
      Serial.printf("[wifi] saved-network blob has an unexpected size (%u bytes); ignored\n", (unsigned)len);
      err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
      out.assign(len, '\0');
      err = nvs_get_blob(h, kNvsKey, &out[0], &len);
      if (err == ESP_OK) out.resize(len);
    }
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      Serial.printf("[wifi] reading the saved networks failed: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK;
  }

  bool save(const std::string& data) override {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
      Serial.printf("[wifi] cannot open the saved-network store: %s\n", esp_err_to_name(err));
      return false;
    }
    if (data.empty()) {
      err = nvs_erase_key(h, kNvsKey);
      if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else if (data.size() > WifiStore::kMaxSerializedBytes) {
      err = ESP_ERR_INVALID_SIZE;
    } else {
      err = nvs_set_blob(h, kNvsKey, data.data(), data.size());
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) Serial.printf("[wifi] writing the saved networks failed: %s\n", esp_err_to_name(err));
    return err == ESP_OK;
  }
};

// ---------------------------------------------------------------- driver events

struct DriverEvent {
  enum Type : uint8_t { GotIp, Disconnected } type;
  uint32_t gen;
  int reason;
  char ip[16];
};

struct Command {
  enum Type : uint8_t { StartScan, CancelScan, Join, Disconnect, ScanDone, Stop } type;
  uint32_t gen;
  char ssid[33];
  char password[64];  // 63 characters + NUL; zeroed after use
};

}  // namespace

// ---------------------------------------------------------------- the radio

// The controller's WifiRadio over the ESP-IDF driver. Methods of the WifiRadio
// interface are called from the LVGL task and only enqueue work or read atomics.
class EspRadio : public WifiRadio {
 public:
  enum ScanPhase : int { Idle = 0, Requested, Running, Done, Failed };

  bool begin() {
    if (task_) return true;
    commands_ = xQueueCreate(kCommandQueueLen, sizeof(Command));
    events_ = xQueueCreate(kEventQueueLen, sizeof(DriverEvent));
    if (!commands_ || !events_) {
      Serial.println("[wifi] ERROR: cannot create the Wi-Fi queues");
      release_queues();
      return false;
    }
    if (xTaskCreatePinnedToCore(&EspRadio::task_main, "wifi_worker", kWorkerStackBytes, this, kWorkerPriority,
                                &task_, kWorkerCore) != pdPASS) {
      Serial.println("[wifi] ERROR: cannot create the Wi-Fi worker task");
      task_ = nullptr;
      release_queues();
      return false;
    }
    return true;
  }

  // WifiRadio
  bool ready() override { return ready_.load(); }
  bool start_scan() override {
    int expected = Idle;
    if (!scanPhase_.compare_exchange_strong(expected, Requested)) return false;
    Command c = {};
    c.type = Command::StartScan;
    if (!post(c)) {
      scanPhase_ = Idle;
      return false;
    }
    return true;
  }
  ScanState poll_scan(std::vector<WifiScanEntry>& out) override {
    const int phase = scanPhase_.load();
    if (phase == Done) {
      out = std::move(results_);
      results_.clear();
      scanPhase_ = Idle;
      return ScanState::Done;
    }
    if (phase == Failed) {
      scanPhase_ = Idle;
      return ScanState::Failed;
    }
    if (phase == Idle) return ScanState::Failed;  // cancelled meanwhile
    return ScanState::Running;
  }
  void cancel_scan() override {
    Command c = {};
    c.type = Command::CancelScan;
    post(c);
  }
  void join(uint32_t gen, const std::string& ssid, const std::string& password) override {
    Command c = {};
    c.type = Command::Join;
    c.gen = gen;
    strlcpy(c.ssid, ssid.c_str(), sizeof(c.ssid));
    strlcpy(c.password, password.c_str(), sizeof(c.password));
    if (!post(c)) {
      // The worker is not taking commands: report a failed attempt so the
      // controller retries within its window and finally times out.
      // (Before start() there is no queue at all: the controller never gets here
      // then, because ready() is false, but the guard stays.)
      if (events_) {
        DriverEvent e = {};
        e.type = DriverEvent::Disconnected;
        e.gen = gen;
        e.reason = 1;
        xQueueSend(events_, &e, 0);
      }
    }
    memset(c.password, 0, sizeof(c.password));
  }
  void disconnect() override {
    Command c = {};
    c.type = Command::Disconnect;
    post(c);
  }
  bool link_up() override { return linkUp_.load(); }
  int rssi() override { return rssi_.load(); }

  bool pop_event(DriverEvent& e) { return events_ && xQueueReceive(events_, &e, 0) == pdTRUE; }
  size_t stack_free_bytes() const { return task_ ? uxTaskGetStackHighWaterMark(task_) * sizeof(StackType_t) : 0; }

 private:
  void release_queues() {
    if (commands_) vQueueDelete(commands_);
    if (events_) vQueueDelete(events_);
    commands_ = nullptr;
    events_ = nullptr;
  }

  bool post(const Command& c) {
    if (!commands_) return false;
    if (xQueueSend(commands_, &c, 0) != pdTRUE) {
      Serial.println("[wifi] command queue full, command dropped");
      return false;
    }
    return true;
  }

  static void task_main(void* arg) {
    static_cast<EspRadio*>(arg)->run();
    vTaskDelete(nullptr);
  }

  // ---- worker task (core 0) ----

  bool init_driver() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
      Serial.printf("[wifi] esp_netif_init: %s\n", esp_err_to_name(err));
      return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: already created
      Serial.printf("[wifi] event loop: %s\n", esp_err_to_name(err));
      return false;
    }
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
      Serial.println("[wifi] cannot create the station interface");
      return false;
    }
    esp_netif_set_hostname(netif, kHostname);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
      Serial.printf("[wifi] esp_wifi_init: %s\n", esp_err_to_name(err));
      return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // the driver keeps no copy of the credentials in flash
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EspRadio::on_wifi_event, this);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EspRadio::on_ip_event, this);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &EspRadio::on_ip_event, this);
    err = esp_wifi_start();
    if (err != ESP_OK) {
      Serial.printf("[wifi] esp_wifi_start: %s\n", esp_err_to_name(err));
      return false;
    }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return true;
  }

  void run() {
    if (!init_driver()) {
      Serial.println("[wifi] the Wi-Fi driver did not start; scans find nothing");
      // Stay alive to answer scan requests with a failure instead of hanging them.
    } else {
      ready_ = true;
      Serial.printf("[wifi] radio ready (worker on core %d), hostname %s, driver config in RAM only\n",
                    (int)xPortGetCoreID(), kHostname);
    }
    uint32_t lastRssi = millis();
    for (;;) {
      Command c;
      if (xQueueReceive(commands_, &c, pdMS_TO_TICKS(250)) == pdTRUE) {
        handle(c);
        memset(c.password, 0, sizeof(c.password));
      }
      if (millis() - lastRssi >= kRssiPeriodMs) {
        lastRssi = millis();
        refresh_rssi();
      }
    }
  }

  void refresh_rssi() {
    wifi_ap_record_t ap;
    rssi_ = (ready_ && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
  }

  void handle(const Command& c) {
    switch (c.type) {
      case Command::StartScan: {
        if (scanPhase_.load() != Requested) break;
        if (!ready_) {
          scanPhase_ = Failed;
          break;
        }
        wifi_scan_config_t sc = {};
        sc.show_hidden = false;
        sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        sc.scan_time.active.min = 100;
        sc.scan_time.active.max = 300;
        const esp_err_t err = esp_wifi_scan_start(&sc, false);
        if (err != ESP_OK) {
          Serial.printf("[wifi] scan start failed: %s\n", esp_err_to_name(err));
          scanPhase_ = Failed;
        } else {
          scanPhase_ = Running;
        }
        break;
      }
      case Command::ScanDone:
        collect_scan();
        break;
      case Command::CancelScan:
        stop_scan();
        scanPhase_ = Idle;
        break;
      case Command::Join:
        do_join(c);
        break;
      case Command::Disconnect:
        gen_ = 0;
        linkUp_ = false;
        esp_wifi_disconnect();
        break;
      case Command::Stop:
        break;
    }
  }

  void stop_scan() {
    if (scanPhase_.load() == Running) {
      esp_wifi_scan_stop();
      uint16_t n = 0;
      esp_wifi_scan_get_ap_num(&n);  // frees the driver's list
      if (n > 0) {
        wifi_ap_record_t* drop = static_cast<wifi_ap_record_t*>(calloc(n, sizeof(wifi_ap_record_t)));
        if (drop) {
          esp_wifi_scan_get_ap_records(&n, drop);
          free(drop);
        } else {
          esp_wifi_clear_ap_list();
        }
      }
    }
  }

  void collect_scan() {
    if (scanPhase_.load() != Running) {  // cancelled, or a stale event: drop the driver's list
      uint16_t n = 0;
      if (esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0) esp_wifi_clear_ap_list();
      return;
    }
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
      scanPhase_ = Failed;
      return;
    }
    uint16_t take = std::min<uint16_t>(n, kMaxScanRecords);
    std::vector<WifiScanEntry> found;
    if (take > 0) {
      wifi_ap_record_t* recs = static_cast<wifi_ap_record_t*>(calloc(take, sizeof(wifi_ap_record_t)));
      if (!recs) {
        esp_wifi_clear_ap_list();
        scanPhase_ = Failed;
        return;
      }
      if (esp_wifi_scan_get_ap_records(&take, recs) == ESP_OK) {
        for (uint16_t i = 0; i < take; ++i) {
          WifiScanEntry e;
          e.ssid = std::string(reinterpret_cast<const char*>(recs[i].ssid),
                               strnlen(reinterpret_cast<const char*>(recs[i].ssid), sizeof(recs[i].ssid)));
          e.rssi = recs[i].rssi;
          e.auth = classify(recs[i].authmode);
          found.push_back(std::move(e));
        }
      } else {
        esp_wifi_clear_ap_list();
      }
      free(recs);
    } else {
      esp_wifi_clear_ap_list();
    }
    results_ = std::move(found);
    scanPhase_ = Done;
  }

  static WifiAuth classify(wifi_auth_mode_t mode) {
    switch (mode) {
      case WIFI_AUTH_OPEN:
        return WifiAuth::Open;
      case WIFI_AUTH_WPA2_PSK:
      case WIFI_AUTH_WPA_WPA2_PSK:
      case WIFI_AUTH_WPA2_WPA3_PSK:
        return WifiAuth::Wpa2Personal;
      default:  // WEP, WPA1 only, enterprise, WPA3 only, OWE, WAPI
        return WifiAuth::Unsupported;
    }
  }

  void do_join(const Command& c) {
    if (!ready_) return;
    if (scanPhase_.load() == Running) {  // a join ends a scan in progress
      stop_scan();
      scanPhase_ = Failed;
    }
    gen_ = c.gen;
    linkUp_ = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {  // still associated: drop it and wait a moment
      esp_wifi_disconnect();
      for (int i = 0; i < 20 && esp_wifi_sta_get_ap_info(&ap) == ESP_OK; ++i) vTaskDelay(pdMS_TO_TICKS(30));
    }
    wifi_config_t conf = {};
    strlcpy(reinterpret_cast<char*>(conf.sta.ssid), c.ssid, sizeof(conf.sta.ssid));
    const bool open = c.password[0] == '\0';
    if (!open) strlcpy(reinterpret_cast<char*>(conf.sta.password), c.password, sizeof(conf.sta.password));
    conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    conf.sta.threshold.rssi = -127;
    conf.sta.threshold.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    memset(&conf, 0, sizeof(conf));
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
      Serial.printf("[wifi] connect request failed: %s\n", esp_err_to_name(err));
      DriverEvent e = {};
      e.type = DriverEvent::Disconnected;
      e.gen = c.gen;
      e.reason = 1;  // unspecified: the controller retries inside its window
      xQueueSend(events_, &e, 0);
    }
  }

  // ---- ESP-IDF event handlers (system event task, core 0, 2 KB stack: keep tiny) ----

  static void on_wifi_event(void* arg, esp_event_base_t, int32_t id, void* data) {
    EspRadio* self = static_cast<EspRadio*>(arg);
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      self->linkUp_ = false;
      DriverEvent e = {};
      e.type = DriverEvent::Disconnected;
      e.gen = self->gen_.load();
      e.reason = data ? static_cast<wifi_event_sta_disconnected_t*>(data)->reason : 0;
      xQueueSend(self->events_, &e, 0);
    } else if (id == WIFI_EVENT_SCAN_DONE) {
      Command c = {};
      c.type = Command::ScanDone;
      xQueueSend(self->commands_, &c, 0);
    }
  }

  static void on_ip_event(void* arg, esp_event_base_t, int32_t id, void* data) {
    EspRadio* self = static_cast<EspRadio*>(arg);
    if (id == IP_EVENT_STA_LOST_IP) {
      self->linkUp_ = false;
      return;
    }
    if (id != IP_EVENT_STA_GOT_IP || !data) return;
    DriverEvent e = {};
    e.type = DriverEvent::GotIp;
    e.gen = self->gen_.load();
    const uint32_t addr = static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr;  // network order, first octet lowest
    char* p = e.ip;
    for (int i = 0; i < 4; ++i) {
      unsigned v = (addr >> (8 * i)) & 0xFF;
      if (v >= 100) *p++ = static_cast<char>('0' + v / 100);
      if (v >= 10) *p++ = static_cast<char>('0' + (v / 10) % 10);
      *p++ = static_cast<char>('0' + v % 10);
      if (i < 3) *p++ = '.';
    }
    *p = '\0';
    wifi_ap_record_t ap;
    self->rssi_ = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;  // at once, not after 2 s
    self->linkUp_ = true;
    xQueueSend(self->events_, &e, 0);
  }

  QueueHandle_t commands_ = nullptr;
  QueueHandle_t events_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> ready_{false};
  std::atomic<bool> linkUp_{false};
  std::atomic<int> rssi_{0};
  std::atomic<uint32_t> gen_{0};
  std::atomic<int> scanPhase_{Idle};
  std::vector<WifiScanEntry> results_;  // written by the worker before phase Done, read after
};

// ---------------------------------------------------------------- the service

struct EspWifiService::Impl {
  Impl()
      : controller(radio, blob, [] { return now_ms(); },
                   [](const std::string& line) { Serial.println(line.c_str()); }) {}
  EspRadio radio;
  NvsWifiBlob blob;
  WifiController controller;
  bool started = false;
};

EspWifiService::EspWifiService() : impl_(new (std::nothrow) Impl) {
  if (!impl_) {
    Serial.println("[wifi] ERROR: out of memory for the Wi-Fi service");
    return;
  }
  if (!prefs_key_valid(kNvsKey) || strlen(kNvsNamespace) > 15) {
    Serial.println("[wifi] ERROR: the NVS names break the key rules");
  }
}

EspWifiService::~EspWifiService() = default;

void EspWifiService::load() {
  if (impl_) impl_->controller.load();
}

bool EspWifiService::start() {
  if (!impl_ || impl_->started) return impl_ && impl_->started;
  impl_->started = impl_->radio.begin();
  return impl_->started;
}

void EspWifiService::pump() {
  if (!impl_) return;
  DriverEvent e;
  int guard = kEventQueueLen;  // at most one queue's worth per call
  while (guard-- > 0 && impl_->radio.pop_event(e)) {
    if (e.type == DriverEvent::GotIp) {
      impl_->controller.on_ip(e.gen, e.ip);
    } else {
      impl_->controller.on_disconnected(e.gen, e.reason);
    }
  }
  impl_->controller.tick();
}

void EspWifiService::scan(std::function<void(std::vector<WifiNetwork>)> done) {
  if (impl_) {
    impl_->controller.scan(std::move(done));
  } else if (done) {
    done({});
  }
}

void EspWifiService::connect(const std::string& ssid, const std::string& password,
                             std::function<void(bool ok)> done) {
  if (impl_) {
    impl_->controller.connect(ssid, password, std::move(done));
  } else if (done) {
    done(false);
  }
}

void EspWifiService::connect_saved(const std::string& ssid, std::function<void(bool ok)> done) {
  if (impl_) {
    impl_->controller.connect_saved(ssid, std::move(done));
  } else if (done) {
    done(false);
  }
}

std::vector<std::string> EspWifiService::saved() {
  return impl_ ? impl_->controller.saved() : std::vector<std::string>();
}

void EspWifiService::forget(const std::string& ssid) {
  if (impl_) impl_->controller.forget(ssid);
}

WifiStatus EspWifiService::status() { return impl_ ? impl_->controller.status() : WifiStatus(); }

void EspWifiService::wipe() {
  if (impl_) impl_->controller.wipe();
}

void EspWifiService::print_state() {
  if (!impl_) {
    Serial.println("[wifi] service not available");
    return;
  }
  Serial.printf("[wifi] %s | internal free %u (min %u) psram free %u | worker stack free %u B\n",
                impl_->controller.describe().c_str(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)worker_stack_free_bytes());
}

bool EspWifiService::link_up() const { return impl_ && impl_->radio.link_up(); }

size_t EspWifiService::worker_stack_free_bytes() const { return impl_ ? impl_->radio.stack_free_bytes() : 0; }

#if APP_DIAG
void EspWifiService::diag_print_scan() {
  if (!impl_) return;
  scan([](std::vector<WifiNetwork> nets) {
    Serial.printf("[wifi-diag] scan found %u network(s)\n", (unsigned)nets.size());
    for (const WifiNetwork& n : nets) {
      Serial.printf("[wifi-diag]   %-34s %4d dBm %s\n", wifi_printable(n.ssid).c_str(), n.rssi,
                    n.open ? "open" : "secured");
    }
  });
}

// Joins the most recently saved network (the user's own) with a deliberately
// wrong, throw-away key to prove the failure path. The failed attempt saves
// nothing and replaces nothing; the auto-join then rejoins the network.
void EspWifiService::diag_wrong_password_test() {
  const std::vector<std::string> list = saved();
  if (list.empty()) {
    Serial.println("[wifi-diag] no saved network to test against (save one first)");
    return;
  }
  const int64_t t0 = now_ms();
  Serial.printf("[wifi-diag] joining %s with a deliberately wrong throw-away credential\n",
                wifi_printable(list.front()).c_str());
  connect(list.front(), "wrong-test-phrase-1", [this, t0](bool ok) {
    Serial.printf("[wifi-diag] result: %s after %lld ms; saved networks now %u\n", ok ? "JOINED (unexpected)" : "failed",
                  (long long)(now_ms() - t0), (unsigned)saved().size());
  });
}

// Joins a network that does not exist: the attempt runs into the 15 s timeout.
void EspWifiService::diag_timeout_test() {
  const int64_t t0 = now_ms();
  Serial.println("[wifi-diag] joining a network that is not there (expect the 15 s timeout)");
  connect("soccer-diag-no-such-net", "throw-away-phrase-2", [this, t0](bool ok) {
    Serial.printf("[wifi-diag] result: %s after %lld ms; saved networks now %u\n", ok ? "JOINED (unexpected)" : "failed",
                  (long long)(now_ms() - t0), (unsigned)saved().size());
  });
}
#endif
