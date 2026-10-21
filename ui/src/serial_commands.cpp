#include "serial_commands.h"

bool SerialCommandSplitter::is_prefix(const std::string& s) const {
  for (const std::string& w : words_) {
    if (w.compare(0, s.size(), s) == 0) return true;  // s is empty or starts w
  }
  return false;
}

bool SerialCommandSplitter::is_word(const std::string& s) const {
  for (const std::string& w : words_) {
    if (w == s) return true;
  }
  return false;
}

// The held input ends here (line end or timeout): a complete word runs, a
// lone letter is a one-letter command, anything longer is dropped.
void SerialCommandSplitter::release(std::vector<Item>& out) {
  if (pending_.empty()) return;
  if (is_word(pending_)) {
    Item it;
    it.isWord = true;
    it.word = pending_;
    out.push_back(it);
  } else if (pending_.size() == 1) {
    Item it;
    it.byte = pending_[0];
    out.push_back(it);
  }
  pending_.clear();
}

void SerialCommandSplitter::feed(char c, uint32_t nowMs, std::vector<Item>& out) {
  if (c == '\n' || c == '\r') {
    const bool had = !pending_.empty();
    release(out);
    if (!had) {  // a bare line end: the one-letter handler ignores it
      Item it;
      it.byte = c;
      out.push_back(it);
    }
    return;
  }
  const std::string candidate = pending_ + c;
  if (is_prefix(candidate)) {
    pending_ = candidate;
    lastByteMs_ = nowMs;
    return;
  }
  if (pending_.size() == 1) {
    // One held letter and then something that is no word: both are plain commands.
    Item first;
    first.byte = pending_[0];
    out.push_back(first);
    pending_.clear();
    feed(c, nowMs, out);
    return;
  }
  if (!pending_.empty()) {
    pending_.clear();  // a half-typed word that went wrong: not run letter by letter
    return;
  }
  Item it;
  it.byte = c;
  out.push_back(it);
}

void SerialCommandSplitter::poll(uint32_t nowMs, std::vector<Item>& out) {
  if (!pending_.empty() && static_cast<uint32_t>(nowMs - lastByteMs_) >= holdMs_) release(out);
}
