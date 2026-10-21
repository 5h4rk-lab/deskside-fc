// SerialCommandSplitter: one-letter commands stay instant, whole-word commands
// never leak their letters into the one-letter handler.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "serial_commands.h"

namespace {

// Feeds `text` byte by byte at `gapMs` intervals, then polls `settleMs` later.
// Returns a readable trace: letters as themselves, words as <word>.
std::string run(const std::string& text, uint32_t gapMs = 1, uint32_t settleMs = 1000) {
  SerialCommandSplitter s({"wifi", "wipe-wifi"});
  std::vector<SerialCommandSplitter::Item> out;
  uint32_t t = 100;
  for (char c : text) {
    s.feed(c, t, out);
    t += gapMs;
  }
  s.poll(t + settleMs, out);
  std::string trace;
  for (const auto& it : out) {
    if (it.isWord) {
      trace += "<" + it.word + ">";
    } else if (it.byte == '\n') {
      trace += "\\n";
    } else if (it.byte == '\r') {
      trace += "\\r";
    } else {
      trace += it.byte;
    }
  }
  return trace;
}

}  // namespace

TEST_CASE("serial commands: one-letter commands run at once, in order") {
  SerialCommandSplitter s({"wifi", "wipe-wifi"});
  std::vector<SerialCommandSplitter::Item> out;
  s.feed('g', 10, out);
  REQUIRE(out.size() == 1);  // no waiting, no poll needed
  CHECK(out[0].byte == 'g');
  CHECK_FALSE(out[0].isWord);
  s.feed('s', 11, out);
  s.feed('1', 12, out);
  REQUIRE(out.size() == 3);
  CHECK(out[1].byte == 's');
  CHECK(out[2].byte == '1');
  CHECK(run("gdp") == "gdp");
  CHECK(run("s1") == "s1");
  CHECK(run("x0") == "x0");
  CHECK(run("=123456") == "=123456");
}

TEST_CASE("serial commands: whole words run at the line end and their letters never run") {
  CHECK(run("wipe-wifi\n") == "<wipe-wifi>");
  CHECK(run("wifi\n") == "<wifi>");
  CHECK(run("wipe-wifi\r\n") == "<wipe-wifi>\\n");  // the second line end is a bare one
  CHECK(run("wifi\r") == "<wifi>");
}

TEST_CASE("serial commands: a complete word without a line end runs after the hold time") {
  SerialCommandSplitter s({"wifi", "wipe-wifi"});
  std::vector<SerialCommandSplitter::Item> out;
  uint32_t t = 0;
  for (char c : std::string("wifi")) s.feed(c, t++, out);
  CHECK(out.empty());
  s.poll(t + 100, out);
  CHECK(out.empty());  // still within the hold time
  s.poll(t + 200, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].isWord);
  CHECK(out[0].word == "wifi");
}

TEST_CASE("serial commands: the lone w of the win command") {
  CHECK(run("w") == "w");  // after the hold time
  SerialCommandSplitter s({"wifi", "wipe-wifi"});
  std::vector<SerialCommandSplitter::Item> out;
  s.feed('w', 100, out);
  CHECK(out.empty());
  s.poll(120, out);
  CHECK(out.empty());
  s.poll(260, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].byte == 'w');
  CHECK(run("wg") == "wg");   // the next byte shows it is no word
  CHECK(run("w\n") == "w");   // Enter after a single letter: the letter, nothing else
}

TEST_CASE("serial commands: a half-typed or wrong word is dropped, not run letter by letter") {
  CHECK(run("wip") == "");
  CHECK(run("wipe-wif") == "");
  CHECK(run("wipe-wifig") == "");   // the e, p, r... never run
  CHECK(run("wifix") == "");
  CHECK(run("wipe\n") == "");       // not a word
  CHECK(run("wi\n") == "");
  CHECK(run("wipe-wifi\ng") == "<wipe-wifi>g");
  CHECK(run("wipe-wifig\ng") == "\\ng");  // the dropped word, then a bare line end, then g
}

TEST_CASE("serial commands: words in a row and mixed with letters") {
  CHECK(run("wifi\nwifi\n") == "<wifi><wifi>");
  CHECK(run("g wifi\n") == "g <wifi>");  // the space is a plain byte
  CHECK(run("wifi\nwipe-wifi\n") == "<wifi><wipe-wifi>");
}

TEST_CASE("serial commands: bare line ends pass through as bytes (ignored by the letter handler)") {
  CHECK(run("\n") == "\\n");
  CHECK(run("g\n") == "g\\n");
}

TEST_CASE("serial commands: a held prefix does not outlive a long pause between bytes") {
  SerialCommandSplitter s({"wifi", "wipe-wifi"});
  std::vector<SerialCommandSplitter::Item> out;
  s.feed('w', 0, out);
  s.feed('i', 100, out);
  s.poll(240, out);  // 140 ms since the last byte: still held
  CHECK(out.empty());
  s.poll(260, out);  // 160 ms: released; "wi" is no word and is dropped
  CHECK(out.empty());
  s.feed('g', 300, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].byte == 'g');
}

TEST_CASE("serial commands: no words configured means plain pass-through") {
  SerialCommandSplitter s({});
  std::vector<SerialCommandSplitter::Item> out;
  for (char c : std::string("wipe\n")) s.feed(c, 0, out);
  REQUIRE(out.size() == 5);
  CHECK(out[0].byte == 'w');
}
