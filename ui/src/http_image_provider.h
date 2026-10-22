// The real ImageProvider: downloads crests, news photos and player cutouts
// through the helper (GET <base>/api/img?... and /api/cutout?...) over an
// HttpTransport and decodes them with an ImageDecoder. Shared with the
// firmware: no stdio, no threads, no clock reads (the host passes the time to
// tick()).
//
// - Only helper image paths are fetched: a path must start with "/api/img?" or
//   "/api/cutout?", be at most kMaxPathLength long, contain no ".." and no
//   "://", and consist only of the bytes A-Z a-z 0-9 and - . _ ~ % & = ? / : + ,
//   (non-ASCII text must arrive percent-encoded). Anything else is answered
//   with nullptr at once and never fetched (the device only talks to its
//   helper).
// - A cached image is answered at once. A miss for a path already queued or
//   in flight just adds a waiter: all waiters get the same ImagePtr, each
//   exactly once. Otherwise the request is queued: first in first out, at most
//   kMaxConcurrent transport requests outstanding, at most `maxQueued`
//   (default kMaxQueued) waiting
//   (past that the oldest waiting request is answered with nullptr; it was
//   never tried, so it is not remembered as failed).
// - A 200 whose body decodes is cached (LRU by data_size) and answered. Any
//   other outcome answers nullptr and is remembered, so the path is not
//   requested again for a while: 5 minutes for 400, 404, other client errors,
//   undecodable bodies and 5xx answers to /api/cutout (a failing player
//   lookup); 30 seconds for transport errors, other 5xx, 408, 429 and lost
//   requests; 60 seconds for a 401, which also sets auth_rejected() until
//   the next success.
// - While the host says it is offline (set_online(false): no network or no
//   valid clock yet), nothing new is sent: requests wait in the queue (same
//   bound) and go out when it is online again, so a picture asked for at boot
//   is fetched once the network is up instead of failing at once. Requests
//   already in flight finish as usual. The provider starts online.
// - The host calls tick() regularly with a monotonic millisecond count: it
//   expires remembered failures and treats a request with no answer for
//   kLostMs as lost (its waiters get nullptr; a late answer is ignored). The
//   clock must be 64-bit and never wrap (on the ESP32 esp_timer_get_time() /
//   1000, not a 32-bit millis()); if it goes backwards anyway, requests in
//   flight count as started at that tick and remembered failures last at most
//   their longest window from it, so nothing freezes.
// - Callers' callbacks may run synchronously (cache hits, refused paths,
//   remembered failures) or later, and may call request() again. A provider
//   destroyed with requests outstanding drops them: their callers are not
//   called. Destroying the provider from inside a callback is supported only
//   for answers that arrive asynchronously from the transport (the transport
//   contract in http_transport.h forbids answering inside get()). The token is
//   never logged or exposed.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "http_transport.h"
#include "image_decoder.h"
#include "image_provider.h"
#include "lru_bytes.h"

class HttpImageProvider : public ImageProvider {
 public:
  static constexpr size_t kDefaultCacheBytes = 3 * 1024 * 1024;
  static constexpr size_t kMaxConcurrent = 2;
  static constexpr size_t kMaxQueued = 64;
  static constexpr size_t kMaxPathLength = 1024;
  static constexpr size_t kMaxNegative = 256;
  // The firmware's bound (internal RAM is scarce there; each entry is a path
  // string plus a map node): passed as `maxNegative`.
  static constexpr size_t kDeviceMaxNegative = 64;
  // The firmware's queue: one Matches list alone asks for more than 64
  // distinct crests at once, and a dropped request leaves a placeholder.
  static constexpr size_t kDeviceMaxQueued = 256;
  // Memory a cached picture costs on the device beyond its pixels (decoder and
  // heap block overhead, the cache's key and nodes, the descriptor), measured
  // on the board at about 1.5 KB: charged to the budget with each picture so
  // that many small crests cannot hold far more than the budget.
  static constexpr size_t kDeviceEntryOverheadBytes = 1536;
  static constexpr int kTimeoutMs = 10000;
  static constexpr int64_t kLostMs = 30000;
  static constexpr int64_t kNegativeLongMs = 5 * 60 * 1000;  // 400, 404, decode failures, cutout 5xx
  static constexpr int64_t kNegativeShortMs = 30 * 1000;     // transport errors, 5xx, lost
  static constexpr int64_t kNegativeAuthMs = 60 * 1000;      // 401

  // `transport` and `decoder` must outlive the provider. A trailing '/' of
  // baseUrl is ignored. `maxNegative` bounds the remembered failures (at least
  // 1; when full, expired entries go first, then the one that expires first).
  // `maxQueued` bounds the requests waiting to be sent (at least 1).
  HttpImageProvider(HttpTransport& transport, ImageDecoder& decoder, std::string baseUrl,
                    std::string token, size_t cacheBytes = kDefaultCacheBytes,
                    size_t maxNegative = kMaxNegative, size_t maxQueued = kMaxQueued);
  ~HttpImageProvider() override;
  HttpImageProvider(const HttpImageProvider&) = delete;
  HttpImageProvider& operator=(const HttpImageProvider&) = delete;

  void request(const std::string& path, std::function<void(ImagePtr)> done) override;

  // Advances the provider's clock (monotonic milliseconds, any origin).
  void tick(int64_t monotonicMs);

  // Bytes charged to the cache budget per picture on top of its pixels
  // (default 0). Affects pictures cached from then on.
  void set_entry_overhead_bytes(size_t bytes) { entryOverhead_ = bytes; }

  // Online (the default): queued requests are sent. Offline: they wait.
  void set_online(bool online);
  bool online() const { return online_; }

  // True when the helper answered 401 and nothing has succeeded since.
  bool auth_rejected() const { return authRejected_; }

  // Bytes charged to the cache (pixels plus the entry overhead), and requests
  // not answered yet (queued + in flight).
  size_t cached_bytes() const { return cache_.used(); }
  size_t cached_images() const { return cache_.count(); }
  size_t in_flight() const { return inFlight_; }
  size_t pending() const { return entries_.size(); }
  size_t negative_entries() const { return negative_.size(); }

  // The path guard on its own.
  static bool is_allowed_path(const std::string& path);

 private:
  using Waiters = std::vector<std::function<void(ImagePtr)>>;
  struct Entry {
    Waiters waiters;
    bool inFlight = false;
    uint32_t id = 0;  // of the transport request, to recognise late answers
    int64_t startedMs = 0;
  };

  bool is_negative(const std::string& path);
  void remember_failure(const std::string& path, int64_t forMs);
  void pump();
  void finish(const std::string& path, uint32_t id, HttpResult result);
  static void answer(Waiters& waiters, const ImagePtr& image);

  HttpTransport& transport_;
  ImageDecoder& decoder_;
  std::string base_;
  std::string token_;
  LruBytes<ImagePtr> cache_;
  std::unordered_map<std::string, Entry> entries_;  // queued and in flight
  std::deque<std::string> queue_;                   // the queued ones, oldest first
  std::unordered_map<std::string, int64_t> negative_;  // path -> time it may be tried again
  size_t inFlight_ = 0;
  uint32_t nextId_ = 0;
  int64_t nowMs_ = 0;
  bool clockStarted_ = false;
  bool authRejected_ = false;
  bool online_ = true;
  size_t entryOverhead_ = 0;
  size_t maxNegative_ = kMaxNegative;
  size_t maxQueued_ = kMaxQueued;
  // Cleared on destruction, for answers delivered after it.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
