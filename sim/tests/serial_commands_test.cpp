// SerialCommandSplitter: one-letter commands stay instant, ":word" commands run
// only at Enter, and typing a word by hand can never run a one-letter command.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "serial_commands.h"

namespace {

std::string trace(const std::vector<SerialCommandSplitter::Item>& out) {
  std::string t;
  for (const auto& it : out) {
    if (it.isWord) {
      t += "<" + it.word + ">";
    } else if (it.byte == '\n') {
      t += "\\n";
    } else if (it.byte == '\r') {
      t += "\\r";
    } else {
      t += it.byte;
    }
  }
  return t;
}

// Feeds `text` byte by byte, `gapMs` apart.
std::string run(const std::string& text, uint32_t gapMs = 1) {
  SerialCommandSplitter s;
  std::vector<SerialCommandSplitter::Item> out;
  uint32_t t = 100;
  for (char c : text) {
    s.feed(c, t, out);
    t += gapMs;
  }
  return trace(out);
}

}  // namespace

TEST_CASE("serial commands: one-letter commands run at once, in order, without waiting") {
  SerialCommandSplitter s;
  std::vector<SerialCommandSplitter::Item> out;
  s.feed('g', 10, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].byte == 'g');
  CHECK_FALSE(out[0].isWord);
  s.feed('w', 11, out);  // the win command: no longer held back
  REQUIRE(out.size() == 2);
  CHECK(out[1].byte == 'w');
  CHECK(run("gdp") == "gdp");
  CHECK(run("s1") == "s1");
  CHECK(run("x0") == "x0");
  CHECK(run("=123456") == "=123456");
  CHECK(run("g\n") == "g\\n");
  CHECK(run("\n") == "\\n");
}

TEST_CASE("serial commands: a :word runs at the line end, its letters never run as commands") {
  CHECK(run(":wipe-wifi\n") == "<wipe-wifi>");
  CHECK(run(":wifi\n") == "<wifi>");
  CHECK(run(":erase-settings\n") == "<erase-settings>");
  CHECK(run(":restart\r\n") == "<restart>\\n");  // the second line end is a bare one
  CHECK(run(":wifi\r") == "<wifi>");
  CHECK(run("g:wifi\ng") == "g<wifi>g");
}

TEST_CASE("serial commands: typing a word slowly by hand never runs a letter command") {
  // 400 ms between keystrokes is slower than any hand, in a terminal monitor.
  CHECK(run(":wipe-wifi\n", 400) == "<wipe-wifi>");
  CHECK(run(":erase-settings\n", 900) == "<erase-settings>");
  // Half-typed words, Enter missing: nothing runs, however long it takes.
  CHECK(run(":wipe-wifi", 400) == "");
  CHECK(run(":e", 400) == "");
  CHECK(run(":erase-settings", 4000) == "");
}

TEST_CASE("serial commands: a word without Enter does not run, even complete") {
  CHECK(run(":wifi") == "");
  SerialCommandSplitter s;
  std::vector<SerialCommandSplitter::Item> out;
  for (char c : std::string(":restart")) s.feed(c, 0, out);
  s.poll(60000);
  CHECK(out.empty());
}

TEST_CASE("serial commands: an unfinished word is abandoned after the idle time, then letters work again") {
  SerialCommandSplitter s(5000);
  std::vector<SerialCommandSplitter::Item> out;
  for (char c : std::string(":wip")) s.feed(c, 100, out);
  s.feed('e', 4000, out);       // still within the idle time: swallowed
  s.feed('g', 9100, out);       // 5.1 s after the last byte: the word is gone, g is a command
  REQUIRE(out.size() == 1);
  CHECK(out[0].byte == 'g');
  // poll() alone also abandons it.
  SerialCommandSplitter s2(5000);
  std::vector<SerialCommandSplitter::Item> o2;
  s2.feed(':', 0, o2);
  s2.poll(5000);
  s2.feed('r', 5001, o2);
  REQUIRE(o2.size() == 1);
  CHECK(o2[0].byte == 'r');
}

TEST_CASE("serial commands: the stray colon swallows the rest of the line only") {
  CHECK(run(":gdp\ng") == "<gdp>g");   // handed to the caller as an unknown word
  CHECK(run(":\n") == "");             // an empty word is dropped
  CHECK(run(":\ng") == "g");
}

TEST_CASE("serial commands: an overlong word is dropped at its line end") {
  const std::string longWord(100, 'a');
  CHECK(run(":" + longWord + "\ng") == "g");
  CHECK(run(":" + std::string(32, 'a') + "\n") == "<" + std::string(32, 'a') + ">");
  CHECK(run(":" + std::string(33, 'a') + "\n") == "");
}

TEST_CASE("serial commands: words in a row") {
  CHECK(run(":wifi\n:wifi\n") == "<wifi><wifi>");
  CHECK(run(":wifi\n:restart\n") == "<wifi><restart>");
}
