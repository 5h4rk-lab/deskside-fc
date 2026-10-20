// The real ImageProvider: downloads crests, news photos and player cutouts
// through the helper (GET <base>/api/img?... and /api/cutout?...) over an
// HttpTransport and decodes them with an ImageDecoder. Shared with the
// firmware: no stdio, no threads, no clock reads (the host passes the time to
// tick()).
//
// - Only helper image paths are fetched: a path must start with "/api/img?" or
//   "/api/cutout?", be at most kMaxPathLength long and contain no "..", no
//   "://", no whitespace and no control characters. Anything else is answered
//   with nullptr at once and never fetched (the device only talks to its
//   helper).
// - A cached image is answered at once. A miss for a path already queued or
//   in flight just adds a waiter: all waiters get the same ImagePtr, each
//   exactly once. Otherwise the request is queued: first in first out, at most
//   kMaxConcurrent transport requests outstanding, at most kMaxQueued waiting
//   (past that the oldest waiting request is answered with nullptr; it was
//   never tried, so it is not remembered as failed).
// - A 200 whose body decodes is cached (LRU by data_size) and answered. Any
//   other outcome answers nullptr and is remembered, so the path is not
//   requested again for a while: 5 minutes for 400, 404, other client errors
//   and undecodable bodies; 30 seconds for transport errors, 5xx, 408, 429 and
//   lost requests; 60 seconds for a 401, which also sets auth_rejected() until
//   the next success.
// - The host calls tick() regularly with a monotonic millisecond count: it
//   expires remembered failures and treats a request with no answer for
//   kLostMs as lost (its waiters get nullptr; a late answer is ignored).
// - Callbacks may run synchronously or later, and may call request() again. A
//   provider destroyed with requests outstanding drops them: their callers
//   are not called. The token is never logged or exposed.
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
  static constexpr int kTimeoutMs = 10000;
  static constexpr int64_t kLostMs = 30000;
  static constexpr int64_t kNegativeLongMs = 5 * 60 * 1000;  // 400, 404, decode failures
  static constexpr int64_t kNegativeShortMs = 30 * 1000;     // transport errors, 5xx, lost
  static constexpr int64_t kNegativeAuthMs = 60 * 1000;      // 401

  // `transport` and `decoder` must outlive the provider. A trailing '/' of
  // baseUrl is ignored.
  HttpImageProvider(HttpTransport& transport, ImageDecoder& decoder, std::string baseUrl,
                    std::string token, size_t cacheBytes = kDefaultCacheBytes);
  ~HttpImageProvider() override;
  HttpImageProvider(const HttpImageProvider&) = delete;
  HttpImageProvider& operator=(const HttpImageProvider&) = delete;

  void request(const std::string& path, std::function<void(ImagePtr)> done) override;

  // Advances the provider's clock (monotonic milliseconds, any origin).
  void tick(int64_t monotonicMs);

  // True when the helper answered 401 and nothing has succeeded since.
  bool auth_rejected() const { return authRejected_; }

  // Bytes held by the cache, and requests not answered yet (queued + in flight).
  size_t cached_bytes() const { return cache_.used(); }
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
  // Cleared on destruction, for answers delivered after it.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
