// HttpTransport over the ESP-IDF HTTP client (esp_http_client + esp-tls +
// mbedTLS, prebuilt in the Arduino core): HTTPS GET with the server
// certificate checked against the ESP-IDF certificate bundle
// (esp_crt_bundle_attach), no redirects followed, headers x-token and Accept
// (application/json for data, image/png and image/jpeg for pictures).
// Two instances run: "data" (JSON, 4 MB cap) and "images" (512 KB cap), each
// with its own worker, so a picture download never queues ahead of a poll.
//
// Threads (contract of ui/src/http_transport.h):
// - get() and poll() run on the LVGL task only. get() never blocks and never
//   calls `done`; poll() delivers finished requests, so every `done` runs on
//   the LVGL task.
// - One worker task on core 0 (8 KB stack) per instance runs its requests one
//   at a time from a queue of 8.
// - Time: the caller's timeout is a TOTAL budget per request, counted from the
//   moment the worker starts it (ui/src/request_deadline.h). Connect and TLS
//   handshake get the whole budget as esp-tls's timeout; once connected,
//   headers and every body read get at most the time left (reads are 1 KB,
//   each with a client timeout of at most 1 s, and the deadline is checked
//   before each), and a one-shot timer shuts the socket down at the deadline,
//   which ends any call still blocked in it (a peer that trickles bytes). Not
//   covered: the DNS lookup before the connection (lwIP's own retries).
// - A request is refused (status 0, transportError, delivered by the next
//   poll(), no network use) while `ready()` is false (the host passes "Wi-Fi
//   has an address and the clock is valid"), when the URL is not https:// or
//   when the queue is full. `ready()` is asked again just before the request
//   starts.
// - Results: any HTTP status is a result. DNS, connection, TLS (including a
//   certificate that does not verify), timeouts, a truncated body and a body
//   over the size cap give status 0 and transportError.
// - The object lives for the whole program (the worker keeps a pointer to it).
//
// An empty token sends no x-token header (diagnostics against other hosts).
//
// Secrets: the token goes from get() into the request record, into the
// client's header list and onto the wire; it is never logged, and the record's
// copy is overwritten before it is freed. Log lines name the URL path only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <esp_timer.h>
#include "../app_diag.h"
#include "health.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "http_transport.h"
#include "request_deadline.h"

// esp_http_client.h's handle type (its header is included by the .cpp only:
// it does not mix with everything that includes this one).
typedef struct esp_http_client* esp_http_client_handle_t;

class EspHttpTransport : public HttpTransport {
 public:
  static constexpr size_t kJsonCapBytes = 4u * 1024u * 1024u;
  static constexpr size_t kImageCapBytes = 512u * 1024u;
  static constexpr int kQueueLength = 8;
  static constexpr uint32_t kWorkerStackBytes = 8192;
  static constexpr int64_t kIdleCloseMs = 15000;
  static constexpr uint32_t kWdtWaitMs = 2000;  // longest idle wait between two task watchdog feeds
  static constexpr uint32_t kDnsAllowanceMs = 30000;  // a connect is covered for its timeout plus this
  // Heap guard default for the images transport: a new connection (TCP + TLS
  // handshake, about 25 KB of internal RAM while it runs) is opened only with
  // at least this much internal RAM free; otherwise the worker waits in
  // kHeapGuardStepMs steps, within the request's total deadline.
  static constexpr size_t kImagesMinInternalFree = 80u * 1024u;
  static constexpr int kHeapGuardStepMs = 50;

  struct Stats {
    uint32_t ok = 0;           // answered with any HTTP status
    uint32_t failed = 0;       // transport errors (network, TLS, timeout, cap)
    uint32_t refused = 0;      // refused without network use (not ready, queue full, bad URL)
    int lastStatus = 0;
    uint32_t lastMs = 0;       // duration of the last request that went out
    uint32_t lastHandshakeMs = 0;
    size_t lastBytes = 0;
    // Internal RAM right after the TLS handshake (the high-water of a session)
    // and the lowest seen since start().
    size_t internalFreeInTls = 0;
    size_t internalFreeInTlsMin = SIZE_MAX;
    size_t internalLargestInTlsMin = SIZE_MAX;
    size_t psramFreeInTlsMin = SIZE_MAX;
  };

  // `name` labels log lines and the worker task ("data", "images"); `capBytes`
  // bounds a body (enforced while it streams); `accept` is the Accept header;
  // `ready` is called from the LVGL task and the worker (must be thread-safe).
  // `keepConnection`: keep the connection open after a complete answer the
  // server allows to reuse, and send the next request to the same host on it
  // (no new TLS handshake); closed after kIdleCloseMs idle, after any error
  // and when the network is not ready. A request that fails on a kept
  // connection before any answer is tried once more on a fresh one, within
  // the same total deadline.
  EspHttpTransport(const char* name, size_t capBytes, const char* accept, bool (*ready)(),
                   bool keepConnection = false);
  EspHttpTransport(const EspHttpTransport&) = delete;
  EspHttpTransport& operator=(const EspHttpTransport&) = delete;

  // Heap guard (0, the default, turns it off): before opening a NEW
  // connection (never before reusing a kept one) wait until internal RAM free
  // is at least `bytes`, or fail the request ("low internal RAM") at its
  // deadline. Call before start().
  void set_min_internal_free(size_t bytes) { minInternalFree_ = bytes; }

  // Creates the queues and the worker (core 0). False (and logged) when out of
  // memory: every request is then refused.
  bool start();

  void get(const std::string& url, const std::string& token, int timeoutMs,
           std::function<void(HttpResult)> done) override;

  // LVGL task: delivers refused requests and at most `maxResults` finished
  // ones (the rest wait for the next poll; the images transport passes 1 so
  // that at most one picture is decoded per host-loop tick).
  void poll(size_t maxResults = SIZE_MAX);

  // Copies of the counters (LVGL task; the worker's fields are atomics or
  // written before the result is queued).
  Stats stats() const;
  size_t worker_stack_free_bytes() const;
  size_t outstanding() const { return pending_.size(); }
  // Requests sent on a kept connection, and kept connections found closed
  // (the request then went out again on a fresh one). Worker counters.
  uint32_t reuses() const { return reuses_.load(); }
  uint32_t reconnects() const { return reconnects_.load(); }
  // Heap guard: requests that had to wait for internal RAM, and those that
  // failed because it never came back in time.
  uint32_t heap_waits() const { return heapWaits_.load(); }
  uint32_t heap_refusals() const { return heapRefusals_.load(); }

  // Asks the worker to close a kept connection now (before a software update:
  // its TLS session's memory is needed). Any task; the worker does it after
  // the request it may be running. has_kept_connection() tells when it is gone.
  void drop_kept_connection();
  bool has_kept_connection() const { return connOpen_.load(); }

#if APP_DIAG
  // Diagnostics builds only (a local update test server): also accept plain
  // http:// URLs. The release build does not have it (no symbol, no flag).
  void set_allow_plain_http(bool allow) { allowHttp_ = allow; }
#endif

 private:
  struct Job;
  static void task_main(void* arg);
  void run(Job& job);
  esp_http_client_handle_t new_client(const Job& job, int timeoutMs);
  void drop_connection();
  void refuse(uint32_t id, const char* why);
  // Deadline watchdog (worker side): watch() arms it for the socket, unwatch()
  // disarms it before the socket is closed and says whether it fired.
  static void watchdog_fired(void* arg);
  void watch(int fd, int ms);
  bool unwatch();

  const char* name_;
  WdtUser wdt_;  // the worker's task watchdog user (platform/health.h)
  size_t cap_;
  const char* accept_;
  bool (*ready_)();
  QueueHandle_t todo_ = nullptr;
  QueueHandle_t done_ = nullptr;
  TaskHandle_t task_ = nullptr;
  uint32_t nextId_ = 0;
  std::map<uint32_t, std::function<void(HttpResult)>> pending_;  // LVGL task only
  std::vector<std::pair<uint32_t, const char*>> refusedNow_;     // LVGL task only
  Stats stats_;  // LVGL task side
  // Worker side, read by the LVGL task after the result is taken off the queue.
  std::atomic<size_t> tlsFree_{0}, tlsFreeMin_{SIZE_MAX}, tlsLargestMin_{SIZE_MAX}, tlsPsramMin_{SIZE_MAX};
  char* scratch_ = nullptr;  // worker read buffer (PSRAM)
  esp_timer_handle_t watchdog_ = nullptr;
  SemaphoreHandle_t guard_ = nullptr;  // watchedFd_ / watchdogFired_ between the worker and the timer
  int watchedFd_ = -1;
  bool watchdogFired_ = false;
  // Worker only: the kept connection (keepConnection_), its "https://host:port"
  // and when its last answer ended.
  bool keepConnection_ = false;
  esp_http_client_handle_t conn_ = nullptr;
  std::string connOrigin_;
  int64_t connLastMs_ = 0;
  std::atomic<uint32_t> reuses_{0}, reconnects_{0}, heapWaits_{0}, heapRefusals_{0};
  std::atomic<bool> connOpen_{false};  // conn_ != nullptr, readable from any task
#if APP_DIAG
  std::atomic<bool> allowHttp_{false};
#endif
  size_t minInternalFree_ = 0;
};

// Sends mbedTLS's allocations (TLS record buffers, handshake and certificate
// parsing) to PSRAM, internal RAM only when PSRAM is full. Call once at start,
// before any TLS or Wi-Fi use.
void tls_use_psram();
