// Order-ID -> resting order maps.
//
// StdOrderMap wraps std::unordered_map: the baseline, one heap node per order.
// FlatOrderMap is open addressing with linear probing over one preallocated
// array, with backward-shift deletion (no tombstones). Once constructed it
// doesn't allocate unless the live count passes 70% of capacity, in which case
// it doubles. book_replay reports whether that ever happened.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "itch/messages.hpp"

namespace lob {

using itch::Price;
using itch::Side;

struct RestingOrder {
    std::uint16_t locate;
    Side side;
    Price price;
    std::uint32_t shares;
};

class StdOrderMap {
public:
    RestingOrder* find(std::uint64_t ref) {
        auto it = m_.find(ref);
        return it == m_.end() ? nullptr : &it->second;
    }
    const RestingOrder* find(std::uint64_t ref) const { return const_cast<StdOrderMap*>(this)->find(ref); }
    bool insert(std::uint64_t ref, const RestingOrder& o) { return m_.try_emplace(ref, o).second; }
    void erase(std::uint64_t ref) { m_.erase(ref); }
    [[nodiscard]] std::size_t size() const { return m_.size(); }
    [[nodiscard]] std::uint64_t grows() const { return 0; }
    template <class F>
    void for_each(F&& f) const {
        for (const auto& [k, v] : m_) f(k, v);
    }

private:
    std::unordered_map<std::uint64_t, RestingOrder> m_;
};

// Open-addressing uint64 -> V map. V must be trivially copyable.
template <class V>
class BasicFlatMap {
public:
    // capacity is rounded up to a power of two. 4M slots * 24 B = 96 MB.
    explicit BasicFlatMap(std::size_t capacity = std::size_t{1} << 22) { init(round_pow2(capacity)); }

    V* find(std::uint64_t ref) {
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == ref) return &s.val;
            if (s.key == kEmpty) return nullptr;
        }
    }

    const V* find(std::uint64_t ref) const { return const_cast<BasicFlatMap*>(this)->find(ref); }

    bool insert(std::uint64_t ref, const V& o) {
        if (size_ + 1 > max_load_) [[unlikely]] grow();
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == ref) return false;
            if (s.key == kEmpty) {
                s.key = ref;
                s.val = o;
                ++size_;
                return true;
            }
        }
    }

    void erase(std::uint64_t ref) {
        std::size_t i = home(ref);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].key == ref) break;
            if (slots_[i].key == kEmpty) return;
        }
        // Backward-shift: pull later entries of the probe run into the hole if
        // their home slot isn't cyclically within (hole, j].
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_; slots_[j].key != kEmpty; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            const bool stays = hole < j ? (h > hole && h <= j) : (h > hole || h <= j);
            if (!stays) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
    }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t capacity() const { return mask_ + 1; }
    [[nodiscard]] std::uint64_t grows() const { return grows_; }

    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i <= mask_; ++i)
            if (slots_[i].key != kEmpty) f(slots_[i].key, slots_[i].val);
    }

private:
    struct Slot {
        std::uint64_t key;
        V val;
    };
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};

    static std::size_t round_pow2(std::size_t n) {
        std::size_t p = 1024;
        while (p < n) p <<= 1;
        return p;
    }

    void init(std::size_t cap) {
        slots_ = std::make_unique<Slot[]>(cap);
        for (std::size_t i = 0; i < cap; ++i) slots_[i].key = kEmpty;  // also pre-faults every page
        mask_ = cap - 1;
        shift_ = 64 - static_cast<unsigned>(__builtin_ctzll(cap));
        max_load_ = cap / 10 * 7;
        size_ = 0;
    }

    // Fibonacci hashing: spreads ITCH's mostly sequential refs across the table.
    [[nodiscard]] std::size_t home(std::uint64_t k) const {
        return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void grow() {
        auto old = std::move(slots_);
        const std::size_t old_cap = mask_ + 1;
        init(old_cap * 2);
        for (std::size_t i = 0; i < old_cap; ++i)
            if (old[i].key != kEmpty) insert(old[i].key, old[i].val);
        ++grows_;
    }

    std::unique_ptr<Slot[]> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
    std::size_t max_load_ = 0;
    std::uint64_t grows_ = 0;
};

using FlatOrderMap = BasicFlatMap<RestingOrder>;

}  // namespace lob
