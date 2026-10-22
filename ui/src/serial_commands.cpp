#include "serial_commands.h"

void SerialCommandSplitter::feed(char c, uint32_t nowMs, std::vector<Item>& out) {
  poll(nowMs);  // a word left unfinished long ago is gone before this byte counts
  if (discarding_) {
    lastByteMs_ = nowMs;
    if (c == '\n' || c == '\r') discarding_ = false;  // the line ends: letters work again
    return;
  }
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
  const bool idle = static_cast<uint32_t>(nowMs - lastByteMs_) >= abandonMs_;
  if (inWord_ && idle) {
    // Dropped; what may still come of its line is swallowed, for up to
    // another abandonMs of silence counted from now.
    inWord_ = false;
    tooLong_ = false;
    word_.clear();
    discarding_ = true;
    lastByteMs_ = nowMs;
  } else if (discarding_ && idle) {
    discarding_ = false;
  }
}

void split_word(const std::string& word, std::string& name, std::string& value) {
  const size_t space = word.find(' ');
  name = word.substr(0, space);
  value.clear();
  if (space == std::string::npos) return;
  const size_t start = word.find_first_not_of(' ', space);
  if (start != std::string::npos) value = word.substr(start);
}

namespace {
struct WordName {
  const char* name;
  ConsoleWord word;
  bool anyValue;  // takes a value (checked by the command itself)
};
constexpr WordName kWords[] = {
    {"help", ConsoleWord::Help, false},
    {"status", ConsoleWord::Status, false},
    {"heap", ConsoleWord::Heap, false},
    {"wifi", ConsoleWord::Wifi, false},
    {"wipe-wifi", ConsoleWord::WipeWifi, false},
    {"wipe-settings", ConsoleWord::WipeSettings, false},
    {"erase-settings", ConsoleWord::WipeSettings, false},
    {"reboot", ConsoleWord::Reboot, false},
    {"restart", ConsoleWord::Reboot, false},
    {"show-config", ConsoleWord::ShowConfig, false},
    {"net", ConsoleWord::Net, false},
    {"power", ConsoleWord::Power, false},
    {"ota", ConsoleWord::Ota, false},
    {"set-url", ConsoleWord::SetUrl, true},
    {"set-token", ConsoleWord::SetToken, true},
    {"sound", ConsoleWord::Sound, true},
    {"cal-battery", ConsoleWord::CalBattery, true},
};
}  // namespace

ConsoleWord parse_console_word(const std::string& name, const std::string& value) {
  if (name == "safe-mode") {
    if (value.empty()) return ConsoleWord::SafeModeShow;
    return value == "off" ? ConsoleWord::SafeModeOff : ConsoleWord::BadValue;
  }
  for (const WordName& w : kWords) {
    if (name != w.name) continue;
    if (!w.anyValue && !value.empty()) return ConsoleWord::BadValue;
    return w.word;
  }
  return ConsoleWord::Unknown;
}

const char* console_help_text() {
  return ":status :heap :wifi :reboot (or :restart) :wipe-wifi :wipe-settings (or :erase-settings) "
         ":safe-mode [off] :set-url <url> :set-token <token> :show-config :net :power :ota "
         ":sound [goal|win|kickoff|fulltime|low|critical|wifi|tone l|tone r|tone both|stop] [volume] "
         ":cal-battery [<mV>|clear] :help";
}
