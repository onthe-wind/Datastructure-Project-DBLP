#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

class StringArena {
public:
    explicit StringArena(std::size_t initial_chunk_bytes = 16u * 1024u * 1024u)
        : chunk_size_(initial_chunk_bytes == 0 ? min_chunk_bytes() : initial_chunk_bytes) {
        if (chunk_size_ < min_chunk_bytes()) {
            chunk_size_ = min_chunk_bytes();
        }
        push_chunk(chunk_size_);
    }

    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;
    StringArena(StringArena&&) = default;
    StringArena& operator=(StringArena&&) = default;

    [[nodiscard]] std::string_view store(std::string_view s) {
        if (s.empty()) {
            return {};
        }
        if (s.size() > remaining_) {
            ensure_room(s.size());
        }
        std::memcpy(cur_, s.data(), s.size());
        char* start = cur_;
        cur_ += s.size();
        remaining_ -= s.size();
        return {start, s.size()};
    }

    void reset() noexcept {
        chunks_.clear();
        cur_ = nullptr;
        remaining_ = 0;
        push_chunk(chunk_size_);
    }

    [[nodiscard]] std::size_t bytes_allocated() const noexcept {
        std::size_t total = 0;
        for (const auto& c : chunks_) {
            total += c.size;
        }
        return total;
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept {
        std::size_t used = 0;
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            const Chunk& c = chunks_[i];
            if (i + 1 == chunks_.size()) {
                used += c.size - remaining_;
            } else {
                used += c.size;
            }
        }
        return used;
    }

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        std::size_t size = 0;
    };

    static constexpr std::size_t min_chunk_bytes() noexcept { return 4096; }

    void push_chunk(std::size_t n) {
        chunks_.push_back(Chunk{std::make_unique_for_overwrite<char[]>(n), n});
        cur_ = chunks_.back().data.get();
        remaining_ = n;
    }

    void ensure_room(std::size_t need) {
        if (need > chunk_size_) {
            push_chunk(need);
            return;
        }
        push_chunk(chunk_size_);
    }

    std::vector<Chunk> chunks_;
    char* cur_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t chunk_size_;
};

template <typename K, typename V, typename Hash = std::hash<K>, typename Eq = std::equal_to<K>>
class FlatMap {
public:
    using key_type = K;
    using mapped_type = V;
    using size_type = std::size_t;

    FlatMap() = default;

    explicit FlatMap(size_type bucket_count) { reserve(bucket_count); }

    FlatMap(const FlatMap&) = delete;
    FlatMap& operator=(const FlatMap&) = delete;

    FlatMap(FlatMap&& other) noexcept
        : slots_(std::move(other.slots_)), size_(std::exchange(other.size_, 0)) {}

    FlatMap& operator=(FlatMap&& other) noexcept {
        if (this != &other) {
            clear();
            slots_ = std::move(other.slots_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~FlatMap() { clear(); }

    void clear() noexcept {
        for (Slot& s : slots_) {
            if (s.state == SlotState::Occupied) {
                std::destroy_at(&s.key());
                std::destroy_at(&s.value());
                s.state = SlotState::Empty;
            } else if (s.state == SlotState::Tombstone) {
                s.state = SlotState::Empty;
            }
        }
        size_ = 0;
    }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    void reserve(size_type n) {
        if (n == 0) {
            return;
        }
        const size_type cap = round_up_pow2(std::max<std::size_t>(n, min_capacity));
        if (cap > slots_.size()) {
            rehash(cap);
        }
    }

    void reserve_capacity(size_type new_cap) {
        if (new_cap <= slots_.size()) {
            return;
        }
        const size_type cap = round_up_pow2(std::max<std::size_t>(new_cap, min_capacity));
        rehash(cap);
    }

    [[nodiscard]] V* find(const K& key) noexcept {
        if (slots_.empty()) {
            return nullptr;
        }
        const std::size_t idx = find_slot_index(key, false);
        if (idx == npos || slots_[idx].state != SlotState::Occupied) {
            return nullptr;
        }
        return &slots_[idx].value();
    }

    [[nodiscard]] const V* find(const K& key) const noexcept {
        return const_cast<FlatMap*>(this)->find(key);
    }

    [[nodiscard]] bool contains(const K& key) const noexcept { return find(key) != nullptr; }

    template <typename... Args>
    std::pair<V*, bool> try_emplace(const K& key, Args&&... args) {
        if (slots_.empty()) {
            grow_to(min_capacity);
        } else if (should_grow()) {
            grow_to(slots_.size() * 2);
        }
        std::size_t idx = find_slot_index(key, true);
        if (slots_[idx].state == SlotState::Occupied) {
            return {&slots_[idx].value(), false};
        }
        std::construct_at(&slots_[idx].key(), key);
        std::construct_at(&slots_[idx].value(), std::forward<Args>(args)...);
        slots_[idx].state = SlotState::Occupied;
        ++size_;
        return {&slots_[idx].value(), true};
    }

    V& operator[](const K& key) {
        if (auto* p = find(key)) {
            return *p;
        }
        auto [ptr, inserted] = try_emplace(key);
        (void)inserted;
        return *ptr;
    }

    bool erase(const K& key) noexcept {
        if (slots_.empty()) {
            return false;
        }
        const std::size_t idx = find_slot_index(key, false);
        if (idx == npos || slots_[idx].state != SlotState::Occupied) {
            return false;
        }
        std::destroy_at(&slots_[idx].key());
        std::destroy_at(&slots_[idx].value());
        slots_[idx].state = SlotState::Tombstone;
        --size_;
        return true;
    }

    template <typename Func>
    void for_each(Func&& func) {
        for (Slot& s : slots_) {
            if (s.state == SlotState::Occupied) {
                func(s.key(), s.value());
            }
        }
    }

    template <typename Func>
    void for_each(Func&& func) const {
        for (const Slot& s : slots_) {
            if (s.state == SlotState::Occupied) {
                func(s.key(), s.value());
            }
        }
    }

private:
    enum class SlotState : std::uint8_t { Empty, Occupied, Tombstone };

    struct Slot {
        alignas(K) unsigned char key_storage[sizeof(K)]{};
        alignas(V) unsigned char value_storage[sizeof(V)]{};
        SlotState state = SlotState::Empty;

        K& key() noexcept { return *reinterpret_cast<K*>(key_storage); }
        V& value() noexcept { return *reinterpret_cast<V*>(value_storage); }
        const K& key() const noexcept { return *reinterpret_cast<const K*>(key_storage); }
        const V& value() const noexcept { return *reinterpret_cast<const V*>(value_storage); }
    };

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    static constexpr std::size_t min_capacity = 8;
    static constexpr double max_load = 0.72;

    static std::size_t round_up_pow2(std::size_t x) {
        if (x < min_capacity) {
            return min_capacity;
        }
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        if constexpr (sizeof(std::size_t) > 4) {
            x |= x >> 32;
        }
        ++x;
        return x;
    }

    [[nodiscard]] bool should_grow() const noexcept {
        return size_ >= static_cast<std::size_t>(static_cast<double>(slots_.size()) * max_load);
    }

    void grow_to(std::size_t new_cap) {
        const std::size_t cap = round_up_pow2(std::max(new_cap, min_capacity));
        rehash(cap);
    }

    void rehash(std::size_t new_capacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        size_ = 0;
        slots_.assign(new_capacity, Slot{});

        if (old.empty()) {
            return;
        }

        for (Slot& s : old) {
            if (s.state != SlotState::Occupied) {
                continue;
            }
            K& k = s.key();
            V& v = s.value();
            std::size_t idx = find_slot_index_for_insert(k);
            std::construct_at(&slots_[idx].key(), std::move(k));
            std::construct_at(&slots_[idx].value(), std::move(v));
            slots_[idx].state = SlotState::Occupied;
            std::destroy_at(&k);
            std::destroy_at(&v);
            s.state = SlotState::Empty;
            ++size_;
        }
    }

    [[nodiscard]] std::size_t find_slot_index(const K& key, bool for_insert) const noexcept {
        const std::size_t mask = slots_.size() - 1;
        std::size_t idx = hasher_(key)& mask;
        std::size_t first_tombstone = npos;

        for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
            Slot& s = const_cast<Slot&>(slots_[idx]);
            if (s.state == SlotState::Empty) {
                if (for_insert) {
                    return first_tombstone != npos ? first_tombstone : idx;
                }
                return npos;
            }
            if (s.state == SlotState::Tombstone) {
                if (for_insert && first_tombstone == npos) {
                    first_tombstone = idx;
                }
            } else if (eq_(s.key(), key)) {
                return idx;
            }
            idx = (idx + 1) & mask;
        }
        return npos;
    }

    [[nodiscard]] std::size_t find_slot_index_for_insert(const K& key) {
        const std::size_t mask = slots_.size() - 1;
        std::size_t idx = hasher_(key)& mask;
        std::size_t first_tombstone = npos;

        for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
            Slot& s = slots_[idx];
            if (s.state == SlotState::Empty) {
                return first_tombstone != npos ? first_tombstone : idx;
            }
            if (s.state == SlotState::Tombstone) {
                if (first_tombstone == npos) {
                    first_tombstone = idx;
                }
            } else if (eq_(s.key(), key)) {
                return idx;
            }
            idx = (idx + 1) & mask;
        }
        std::abort();
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    [[no_unique_address]] Hash hasher_{};
    [[no_unique_address]] Eq eq_{};
};
