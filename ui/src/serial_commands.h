// Splits the serial console's input into the firmware's one-letter commands
// (acted on at once, no Enter needed) and whole-word commands (":wifi",
// ":wipe-wifi", ":erase-settings", ":restart"). Pure logic, no clock reads.
//
// A word starts with ':' (no one-letter command uses it) and runs only when
// the line is ended with Enter. While a word is being typed its letters are
// swallowed, so typing it by hand, at any speed, can never run the one-letter
// commands that share those letters. A word that is never finished (nothing
// typed for abandonMs) is dropped, and so is one that grows past kMaxWordLen.
// After a word is dropped for the idle time, the rest of its line is
// swallowed too (until the next CR/LF, or until nothing has come for another
// abandonMs): the tail of a token typed with a pause in it must never run as
// one-letter commands.
// Anything that erases or restarts must be a word: no one-letter command does.
// kMaxWordLen leaves room for the longest settings words (":set-url " plus a
// 200-character URL, ":set-token " plus a 128-character token).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

class SerialCommandSplitter {
 public:
  static constexpr char kMarker = ':';
  static constexpr size_t kMaxWordLen = 256;

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
  bool discarding_ = false;  // the rest of an abandoned word's line
  bool tooLong_ = false;
  std::string word_;
  uint32_t lastByteMs_ = 0;
};

// Splits a word into its name (up to the first space) and its value (after
// that space and any further leading spaces; the rest is kept as it is).
// Callers print only the name of a word they do not know: the value may be a
// secret.
void split_word(const std::string& word, std::string& name, std::string& value);

// The release console's words (a diagnostics build checks its own diag-* and
// ota-* words first). Pure: the caller acts on the result. Two names may mean
// one command: `:restart` and `:reboot`, `:erase-settings` and `:wipe-settings`.
enum class ConsoleWord : uint8_t {
  Unknown,       // not a word of this console (the caller says so without echoing it)
  BadValue,      // a known word with a value it does not take (never echoed: it may be a secret)
  Help,
  Status,        // uptime, reset reason, crash counter, safe mode, version, Wi-Fi, heap, stacks, battery, update trial
  Heap,          // the memory report
  Wifi,
  WipeWifi,      // forget every saved network, restart
  WipeSettings,  // erase the saved settings (not Wi-Fi, not the helper), restart
  Reboot,        // restart (a software restart: never counted as a crash)
  SafeModeShow,  // :safe-mode
  SafeModeOff,   // :safe-mode off: clear the crash counter, restart in normal mode
  SetUrl,
  SetToken,
  ShowConfig,
  Net,
  Power,
  Ota,
  Sound,         // takes any value (checked by its own parser)
  CalBattery,    // takes any value (parse_cal_command)
};
ConsoleWord parse_console_word(const std::string& name, const std::string& value);

// One line naming every release word, for :help (each word in it parses).
const char* console_help_text();
