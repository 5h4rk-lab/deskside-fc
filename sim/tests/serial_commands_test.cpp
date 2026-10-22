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

TEST_CASE("serial commands: an unfinished word is abandoned after the idle time, its line is swallowed, then letters work again") {
  SerialCommandSplitter s(5000);
  std::vector<SerialCommandSplitter::Item> out;
  for (char c : std::string(":wip")) s.feed(c, 100, out);
  s.feed('e', 4000, out);       // still within the idle time: swallowed
  s.feed('g', 9100, out);       // 5.1 s after the last byte: the word is gone, but its line is still swallowed
  CHECK(out.empty());
  s.feed('\n', 9200, out);      // the line ends (swallowed too)
  CHECK(out.empty());
  s.feed('g', 9300, out);       // now a command
  REQUIRE(out.size() == 1);
  CHECK(out[0].byte == 'g');
  // poll() alone abandons it; the swallowing ends after another idle time without input.
  SerialCommandSplitter s2(5000);
  std::vector<SerialCommandSplitter::Item> o2;
  s2.feed(':', 0, o2);
  s2.poll(5000);                // abandoned: swallowing from 5000
  s2.feed('r', 5001, o2);       // swallowed
  CHECK(o2.empty());
  s2.poll(10001);               // 5 s of silence since that byte: swallowing ends
  s2.feed('g', 10002, o2);
  REQUIRE(o2.size() == 1);
  CHECK(o2[0].byte == 'g');
  // A word inside the swallowed line does not start either; the next line's word does.
  SerialCommandSplitter s3(5000);
  std::vector<SerialCommandSplitter::Item> o3;
  s3.feed(':', 0, o3);
  for (char c : std::string("abc:wifi\n:wifi\n")) s3.feed(c, 6000, o3);
  REQUIRE(o3.size() == 1);
  CHECK(o3[0].isWord);
  CHECK(o3[0].word == "wifi");
}

TEST_CASE("serial commands: the stray colon swallows the rest of the line only") {
  CHECK(run(":gdp\ng") == "<gdp>g");   // handed to the caller as an unknown word
  CHECK(run(":\n") == "");             // an empty word is dropped
  CHECK(run(":\ng") == "g");
}

TEST_CASE("serial commands: an overlong word is dropped at its line end") {
  CHECK(SerialCommandSplitter::kMaxWordLen == 256);
  const std::string longWord(300, 'a');
  CHECK(run(":" + longWord + "\ng") == "g");
  CHECK(run(":" + std::string(256, 'a') + "\n") == "<" + std::string(256, 'a') + ">");
  CHECK(run(":" + std::string(257, 'a') + "\n") == "");
  // Its letters never ran either, however long it grew.
  CHECK(run(":" + std::string(5000, 'g') + "\n") == "");
}

TEST_CASE("serial commands: the longest settings words fit (URL 200, token 128 characters)") {
  const std::string url = "https://" + std::string(192, 'u');
  const std::string token(128, 't');
  CHECK(run(":set-url " + url + "\n") == "<set-url " + url + ">");
  CHECK(run(":set-token " + token + "\n") == "<set-token " + token + ">");
  // Typed by hand, a key every 400 ms: still one word, only at Enter.
  CHECK(run(":set-token " + token + "\n", 400) == "<set-token " + token + ">");
  // Half typed (no Enter): nothing runs, not even after a minute.
  SerialCommandSplitter s;
  std::vector<SerialCommandSplitter::Item> out;
  uint32_t t = 0;
  for (char c : ":set-token " + token) s.feed(c, t += 300, out);
  s.poll(t + 60000);
  CHECK(out.empty());
  // A pause of 5 s inside it abandons the word, and the rest of the line is
  // swallowed: the tail of a token never runs as one-letter commands
  // (t would save a favourite, W/Z/= join Wi-Fi or tap in the diag builds).
  SerialCommandSplitter s2(5000);
  std::vector<SerialCommandSplitter::Item> o2;
  for (char c : std::string(":set-token abcdefgh")) s2.feed(c, 10, o2);
  for (char c : std::string("tWZ=gdpk12345\n")) s2.feed(c, 6000, o2);  // after the pause
  CHECK(o2.empty());
  // Even typed slowly after the pause (each key within the idle time).
  SerialCommandSplitter s3(5000);
  std::vector<SerialCommandSplitter::Item> o3;
  for (char c : std::string(":set-token abcdefgh")) s3.feed(c, 10, o3);
  uint32_t t3 = 6000;
  for (char c : std::string("tWZgdp")) s3.feed(c, t3 += 3000, o3);
  CHECK(o3.empty());
  s3.feed('\r', t3 += 100, o3);
  s3.feed('g', t3 += 100, o3);
  REQUIRE(o3.size() == 1);
  CHECK(o3[0].byte == 'g');
}

TEST_CASE("serial commands: a word splits into its name and its value") {
  std::string name, value;
  split_word("set-url https://example.com", name, value);
  CHECK(name == "set-url");
  CHECK(value == "https://example.com");
  split_word("wifi", name, value);
  CHECK(name == "wifi");
  CHECK(value.empty());
  split_word("set-token   abc def ", name, value);  // only the first run of spaces separates
  CHECK(name == "set-token");
  CHECK(value == "abc def ");
  split_word("", name, value);
  CHECK(name.empty());
  CHECK(value.empty());
  split_word(" lead", name, value);
  CHECK(name.empty());
  CHECK(value == "lead");
}

TEST_CASE("serial commands: words in a row") {
  CHECK(run(":wifi\n:wifi\n") == "<wifi><wifi>");
  CHECK(run(":wifi\n:restart\n") == "<wifi><restart>");
}
