// Splits the serial console's input into the firmware's one-letter commands
// (acted on at once, no Enter needed) and whole-word commands (":wifi",
// ":wipe-wifi", ":erase-settings", ":restart"). Pure logic, no clock reads.
//
// A word starts with ':' (no one-letter command uses it) and runs only when
// the line is ended with Enter. While a word is being typed its letters are
// swallowed, so typing it by hand, at any speed, can never run the one-letter
// commands that share those letters. A word that is never finished (nothing
// typed for abandonMs) is dropped, and so is one that grows past kMaxWordLen.
// Anything that erases or restarts must be a word: no one-letter command does.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

class SerialCommandSplitter {
 public:
  static constexpr char kMarker = ':';
  static constexpr size_t kMaxWordLen = 32;

  struct Item {
    bool isWord = false;
    char byte = 0;     // when !isWord
    std::string word;  // when isWord: the text after ':' (may be unknown to the caller)
  };

  explicit SerialCommandSplitter(uint32_t abandonMs = 5000) : abandonMs_(abandonMs) {}

  // One input byte at time `nowMs`. Appends what is ready to `out`.
  void feed(char c, uint32_t nowMs, std::vector<Item>& out);
  // Call regularly: drops a word nobody finished.
  void poll(uint32_t nowMs);

 private:
  uint32_t abandonMs_;
  bool inWord_ = false;
  bool tooLong_ = false;
  std::string word_;
  uint32_t lastByteMs_ = 0;
};
