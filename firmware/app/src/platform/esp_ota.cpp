#include "esp_ota.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <string.h>

#include "version.h"

namespace {

constexpr const char* kNamespace = "ota";

int64_t nowMs() { return esp_timer_get_time() / 1000; }

// The trial record (NVS namespace "ota"). "" when there is none.
std::string read_trial() {
  nvs_handle_t h = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return "";
  char buf[64] = {};
  size_t len = sizeof(buf);
  const esp_err_t err = nvs_get_str(h, kTrialKey, buf, &len);
  nvs_close(h);
  return err == ESP_OK ? std::string(buf) : std::string();
}

bool write_trial(const std::string& value) {
  nvs_handle_t h = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, kTrialKey, value.c_str());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool erase_trial() {
  nvs_handle_t h = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_erase_key(h, kTrialKey);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

const char* state_name(int s) {
  switch (static_cast<esp_ota_img_states_t>(s)) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
  }
  return "unknown";
}

int image_state(const esp_partition_t* p) {
  esp_ota_img_states_t s;
  if (p == nullptr || esp_ota_get_state_partition(p, &s) != ESP_OK) return -1;
  return static_cast<int>(s);
}

Version running_version() {
  Version v;
  parse_version(kAppVersion, v);
  return v;
}

// The other app slot (the one the running image did not boot from).
const esp_partition_t* other_slot() { return esp_ota_get_next_update_partition(nullptr); }

void hex(const uint8_t* d, size_t n, char* out) {
  static const char* const k = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = k[d[i] >> 4];
    out[2 * i + 1] = k[d[i] & 15];
  }
  out[2 * n] = 0;
}

}  // namespace

// ---- start-up trial -------------------------------------------------------------

OtaBoot ota_boot() {
  OtaBoot b;
  const esp_partition_t* running = esp_ota_get_running_partition();
  b.runningSlot = running ? running->label : "?";
  b.running = running_version();
  b.imageState = image_state(running);
  const std::string stored = read_trial();
  const BootDecision d = decide_boot(stored, b.runningSlot, b.running);
  b.action = d.action;
  b.boots = d.record.boots;
  if (d.erase) erase_trial();
  if (d.write && !write_trial(encode_trial(d.record))) {
    Serial.println("[ota] WARNING: could not write the trial record");
  }
  if (d.action == BootAction::RollBack) {
    const esp_partition_t* back = other_slot();
    Serial.printf("[ota] %s (%s) started %d times without confirming: rolling back to %s\n", b.runningSlot.c_str(),
                  version_text(b.running).c_str(), d.record.boots, back ? back->label : "?");
    const esp_err_t err = back ? esp_ota_set_boot_partition(back) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
      Serial.flush();
      delay(50);
      esp_restart();
    }
    // The previous slot cannot boot: keep this one (better than nothing).
    Serial.printf("[ota] ERROR: cannot switch back (%s): staying on %s\n", esp_err_to_name(err),
                  b.runningSlot.c_str());
    erase_trial();
    b.action = BootAction::Normal;
  }
  b.onTrial = b.action == BootAction::Trial || b.imageState == static_cast<int>(ESP_OTA_IMG_PENDING_VERIFY);
  b.notice = d.notice;
  Serial.printf("[ota] running %s %s (%s)%s%s%s\n", b.runningSlot.c_str(), version_text(b.running).c_str(),
                state_name(b.imageState),
                b.action == BootAction::Trial ? ", ON TRIAL (start " : "",
                b.action == BootAction::Trial ? std::to_string(b.boots).c_str() : "",
                b.action == BootAction::Trial ? " of at most 2): confirms once healthy"
                : b.action == BootAction::RolledBack ? ", ROLLED BACK: the update did not start"
                : b.onTrial ? ", pending verification by the bootloader" : "");
  return b;
}

bool ota_confirm() {
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  const bool erased = erase_trial();
  Serial.printf("[ota] confirmed: %s (mark valid %s, trial record %s)\n",
                version_text(running_version()).c_str(), esp_err_to_name(err), erased ? "erased" : "NOT erased");
  return err == ESP_OK && erased;
}

void ota_print_state() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* other = other_slot();
  esp_app_desc_t otherDesc = {};
  const bool otherOk = other && esp_ota_get_partition_description(other, &otherDesc) == ESP_OK;
  const std::string trial = read_trial();
  Serial.printf("[ota] running %s %s (%s), boot slot %s, other %s %s (%s) | trial record %s | rollback: own trial%s\n",
                running ? running->label : "?", kAppVersion, state_name(image_state(running)),
                boot ? boot->label : "?", other ? other->label : "?",
                otherOk ? otherDesc.version : "(no image)", state_name(image_state(other)),
                trial.empty() ? "none" : trial.c_str(),
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
                " + bootloader (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)"
#else
                ""
#endif
  );
}

// ---- download ---------------------------------------------------------------------

bool EspOta::start(const UpdateInfo& info, const Version& running, bool allowHttp) {
  if (state_.load() == State::Running) return false;
  info_ = info;
  running_ = running;
#if APP_DIAG
  allowHttp_ = allowHttp;
#else
  (void)allowHttp;  // the release build downloads over https only (allowHttp_ is constexpr false)
#endif
  stats_ = Stats();
  inTask_ = false;
  percent_.store(0);
  error_.store(UpdateError::None);
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalFree + kStackBytes + kChunkBytes) {
    fail(UpdateError::LowMemory, "internal RAM below the floor before the start");
    return false;
  }
  state_.store(State::Running);
  // Internal-RAM stack (the default): flash writes must not run on a PSRAM stack.
  if (xTaskCreatePinnedToCore(&EspOta::task_main, "ota", kStackBytes, this, 2, &task_, 0) != pdPASS) {
    task_ = nullptr;
    fail(UpdateError::LowMemory, "cannot start the download task");
    return false;
  }
  return true;
}

// Every Stats field is written before the final state store (release): the
// LVGL task reads stats() only after it saw Done or Failed.
void EspOta::fail(UpdateError e, const char* why, int err) {
  if (inTask_) stats_.stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  stats_.why = why;
  stats_.espErr = err;
  error_.store(e);
  state_.store(State::Failed);
}

void EspOta::task_main(void* arg) {
  auto* self = static_cast<EspOta*>(arg);
  self->inTask_ = true;
  self->run();
  // Nothing of stats_ is written after run(): the state is final already.
  self->task_ = nullptr;
  vTaskDelete(nullptr);
}

void EspOta::run() {
  const int64_t t0 = nowMs();
  auto track = [this] {
    const size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (stats_.internalFreeMin == 0 || f < stats_.internalFreeMin) stats_.internalFreeMin = f;
    if (stats_.internalLargestMin == 0 || l < stats_.internalLargestMin) stats_.internalLargestMin = l;
  };
  track();
  const bool https = info_.url.compare(0, 8, "https://") == 0;
  if (!https && !(allowHttp_ && info_.url.compare(0, 7, "http://") == 0)) {
    return fail(UpdateError::WrongImage, "URL scheme not allowed");
  }
  const esp_partition_t* slot = other_slot();
  if (slot == nullptr || static_cast<uint32_t>(info_.sizeBytes) > slot->size) {
    return fail(UpdateError::SaveFailed, "no idle slot large enough");
  }
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buf == nullptr) return fail(UpdateError::LowMemory, "no buffer");

  esp_http_client_config_t cfg = {};
  cfg.url = info_.url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = kReadTimeoutMs;
  cfg.disable_auto_redirect = true;
  if (https) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  cfg.keep_alive_enable = false;
  cfg.addr_type = HTTP_ADDR_TYPE_INET;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) {
    heap_caps_free(buf);
    return fail(UpdateError::LowMemory, "client init failed");
  }
  esp_http_client_set_header(c, "Accept", "application/octet-stream");

  esp_ota_handle_t ota = 0;
  bool otaOpen = false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  // Every way out: close the session, drop the half-written image, free.
  auto finish = [&](bool ok) {
    if (otaOpen && !ok) esp_ota_abort(ota);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    mbedtls_sha256_free(&sha);
    heap_caps_free(buf);
    stats_.ms = static_cast<uint32_t>(nowMs() - t0);
  };

  const int64_t tOpen = nowMs();
  esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    finish(false);
    return fail(UpdateError::DownloadFailed, "connect/DNS/TLS failed", err);
  }
  stats_.handshakeMs = static_cast<uint32_t>(nowMs() - tOpen);
  track();
  const int64_t length = esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  if (status != 200) {
    finish(false);
    return fail(UpdateError::DownloadFailed, "the server did not answer 200");
  }
  if (length > 0 && length != info_.sizeBytes) {
    finish(false);
    return fail(UpdateError::Damaged, "Content-Length differs from the manifest size");
  }

  uint8_t head[kImageHeadBytes];
  size_t headLen = 0;
  uint32_t bytes = 0;
  int stalls = 0;
  const uint32_t size = static_cast<uint32_t>(info_.sizeBytes);
  while (bytes < size) {
    if (nowMs() - t0 > kTotalLimitMs) {
      finish(false);
      return fail(UpdateError::DownloadFailed, "took longer than the total limit");
    }
    const int want = static_cast<int>(size - bytes < kChunkBytes ? size - bytes : kChunkBytes);
    const int n = esp_http_client_read(c, reinterpret_cast<char*>(buf), want);
    if (n == -ESP_ERR_HTTP_EAGAIN) {  // nothing for kReadTimeoutMs
      if (++stalls > kMaxStalls) {
        finish(false);
        return fail(UpdateError::DownloadFailed, "stalled");
      }
      continue;
    }
    if (n < 0) {
      finish(false);
      return fail(UpdateError::DownloadFailed, "read failed", n);
    }
    if (n == 0) {  // the server closed the connection early
      finish(false);
      return fail(UpdateError::DownloadFailed, "connection closed before the end");
    }
    stalls = 0;
    // The head first: nothing is written before it is known to be ours.
    if (headLen < kImageHeadBytes) {
      const size_t take = kImageHeadBytes - headLen < static_cast<size_t>(n) ? kImageHeadBytes - headLen : n;
      memcpy(head + headLen, buf, take);
      headLen += take;
      if (headLen == kImageHeadBytes) {
        const ImageHead h = check_image_head(head, headLen, kFirmwareProjectName, info_.version);
        if (h != ImageHead::Ok) {
          finish(false);
          return fail(UpdateError::WrongImage, h == ImageHead::WrongProject ? "another project's image"
                                               : h == ImageHead::WrongChip  ? "built for another chip"
                                               : h == ImageHead::WrongVersion
                                                   ? "the image's version is not the announced one"
                                                   : "not an app image");
        }
        err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota);
        if (err != ESP_OK) {
          finish(false);
          return fail(UpdateError::SaveFailed, "esp_ota_begin failed", err);
        }
        otaOpen = true;
        // Everything held back so far (the head) goes in first.
        err = esp_ota_write(ota, head, headLen);
        if (err == ESP_OK && static_cast<size_t>(n) > take) err = esp_ota_write(ota, buf + take, n - take);
      } else {
        err = ESP_OK;
      }
    } else {
      err = esp_ota_write(ota, buf, n);
    }
    if (err != ESP_OK) {
      finish(false);
      return fail(UpdateError::SaveFailed, "esp_ota_write failed", err);
    }
    mbedtls_sha256_update(&sha, buf, n);
    bytes += n;
    stats_.bytes = bytes;
    percent_.store(static_cast<int>(static_cast<uint64_t>(bytes) * 100 / size));
    track();
  }
  if (!otaOpen) {  // smaller than the head: cannot be an image (the manifest forbids it anyway)
    finish(false);
    return fail(UpdateError::Damaged, "shorter than an image head");
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  char digestHex[65];
  hex(digest, sizeof(digest), digestHex);
  if (info_.sha256 != digestHex) {
    finish(false);
    return fail(UpdateError::Damaged, "SHA-256 mismatch");
  }
  err = esp_ota_end(ota);  // also checks the image's own structure and hash
  otaOpen = false;
  if (err != ESP_OK) {
    finish(false);
    return fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? UpdateError::Damaged : UpdateError::SaveFailed,
                "esp_ota_end refused the image", err);
  }
  TrialRecord r;
  r.slot = slot->label;
  r.boots = 0;
  r.version = info_.version;
  r.from = running_;
  if (!write_trial(encode_trial(r))) {
    finish(false);
    return fail(UpdateError::SaveFailed, "cannot write the trial record");
  }
  err = esp_ota_set_boot_partition(slot);
  if (err != ESP_OK) {
    erase_trial();
    finish(false);
    return fail(UpdateError::SaveFailed, "cannot switch the boot slot", err);
  }
  finish(true);
  stats_.stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  percent_.store(100);
  state_.store(State::Done);
}
