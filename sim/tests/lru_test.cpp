#include <doctest/doctest.h>

#include <string>

#include "lru_bytes.h"

TEST_CASE("LruBytes evicts the least recently used entry to fit a new one") {
  LruBytes<int> c(100);
  c.put("A", 1, 60);
  c.put("B", 2, 60);
  CHECK(c.get("A") == nullptr);
  REQUIRE(c.get("B") != nullptr);
  CHECK(*c.get("B") == 2);
  CHECK(c.used() == 60);
}

TEST_CASE("LruBytes get marks an entry most recently used") {
  LruBytes<int> c(100);
  c.put("A", 1, 40);
  c.put("B", 2, 40);
  REQUIRE(c.get("A") != nullptr);  // A is now newer than B
  c.put("C", 3, 40);               // needs 20 more bytes: evicts B, not A
  CHECK(c.get("B") == nullptr);
  CHECK(c.get("A") != nullptr);
  CHECK(c.get("C") != nullptr);
  CHECK(c.used() == 80);
}

TEST_CASE("LruBytes evicts several entries when one item needs the room") {
  LruBytes<int> c(100);
  c.put("A", 1, 30);
  c.put("B", 2, 30);
  c.put("C", 3, 30);
  c.put("D", 4, 80);
  CHECK(c.get("A") == nullptr);
  CHECK(c.get("B") == nullptr);
  CHECK(c.get("C") == nullptr);
  CHECK(c.get("D") != nullptr);
  CHECK(c.used() == 80);
}

TEST_CASE("LruBytes does not store an item larger than the whole budget") {
  LruBytes<int> c(100);
  c.put("A", 1, 50);
  c.put("huge", 2, 101);
  CHECK(c.get("huge") == nullptr);
  CHECK(c.used() == 50);
  CHECK(c.get("A") != nullptr);  // nothing was evicted for it either
}

TEST_CASE("LruBytes stores an item exactly as large as the budget") {
  LruBytes<int> c(100);
  c.put("A", 1, 10);
  c.put("full", 2, 100);
  CHECK(c.get("A") == nullptr);
  CHECK(c.get("full") != nullptr);
  CHECK(c.used() == 100);
}

TEST_CASE("LruBytes replacing a key updates the value and used()") {
  LruBytes<std::string> c(100);
  c.put("A", "one", 30);
  c.put("B", "two", 30);
  c.put("A", "uno", 50);
  REQUIRE(c.get("A") != nullptr);
  CHECK(*c.get("A") == "uno");
  CHECK(c.used() == 80);
  c.put("A", "tiny", 10);
  CHECK(c.used() == 40);
  CHECK(*c.get("A") == "tiny");
  CHECK(*c.get("B") == "two");
}

TEST_CASE("LruBytes replacing a key counts as a fresh use") {
  LruBytes<int> c(100);
  c.put("A", 1, 40);
  c.put("B", 2, 40);
  c.put("A", 3, 40);  // A is newest now
  c.put("C", 4, 40);  // evicts B
  CHECK(c.get("B") == nullptr);
  REQUIRE(c.get("A") != nullptr);
  CHECK(*c.get("A") == 3);
}

TEST_CASE("LruBytes replacing a key with an oversized value leaves it unchanged") {
  LruBytes<int> c(100);
  c.put("A", 1, 40);
  c.put("A", 2, 500);
  REQUIRE(c.get("A") != nullptr);
  CHECK(*c.get("A") == 1);
  CHECK(c.used() == 40);
}

TEST_CASE("LruBytes used() is exact after a run of evictions") {
  LruBytes<int> c(100);
  for (int i = 0; i < 50; ++i) c.put("k" + std::to_string(i), i, 7 + (i % 5));
  CHECK(c.used() <= 100);
  size_t sum = 0;
  for (int i = 0; i < 50; ++i) {
    if (c.get("k" + std::to_string(i))) sum += 7 + (i % 5);
  }
  CHECK(sum == c.used());
  CHECK(c.get("k49") != nullptr);
  CHECK(c.get("k0") == nullptr);
}
