#include "esp_http_transport.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/platform.h>
#include <string.h>
#include <sys/socket.h>

#include <new>
#include <utility>

namespace {

constexpr UBaseType_t kWorkerPriority = 2;  // below the Wi-Fi worker (3), on core 0
constexpr BaseType_t kWorkerCore = 0;
constexpr size_t kScratchBytes = 1024;
constexpr int kChunkBytes = 1024;   // body read size: the deadline is checked between reads
constexpr int kReadStepMs = 1000;   // client timeout of one read (at most the time left)

int64_t nowMs() { return esp_timer_get_time() / 1000; }

// "https://host:port" of a URL (what a kept connection is for).
std::string origin_of(const std::string& url) {
  const size_t hostStart = url.find("://");
  const size_t p = url.find('/', hostStart == std::string::npos ? 0 : hostStart + 3);
  return url.substr(0, p);
}

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

EspHttpTransport::EspHttpTransport(const char* name, size_t capBytes, const char* accept, bool (*ready)(),
                                   bool keepConnection)
    : name_(name), cap_(capBytes), accept_(accept), ready_(ready), keepConnection_(keepConnection) {}

bool EspHttpTransport::start() {
  if (task_ != nullptr) return true;
  char workerName[configMAX_TASK_NAME_LEN];
  snprintf(workerName, sizeof(workerName), "http_%s", name_);
  todo_ = xQueueCreate(kQueueLength, sizeof(Job*));
  done_ = xQueueCreate(kQueueLength, sizeof(Job*));
  scratch_ = static_cast<char*>(heap_caps_malloc(kScratchBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  guard_ = xSemaphoreCreateMutex();
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = &EspHttpTransport::watchdog_fired;
  timerArgs.arg = this;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "http_deadline";
  if (guard_ != nullptr && esp_timer_create(&timerArgs, &watchdog_) != ESP_OK) watchdog_ = nullptr;
  if (todo_ == nullptr || done_ == nullptr || scratch_ == nullptr || guard_ == nullptr || watchdog_ == nullptr ||
      xTaskCreatePinnedToCore(&EspHttpTransport::task_main, workerName, kWorkerStackBytes, this, kWorkerPriority,
                              &task_, kWorkerCore) != pdPASS) {
    Serial.printf("[http] %s: cannot start the worker (out of memory); requests are refused\n", name_);
    if (todo_) vQueueDelete(todo_);
    if (done_) vQueueDelete(done_);
    if (watchdog_) esp_timer_delete(watchdog_);
    if (guard_) vSemaphoreDelete(guard_);
    watchdog_ = nullptr;
    guard_ = nullptr;
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

void EspHttpTransport::poll(size_t maxResults) {
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
  size_t delivered = 0;
  while (delivered < maxResults && xQueueReceive(done_, &job, 0) == pdTRUE) {
    if (job == nullptr) continue;
    ++delivered;
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
    // A kept connection is closed after kIdleCloseMs without a request (it
    // holds a TLS session's memory, and the server drops idle ones anyway).
    const TickType_t wait = self->conn_ != nullptr ? pdMS_TO_TICKS(kIdleCloseMs) : portMAX_DELAY;
    if (xQueueReceive(self->todo_, &job, wait) != pdTRUE) {
      self->drop_connection();
      continue;
    }
    if (job == nullptr) continue;
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

void EspHttpTransport::watchdog_fired(void* arg) {
  // esp_timer task: end a step that is still blocked at the request's deadline by shutting
  // the socket down (lwIP is built full-duplex: this wakes a recv() blocked in the worker).
  auto* self = static_cast<EspHttpTransport*>(arg);
  if (xSemaphoreTake(self->guard_, portMAX_DELAY) != pdTRUE) return;
  if (self->watchedFd_ >= 0) {
    shutdown(self->watchedFd_, SHUT_RDWR);
    self->watchdogFired_ = true;
  }
  xSemaphoreGive(self->guard_);
}

void EspHttpTransport::watch(int fd, int ms) {
  xSemaphoreTake(guard_, portMAX_DELAY);
  watchedFd_ = fd;
  watchdogFired_ = false;
  xSemaphoreGive(guard_);
  if (fd >= 0 && ms > 0) esp_timer_start_once(watchdog_, static_cast<uint64_t>(ms) * 1000u);
}

bool EspHttpTransport::unwatch() {
  esp_timer_stop(watchdog_);  // harmless when it is not running
  // After this the callback can no longer touch the socket, so closing it (and the
  // descriptor being reused) is safe.
  xSemaphoreTake(guard_, portMAX_DELAY);
  watchedFd_ = -1;
  const bool fired = watchdogFired_;
  watchdogFired_ = false;
  xSemaphoreGive(guard_);
  return fired;
}

esp_http_client_handle_t EspHttpTransport::new_client(const Job& job, int timeoutMs) {
  esp_http_client_config_t cfg = {};
  cfg.url = job.url.c_str();
  cfg.method = HTTP_METHOD_GET;
  // Connect and handshake: esp-tls bounds both by this. The DNS lookup is
  // lwIP's own (not bounded by it).
  cfg.timeout_ms = timeoutMs;
  cfg.disable_auto_redirect = true;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  cfg.keep_alive_enable = false;  // TCP keep-alive probes; HTTP connection reuse is keepConnection_
  cfg.addr_type = HTTP_ADDR_TYPE_INET;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return nullptr;
  if (!job.token.empty()) esp_http_client_set_header(c, "x-token", job.token.c_str());
  esp_http_client_set_header(c, "Accept", accept_);
  return c;
}

void EspHttpTransport::drop_connection() {
  if (conn_ == nullptr) return;
  esp_http_client_close(conn_);
  esp_http_client_cleanup(conn_);
  conn_ = nullptr;
  connOrigin_.clear();
}

void EspHttpTransport::run(Job& job) {
  HttpResult& r = job.result;
  r = HttpResult();
  auto fail = [&](const char* why, esp_err_t err = ESP_OK) {
    r.transportError = true;
    job.why = why;
    job.err = err;
  };
  if (ready_ != nullptr && !ready_()) {
    drop_connection();
    return fail("no network or no valid clock");
  }
  // The whole request (connect, TLS, headers, body), including a second try on
  // a fresh connection, gets job.timeoutMs from here.
  const RequestDeadline deadline(nowMs(), job.timeoutMs);
  const std::string origin = origin_of(job.url);

  for (int attempt = 0; attempt < 2; ++attempt) {
    r = HttpResult();
    // A kept connection to the same host, not idle too long: send the next
    // request on it (no new TLS handshake). Otherwise a fresh client.
    esp_http_client_handle_t c = nullptr;
    bool reused = false;
    if (conn_ != nullptr && connOrigin_ == origin && nowMs() - connLastMs_ < kIdleCloseMs &&
        esp_http_client_set_url(conn_, job.url.c_str()) == ESP_OK) {
      c = conn_;
      reused = true;
      if (!job.token.empty()) {
        esp_http_client_set_header(c, "x-token", job.token.c_str());
      } else {
        esp_http_client_delete_header(c, "x-token");
      }
      esp_http_client_set_timeout_ms(c, deadline.remaining_ms(nowMs()));
      conn_ = nullptr;  // this attempt owns it until it is kept again
      connOrigin_.clear();
    } else {
      drop_connection();
      // Heap guard: a new TCP + TLS session needs internal RAM; wait for it
      // (another session may be ending) rather than squeeze the system.
      if (minInternalFree_ > 0 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < minInternalFree_) {
        ++heapWaits_;
        while (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < minInternalFree_) {
          if (deadline.remaining_ms(nowMs()) <= kHeapGuardStepMs) {
            ++heapRefusals_;
            return fail("low internal RAM: no new connection before the deadline");
          }
          vTaskDelay(pdMS_TO_TICKS(kHeapGuardStepMs));
        }
      }
      c = new_client(job, deadline.remaining_ms(nowMs()));
      if (c == nullptr) return fail("client init failed (out of memory or bad URL)");
    }
    // Ends this attempt's client: kept for the next request, or closed.
    auto release = [&](bool keep) {
      if (keep && keepConnection_) {
        conn_ = c;
        connOrigin_ = origin;
        connLastMs_ = nowMs();
      } else {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
      }
    };
    // A kept connection the server has closed in the meantime fails before any
    // answer: try once more on a fresh one (a GET can be repeated).
    auto retry = [&]() { return reused && attempt == 0 && !deadline.expired(nowMs()); };

    const int64_t tOpen = nowMs();
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
      release(false);
      if (retry()) {
        ++reconnects_;
        continue;
      }
      return fail(deadline.expired(nowMs()) ? "timed out connecting:" : "connect/DNS/TLS failed:", err);
    }
    if (reused) {
      ++reuses_;
    } else {
      job.handshakeMs = static_cast<uint32_t>(nowMs() - tOpen);
    }
    if (deadline.expired(nowMs())) {
      release(false);
      return fail("timed out (total deadline) after the handshake");
    }
    // Memory with a TLS session open (both record buffers allocated).
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    tlsFree_.store(freeNow);
    if (freeNow < tlsFreeMin_.load()) tlsFreeMin_.store(freeNow);
    if (largest < tlsLargestMin_.load()) tlsLargestMin_.store(largest);
    if (psram < tlsPsramMin_.load()) tlsPsramMin_.store(psram);

    // From here every blocking call is also ended from outside at the deadline.
    watch(esp_http_client_get_socket(c), deadline.remaining_ms(nowMs()));
    esp_http_client_set_timeout_ms(c, deadline.step_timeout_ms(nowMs(), job.timeoutMs));
    const int64_t len = esp_http_client_fetch_headers(c);
    const bool chunked = esp_http_client_is_chunked_response(c);
    if (len < 0 || deadline.expired(nowMs())) {
      const bool fired = unwatch();
      release(false);
      if (!fired && retry()) {
        ++reconnects_;
        continue;
      }
      return fail(deadline.expired(nowMs()) || len == -ESP_ERR_HTTP_EAGAIN
                      ? "timed out (total deadline) waiting for the answer"
                      : "reading the headers failed");
    }
    r.status = esp_http_client_get_status_code(c);
    if (r.status == 0 && retry()) {
      // Nothing parsed: the kept connection was closed under us.
      unwatch();
      release(false);
      ++reconnects_;
      continue;
    }
    const BodyResult body = read_body(
        [&](char* buf, int n) {
          const int step = deadline.step_timeout_ms(nowMs(), kReadStepMs);
          if (step <= 0) return -1;
          esp_http_client_set_timeout_ms(c, step);
          const int got = esp_http_client_read(c, buf, n);
          // A step that timed out with nothing read: try again while time is left.
          return got == -ESP_ERR_HTTP_EAGAIN ? kBodyReadAgain : got;
        },
        [] { return nowMs(); }, deadline, cap_, len > 0 ? len : 0, scratch_, kChunkBytes, r.body);
    const bool fired = unwatch();
    switch (body) {
      case BodyResult::Complete:
        if (chunked && !esp_http_client_is_complete_data_received(c)) fail("chunked body cut short");
        break;
      case BodyResult::TimedOut: fail("timed out (total deadline) reading the body"); break;
      case BodyResult::ReadError: fail(fired ? "timed out (total deadline) reading the body" : "reading the body failed"); break;
      case BodyResult::OverCap: fail("body over the size cap"); break;
      case BodyResult::CutShort: fail("body cut short"); break;
    }
    // Kept only after a whole answer the server lets us reuse.
    release(!r.transportError && !fired && esp_http_client_is_complete_data_received(c) &&
            esp_http_client_is_persistent_connection(c));
    return;
  }
}
