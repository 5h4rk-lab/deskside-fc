#include "request_deadline.h"

int RequestDeadline::remaining_ms(int64_t nowMs) const {
  if (nowMs < start_) return total_;  // the clock went backwards: the whole budget, no more
  const int64_t left = deadline_ - nowMs;
  return left > 0 ? static_cast<int>(left) : 0;
}

int RequestDeadline::step_timeout_ms(int64_t nowMs, int maxStepMs) const {
  const int left = remaining_ms(nowMs);
  if (left <= 0) return 0;
  return maxStepMs > 0 && maxStepMs < left ? maxStepMs : left;
}

BodyResult read_body(const std::function<int(char*, int)>& read, const std::function<int64_t()>& nowMs,
                     const RequestDeadline& deadline, size_t capBytes, int64_t contentLength, char* scratch,
                     int chunkBytes, std::string& body) {
  if (contentLength > 0 && static_cast<uint64_t>(contentLength) > capBytes) return BodyResult::OverCap;
  if (chunkBytes <= 0 || scratch == nullptr) return BodyResult::ReadError;
  if (contentLength > 0) body.reserve(static_cast<size_t>(contentLength));
  for (;;) {
    if (contentLength > 0 && body.size() >= static_cast<size_t>(contentLength)) return BodyResult::Complete;
    if (deadline.expired(nowMs())) return BodyResult::TimedOut;
    int ask = chunkBytes;
    if (contentLength > 0) {
      const int64_t left = contentLength - static_cast<int64_t>(body.size());
      if (left < ask) ask = static_cast<int>(left);
    }
    const int n = read(scratch, ask);
    if (n == kBodyReadAgain) continue;  // the deadline is checked again first
    if (n < 0) return deadline.expired(nowMs()) ? BodyResult::TimedOut : BodyResult::ReadError;
    if (n == 0) {
      return contentLength > 0 && body.size() != static_cast<size_t>(contentLength) ? BodyResult::CutShort
                                                                                     : BodyResult::Complete;
    }
    if (body.size() + static_cast<size_t>(n) > capBytes) return BodyResult::OverCap;
    body.append(scratch, static_cast<size_t>(n > ask ? ask : n));
  }
}
