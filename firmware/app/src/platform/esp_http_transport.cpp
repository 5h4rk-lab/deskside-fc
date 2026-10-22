#include "esp_http_transport.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/platform.h>
#include <string.h>

#include <new>
#include <utility>

namespace {

constexpr UBaseType_t kWorkerPriority = 2;  // below the Wi-Fi worker (3), on core 0
constexpr BaseType_t kWorkerCore = 0;
constexpr size_t kScratchBytes = 4096;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

// The part of a URL that may be logged: the path, without scheme, host or query.
std::string path_of(const std::string& url) {
  const size_t hostStart = url.find("://");
  size_t p = url.find('/', hostStart == std::string::npos ? 0 : hostStart + 3);
  if (p == std::string::npos) return "/";
  const size_t q = url.find('?', p);
  return url.substr(p, q == std::string::npos ? std::string::npos : q - p);
}

void wipe(std::string& s) {
  if (!s.empty()) memset(&s[0], 0, s.size());
  s.clear();
  s.shrink_to_fit();
}

void* tls_calloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void tls_free(void* p) { heap_caps_free(p); }

}  // namespace

void tls_use_psram() { mbedtls_platform_set_calloc_free(tls_calloc, tls_free); }

struct EspHttpTransport::Job {
  uint32_t id = 0;
  std::string url;
  std::string token;
  int timeoutMs = 0;
  HttpResult result;
  const char* why = nullptr;  // transport error reason (static text) for the log
  esp_err_t err = ESP_OK;
  uint32_t ms = 0;
  uint32_t handshakeMs = 0;
};

EspHttpTransport::EspHttpTransport(const char* name, size_t capBytes, bool (*ready)())
    : name_(name), cap_(capBytes), ready_(ready) {}

bool EspHttpTransport::start() {
  if (task_ != nullptr) return true;
  todo_ = xQueueCreate(kQueueLength, sizeof(Job*));
  done_ = xQueueCreate(kQueueLength, sizeof(Job*));
  scratch_ = static_cast<char*>(heap_caps_malloc(kScratchBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (todo_ == nullptr || done_ == nullptr || scratch_ == nullptr ||
      xTaskCreatePinnedToCore(&EspHttpTransport::task_main, "http_worker", kWorkerStackBytes, this, kWorkerPriority,
                              &task_, kWorkerCore) != pdPASS) {
    Serial.printf("[http] %s: cannot start the worker (out of memory); requests are refused\n", name_);
    if (todo_) vQueueDelete(todo_);
    if (done_) vQueueDelete(done_);
    heap_caps_free(scratch_);
    todo_ = done_ = nullptr;
    scratch_ = nullptr;
    task_ = nullptr;
    return false;
  }
  return true;
}

void EspHttpTransport::refuse(uint32_t id, const char* why) { refusedNow_.emplace_back(id, why); }

void EspHttpTransport::get(const std::string& url, const std::string& token, int timeoutMs,
                           std::function<void(HttpResult)> done) {
  const uint32_t id = ++nextId_;
  pending_[id] = std::move(done);
  if (task_ == nullptr) return refuse(id, "worker not running");
  if (url.compare(0, 8, "https://") != 0) return refuse(id, "not an https URL");
  if (ready_ != nullptr && !ready_()) return refuse(id, "no network or no valid clock yet");
  Job* job = new (std::nothrow) Job();
  if (job == nullptr) return refuse(id, "out of memory");
  job->id = id;
  job->url = url;
  job->token = token;
  job->timeoutMs = timeoutMs > 0 ? timeoutMs : 8000;
  if (xQueueSend(todo_, &job, 0) != pdTRUE) {
    wipe(job->token);
    delete job;
    return refuse(id, "queue full");
  }
}

void EspHttpTransport::poll() {
  // Refusals first: they were asked before anything the worker finishes now.
  if (!refusedNow_.empty()) {
    std::vector<std::pair<uint32_t, const char*>> refused;
    refused.swap(refusedNow_);
    for (const auto& r : refused) {
      auto it = pending_.find(r.first);
      if (it == pending_.end()) continue;
      std::function<void(HttpResult)> done = std::move(it->second);
      pending_.erase(it);
      ++stats_.refused;
      HttpResult res;
      res.transportError = true;
      if (done) done(std::move(res));
    }
  }
  if (done_ == nullptr) return;
  Job* job = nullptr;
  while (xQueueReceive(done_, &job, 0) == pdTRUE) {
    if (job == nullptr) continue;
    const bool failed = job->result.transportError;
    if (failed) {
      ++stats_.failed;
      Serial.printf("[http] %s GET %s failed after %lu ms: %s%s%s\n", name_, path_of(job->url).c_str(),
                    (unsigned long)job->ms, job->why ? job->why : "error", job->err != ESP_OK ? " " : "",
                    job->err != ESP_OK ? esp_err_to_name(job->err) : "");
    } else {
      ++stats_.ok;
      if (job->result.status != 200) {
        Serial.printf("[http] %s GET %s answered %d after %lu ms\n", name_, path_of(job->url).c_str(),
                      job->result.status, (unsigned long)job->ms);
      }
      stats_.lastStatus = job->result.status;
      stats_.lastBytes = job->result.body.size();
    }
    stats_.lastMs = job->ms;
    if (job->handshakeMs) stats_.lastHandshakeMs = job->handshakeMs;
    auto it = pending_.find(job->id);
    std::function<void(HttpResult)> done;
    if (it != pending_.end()) {
      done = std::move(it->second);
      pending_.erase(it);
    }
    HttpResult res = std::move(job->result);
    delete job;  // the token was wiped by the worker
    if (done) done(std::move(res));
  }
}

EspHttpTransport::Stats EspHttpTransport::stats() const {
  Stats s = stats_;
  s.internalFreeInTls = tlsFree_.load();
  s.internalFreeInTlsMin = tlsFreeMin_.load();
  s.internalLargestInTlsMin = tlsLargestMin_.load();
  s.psramFreeInTlsMin = tlsPsramMin_.load();
  return s;
}

size_t EspHttpTransport::worker_stack_free_bytes() const {
  return task_ ? uxTaskGetStackHighWaterMark(task_) * sizeof(StackType_t) : 0;
}

void EspHttpTransport::task_main(void* arg) {
  auto* self = static_cast<EspHttpTransport*>(arg);
  for (;;) {
    Job* job = nullptr;
    if (xQueueReceive(self->todo_, &job, portMAX_DELAY) != pdTRUE || job == nullptr) continue;
    const int64_t t0 = nowMs();
    self->run(*job);
    job->ms = static_cast<uint32_t>(nowMs() - t0);
    wipe(job->token);
    if (job->result.transportError) {
      job->result.status = 0;
      job->result.body.clear();
      job->result.body.shrink_to_fit();
    }
    // The done queue has room for every request the todo queue held; wait if
    // the LVGL task is slow rather than lose an answer.
    while (xQueueSend(self->done_, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
    }
  }
}

void EspHttpTransport::run(Job& job) {
  HttpResult& r = job.result;
  r = HttpResult();
  auto fail = [&](const char* why, esp_err_t err = ESP_OK) {
    r.transportError = true;
    job.why = why;
    job.err = err;
  };
  if (ready_ != nullptr && !ready_()) return fail("no network or no valid clock");
  const int64_t deadline = nowMs() + job.timeoutMs;

  esp_http_client_config_t cfg = {};
  cfg.url = job.url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = job.timeoutMs;
  cfg.disable_auto_redirect = true;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  cfg.keep_alive_enable = false;
  cfg.addr_type = HTTP_ADDR_TYPE_INET;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return fail("client init failed (out of memory or bad URL)");
  esp_http_client_set_header(c, "x-token", job.token.c_str());
  esp_http_client_set_header(c, "Accept", "application/json");

  const int64_t tOpen = nowMs();
  esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(c);
    return fail("connect/DNS/TLS failed:", err);
  }
  job.handshakeMs = static_cast<uint32_t>(nowMs() - tOpen);
  // Memory with a TLS session open (both record buffers allocated).
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  tlsFree_.store(freeNow);
  if (freeNow < tlsFreeMin_.load()) tlsFreeMin_.store(freeNow);
  if (largest < tlsLargestMin_.load()) tlsLargestMin_.store(largest);
  if (psram < tlsPsramMin_.load()) tlsPsramMin_.store(psram);

  const int64_t len = esp_http_client_fetch_headers(c);
  const bool chunked = esp_http_client_is_chunked_response(c);
  if (len < 0) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return fail(len == -ESP_ERR_HTTP_EAGAIN ? "timed out waiting for the answer" : "reading the headers failed");
  }
  r.status = esp_http_client_get_status_code(c);
  if (static_cast<uint64_t>(len) > cap_) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return fail("body over the size cap");
  }
  if (len > 0) r.body.reserve(static_cast<size_t>(len));
  bool ok = true;
  for (;;) {
    if (nowMs() > deadline) {
      ok = false;
      fail("timed out reading the body");
      break;
    }
    const int n = esp_http_client_read(c, scratch_, kScratchBytes);
    if (n < 0) {
      ok = false;
      fail(n == -ESP_ERR_HTTP_EAGAIN ? "timed out reading the body" : "reading the body failed");
      break;
    }
    if (n == 0) break;
    if (r.body.size() + static_cast<size_t>(n) > cap_) {
      ok = false;
      fail("body over the size cap");
      break;
    }
    r.body.append(scratch_, static_cast<size_t>(n));
    if (len > 0 && r.body.size() >= static_cast<size_t>(len)) break;
  }
  if (ok) {
    if (len > 0 && r.body.size() != static_cast<size_t>(len)) {
      fail("body cut short");
    } else if (chunked && !esp_http_client_is_complete_data_received(c)) {
      fail("chunked body cut short");
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
}
