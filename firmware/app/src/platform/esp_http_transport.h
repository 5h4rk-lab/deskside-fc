// HttpTransport over the ESP-IDF HTTP client (esp_http_client + esp-tls +
// mbedTLS, prebuilt in the Arduino core): HTTPS GET with the server
// certificate checked against the ESP-IDF certificate bundle
// (esp_crt_bundle_attach), no redirects followed, headers x-token and
// Accept: application/json.
//
// Threads (contract of ui/src/http_transport.h):
// - get() and poll() run on the LVGL task only. get() never blocks and never
//   calls `done`; poll() delivers finished requests, so every `done` runs on
//   the LVGL task.
// - One worker task on core 0 (8 KB stack) runs the requests one at a time
//   from a queue of 8. The whole request (DNS, TLS handshake, headers, body) is
//   bounded by the caller's timeout.
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

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "http_transport.h"

class EspHttpTransport : public HttpTransport {
 public:
  static constexpr size_t kJsonCapBytes = 4u * 1024u * 1024u;
  static constexpr size_t kImageCapBytes = 512u * 1024u;
  static constexpr int kQueueLength = 8;
  static constexpr uint32_t kWorkerStackBytes = 8192;

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

  // `name` labels log lines ("data", "images"); `capBytes` bounds a body;
  // `ready` is called from the LVGL task and the worker (must be thread-safe).
  EspHttpTransport(const char* name, size_t capBytes, bool (*ready)());
  EspHttpTransport(const EspHttpTransport&) = delete;
  EspHttpTransport& operator=(const EspHttpTransport&) = delete;

  // Creates the queues and the worker (core 0). False (and logged) when out of
  // memory: every request is then refused.
  bool start();

  void get(const std::string& url, const std::string& token, int timeoutMs,
           std::function<void(HttpResult)> done) override;

  // LVGL task: delivers finished and refused requests.
  void poll();

  // Copies of the counters (LVGL task; the worker's fields are atomics or
  // written before the result is queued).
  Stats stats() const;
  size_t worker_stack_free_bytes() const;
  size_t outstanding() const { return pending_.size(); }

 private:
  struct Job;
  static void task_main(void* arg);
  void run(Job& job);
  void refuse(uint32_t id, const char* why);

  const char* name_;
  size_t cap_;
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
};

// Sends mbedTLS's allocations (TLS record buffers, handshake and certificate
// parsing) to PSRAM, internal RAM only when PSRAM is full. Call once at start,
// before any TLS or Wi-Fi use.
void tls_use_psram();
