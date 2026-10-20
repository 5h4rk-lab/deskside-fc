// Small key/value preference store. The firmware implements Prefs over NVS;
// the simulator and the tests use MemoryPrefs. Pure logic: no stdio, no
// filesystem, no exceptions.
#pragma once

#include <map>
#include <string>

class Prefs {
 public:
  virtual ~Prefs() = default;
  // True and fills `out` when the key exists; leaves `out` alone otherwise.
  virtual bool get(const std::string& key, std::string& out) const = 0;
  virtual void set(const std::string& key, const std::string& value) = 0;
  virtual void remove(const std::string& key) = 0;
};

class MemoryPrefs : public Prefs {
 public:
  bool get(const std::string& key, std::string& out) const override;
  void set(const std::string& key, const std::string& value) override;
  void remove(const std::string& key) override;

 private:
  std::map<std::string, std::string> values_;
};

// Integer value of `key`. A missing key, or a value that is not an optional
// leading '-' followed by digits only (or that overflows int), gives `def`.
int pref_int(const Prefs& prefs, const std::string& key, int def);
void set_pref_int(Prefs& prefs, const std::string& key, int value);

// "1" is true, "0" is false; anything else (or missing) gives `def`.
bool pref_bool(const Prefs& prefs, const std::string& key, bool def);
void set_pref_bool(Prefs& prefs, const std::string& key, bool value);
