#include "serial_commands.h"

void SerialCommandSplitter::feed(char c, uint32_t nowMs, std::vector<Item>& out) {
  poll(nowMs);  // a word left unfinished long ago is gone before this byte counts
  if (inWord_) {
    lastByteMs_ = nowMs;
    if (c == '\n' || c == '\r') {
      if (!tooLong_ && !word_.empty()) {
        Item it;
        it.isWord = true;
        it.word = word_;
        out.push_back(it);
      }
      inWord_ = false;
      tooLong_ = false;
      word_.clear();
      return;
    }
    if (word_.size() >= kMaxWordLen) {
      tooLong_ = true;
    } else {
      word_.push_back(c);
    }
    return;
  }
  if (c == kMarker) {
    inWord_ = true;
    tooLong_ = false;
    word_.clear();
    lastByteMs_ = nowMs;
    return;
  }
  Item it;
  it.byte = c;
  out.push_back(it);
}

void SerialCommandSplitter::poll(uint32_t nowMs) {
  if (inWord_ && static_cast<uint32_t>(nowMs - lastByteMs_) >= abandonMs_) {
    inWord_ = false;
    tooLong_ = false;
    word_.clear();
  }
}
