// Splits the serial console's input into the firmware's one-letter commands
// (acted on at once, no Enter needed) and a few whole-word commands such as
// "wipe-wifi" (acted on at the end of the line). Pure logic, no clock reads.
//
// The letters of a word must never run as one-letter commands (the "e" in
// "wipe-wifi" erases the settings), so input that could still become a word
// is held back for a moment:
// - bytes that cannot begin a word are returned at once;
// - bytes that are a prefix of a word wait for the rest, up to holdMs;
// - a complete word runs at the line end (or after holdMs);
// - a lone held letter (the "w" of the win command) is returned as that
//   letter when the next byte shows it is no word, or after holdMs;
// - a half-typed word (two or more letters) that goes wrong is dropped, not
//   run letter by letter.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

class SerialCommandSplitter {
 public:
  struct Item {
    bool isWord = false;
    char byte = 0;     // when !isWord
    std::string word;  // when isWord
  };

  explicit SerialCommandSplitter(std::vector<std::string> words, uint32_t holdMs = 150)
      : words_(std::move(words)), holdMs_(holdMs) {}

  // One input byte at time `nowMs`. Appends what is ready to `out`.
  void feed(char c, uint32_t nowMs, std::vector<Item>& out);
  // Call regularly: releases held input after holdMs.
  void poll(uint32_t nowMs, std::vector<Item>& out);

 private:
  bool is_prefix(const std::string& s) const;
  bool is_word(const std::string& s) const;
  void release(std::vector<Item>& out);

  std::vector<std::string> words_;
  uint32_t holdMs_;
  std::string pending_;
  uint32_t lastByteMs_ = 0;
};
