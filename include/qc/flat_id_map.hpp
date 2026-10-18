#pragma once
// Open-addressing hash map from a 64-bit id to a 32-bit slot index.
//
// Why not std::unordered_map: every cancel/modify in the order book is an id lookup, and a
// node-based map costs a cache miss per probe plus an allocation per insert. This table is a
// single contiguous array, Fibonacci-hashed, linear-probed, with backward-shift deletion (no
// tombstones, so lookups never degrade after heavy churn).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qc {

class FlatIdMap {
 public:
  static constexpr std::uint64_t kEmptyKey = ~std::uint64_t{0};  // this id value is not storable

  explicit FlatIdMap(std::size_t initial_capacity = 1024) { init(round_up(initial_capacity)); }

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Returns pointer to the value or nullptr.
  std::uint32_t* find(std::uint64_t key) {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.key == key) return &slots_[i].val;
      if (s.key == kEmptyKey) return nullptr;
    }
  }
  const std::uint32_t* find(std::uint64_t key) const {
    return const_cast<FlatIdMap*>(this)->find(key);
  }
  bool contains(std::uint64_t key) const { return find(key) != nullptr; }

  // Returns false (and leaves the map unchanged) if the key already exists.
  bool insert(std::uint64_t key, std::uint32_t val) {
    if ((size_ + 1) * 2 > slots_.size()) grow();  // keep load factor <= 0.5
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == key) return false;
      if (slots_[i].key == kEmptyKey) break;
    }
    slots_[i] = Slot{key, val};
    ++size_;
    return true;
  }

  bool erase(std::uint64_t key) {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == key) break;
      if (slots_[i].key == kEmptyKey) return false;
    }
    // Backward-shift deletion: pull later members of the probe chain into the hole whenever
    // doing so keeps them reachable from their home slot.
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmptyKey) break;
      const std::size_t k = home(slots_[j].key);
      const bool k_in_i_to_j = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (k_in_i_to_j) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].key = kEmptyKey;
    --size_;
    return true;
  }

  void clear() {
    for (auto& s : slots_) s.key = kEmptyKey;
    size_ = 0;
  }

  void reserve(std::size_t n) {
    while (n * 2 > slots_.size()) grow();
  }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t val;
  };

  static std::size_t round_up(std::size_t n) { return std::bit_ceil(n < 16 ? std::size_t{16} : n); }

  void init(std::size_t cap) {
    slots_.assign(cap, Slot{kEmptyKey, 0});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
    size_ = 0;
  }

  std::size_t home(std::uint64_t key) const {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    init(old.size() * 2);
    for (const Slot& s : old) {
      if (s.key != kEmptyKey) insert(s.key, s.val);
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  int shift_ = 0;
  std::size_t size_ = 0;
};

}  // namespace qc
