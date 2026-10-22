// Byte-budget LRU cache. Header-only, no LVGL or platform dependencies.
#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

template <class V>
class LruBytes {
 public:
  explicit LruBytes(size_t budget) : budget_(budget) {}

  // Stores `value` under `key`, charged as `bytes`, evicting least recently
  // used entries until it fits. An existing key is replaced. An item larger
  // than the whole budget is not stored and changes nothing.
  void put(const std::string& key, V value, size_t bytes) {
    if (bytes > budget_) return;
    erase(key);
    while (used_ + bytes > budget_ && !order_.empty()) {
      const Entry& oldest = order_.back();
      used_ -= oldest.bytes;
      index_.erase(oldest.key);
      order_.pop_back();
    }
    order_.push_front(Entry{key, std::move(value), bytes});
    index_[key] = order_.begin();
    used_ += bytes;
  }

  // Returns the stored value and marks it most recently used, or nullptr. The
  // pointer stays valid until that entry is evicted, replaced or the cache dies.
  V* get(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    order_.splice(order_.begin(), order_, it->second);
    return &it->second->value;
  }

  size_t used() const { return used_; }
  size_t count() const { return index_.size(); }

 private:
  struct Entry {
    std::string key;
    V value;
    size_t bytes;
  };

  void erase(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    used_ -= it->second->bytes;
    order_.erase(it->second);
    index_.erase(it);
  }

  size_t budget_;
  size_t used_ = 0;
  std::list<Entry> order_;  // front = most recently used
  std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
};
