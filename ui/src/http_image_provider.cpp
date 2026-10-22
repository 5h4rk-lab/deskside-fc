#include "http_image_provider.h"

#include <iterator>
#include <string>
#include <utility>

namespace {

constexpr const char* kImgPrefix = "/api/img?";
constexpr const char* kCutoutPrefix = "/api/cutout?";

bool starts_with(const std::string& s, const char* prefix) {
  return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// The bytes a helper image path may contain: unreserved characters, '%' for
// escapes and the few delimiters the helper's query strings use. Everything
// else (bytes >= 0x80, controls, space, quotes, '\\', '#', '<', '>' ...) is refused.
bool url_safe(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  for (const char* p = "-._~%&=?/:+,"; *p != '\0'; ++p) {
    if (c == static_cast<unsigned char>(*p)) return true;
  }
  return false;
}

}  // namespace

HttpImageProvider::HttpImageProvider(HttpTransport& transport, ImageDecoder& decoder,
                                     std::string baseUrl, std::string token, size_t cacheBytes,
                                     size_t maxNegative, size_t maxQueued)
    : transport_(transport), decoder_(decoder), base_(std::move(baseUrl)), token_(std::move(token)),
      cache_(cacheBytes), maxNegative_(maxNegative > 0 ? maxNegative : 1),
      maxQueued_(maxQueued > 0 ? maxQueued : 1) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

HttpImageProvider::~HttpImageProvider() { *alive_ = false; }

bool HttpImageProvider::is_allowed_path(const std::string& path) {
  if (path.size() > kMaxPathLength) return false;
  if (!starts_with(path, kImgPrefix) && !starts_with(path, kCutoutPrefix)) return false;
  for (char ch : path) {
    if (!url_safe(static_cast<unsigned char>(ch))) return false;
  }
  if (path.find("..") != std::string::npos) return false;
  if (path.find("://") != std::string::npos) return false;
  return true;
}

void HttpImageProvider::answer(Waiters& waiters, const ImagePtr& image) {
  // Each waiter is called once; the list is ours alone, so a callback that
  // calls request() (or destroys the provider) cannot disturb it.
  for (std::function<void(ImagePtr)>& waiter : waiters) waiter(image);
}

bool HttpImageProvider::is_negative(const std::string& path) {
  auto it = negative_.find(path);
  if (it == negative_.end()) return false;
  if (it->second > nowMs_) return true;
  negative_.erase(it);
  return false;
}

void HttpImageProvider::remember_failure(const std::string& path, int64_t forMs) {
  if (negative_.size() >= maxNegative_ && negative_.find(path) == negative_.end()) {
    for (auto it = negative_.begin(); it != negative_.end();) {
      it = it->second <= nowMs_ ? negative_.erase(it) : std::next(it);
    }
    // Still full of live entries: forget the one that expires first.
    if (negative_.size() >= maxNegative_) {
      auto soonest = negative_.begin();
      for (auto it = negative_.begin(); it != negative_.end(); ++it) {
        if (it->second < soonest->second) soonest = it;
      }
      negative_.erase(soonest);
    }
  }
  negative_[path] = nowMs_ + forMs;
}

void HttpImageProvider::request(const std::string& path, std::function<void(ImagePtr)> done) {
  if (!is_allowed_path(path)) return done(nullptr);

  if (ImagePtr* hit = cache_.get(path)) {
    ImagePtr image = *hit;  // copy before the callback can touch the cache
    return done(std::move(image));
  }
  auto existing = entries_.find(path);
  if (existing != entries_.end()) {
    existing->second.waiters.push_back(std::move(done));
    return;
  }
  if (is_negative(path)) return done(nullptr);

  Entry entry;
  entry.waiters.push_back(std::move(done));
  entries_.emplace(path, std::move(entry));
  queue_.push_back(path);
  pump();

  // Too many waiting: the oldest ones give up (they were never attempted).
  std::vector<Waiters> dropped;
  while (queue_.size() > maxQueued_) {
    auto it = entries_.find(queue_.front());
    queue_.pop_front();
    if (it == entries_.end()) continue;
    dropped.push_back(std::move(it->second.waiters));
    entries_.erase(it);
  }
  for (Waiters& waiters : dropped) answer(waiters, nullptr);
}

void HttpImageProvider::set_online(bool online) {
  if (online == online_) return;
  online_ = online;
  if (online_) pump();
}

void HttpImageProvider::pump() {
  while (online_ && inFlight_ < kMaxConcurrent && !queue_.empty()) {
    std::string path = std::move(queue_.front());
    queue_.pop_front();
    auto it = entries_.find(path);
    if (it == entries_.end()) continue;
    Entry& entry = it->second;
    entry.inFlight = true;
    entry.id = ++nextId_;
    entry.startedMs = nowMs_;
    ++inFlight_;
    const uint32_t id = entry.id;
    std::shared_ptr<bool> alive = alive_;
    // `entry` must not be used past this call: an answer from inside get()
    // erases it.
    const std::string url = base_ + path;
    transport_.get(url, token_, kTimeoutMs,
                   [this, alive, path = std::move(path), id](HttpResult r) {
                     if (!*alive) return;
                     finish(path, id, std::move(r));
                   });
  }
}

void HttpImageProvider::finish(const std::string& path, uint32_t id, HttpResult result) {
  auto it = entries_.find(path);
  if (it == entries_.end() || !it->second.inFlight || it->second.id != id) return;  // lost or stale
  Waiters waiters = std::move(it->second.waiters);
  entries_.erase(it);
  --inFlight_;

  ImagePtr image;
  if (result.transportError) {
    remember_failure(path, kNegativeShortMs);
  } else if (result.status == 200) {
    image = decoder_.decode(result.body);
    if (image) {
      authRejected_ = false;
      cache_.put(path, image, image->data_size + entryOverhead_);
    } else {
      remember_failure(path, kNegativeLongMs);
    }
  } else if (result.status == 401) {
    authRejected_ = true;
    remember_failure(path, kNegativeAuthMs);
  } else if (result.status >= 500 || result.status == 408 || result.status == 429) {
    remember_failure(path, kNegativeShortMs);
  } else {
    remember_failure(path, kNegativeLongMs);
  }

  std::shared_ptr<bool> alive = alive_;
  result.body = std::string();  // release the bytes before running callers
  answer(waiters, image);
  if (*alive) pump();
}

void HttpImageProvider::tick(int64_t monotonicMs) {
  if (clockStarted_ && monotonicMs < nowMs_) {
    // The clock went backwards (a wrapped counter): requests in flight count
    // as just started, and no remembered failure outlasts its own window.
    for (auto& kv : entries_) {
      if (kv.second.inFlight && kv.second.startedMs > monotonicMs) kv.second.startedMs = monotonicMs;
    }
    for (auto& kv : negative_) {
      if (kv.second > monotonicMs + kNegativeLongMs) kv.second = monotonicMs + kNegativeLongMs;
    }
  }
  nowMs_ = monotonicMs;
  if (!clockStarted_) {
    // Requests made before the first tick were stamped with time 0.
    clockStarted_ = true;
    for (auto& kv : entries_) {
      if (kv.second.inFlight) kv.second.startedMs = nowMs_;
    }
  }
  for (auto it = negative_.begin(); it != negative_.end();) {
    it = it->second <= nowMs_ ? negative_.erase(it) : std::next(it);
  }

  std::vector<Waiters> lost;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.inFlight && nowMs_ - it->second.startedMs >= kLostMs) {
      lost.push_back(std::move(it->second.waiters));
      remember_failure(it->first, kNegativeShortMs);
      --inFlight_;
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  if (lost.empty()) return;
  std::shared_ptr<bool> alive = alive_;
  for (Waiters& waiters : lost) answer(waiters, nullptr);
  if (*alive) pump();
}
