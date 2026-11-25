// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A LIFO stack of objects built in place in one aligned buffer, for the UI pages: only the pages that are on screen
// (the rotation page and the menus opened above it) exist in RAM; a page is constructed when it is entered and destroyed
// when it is left. Static storage, no heap.
//
// The buffer is sized by a constexpr maximum over the page types (arena_bytes<>), and push<T>() refuses, returns
// nullptr and builds nothing, when the object does not fit or the depth is full; the caller static_asserts the real
// page chains so the refusal is a safety net that is never reached.
//
// The element base class has no virtual destructor (that would pull operator delete into the image), so every entry
// stores a plain destroy function pointer made for the concrete type at push time.
#pragma once
#include <cstddef>
#include <cstdint>
#include "k2/core/bss_zero.hpp"
#include <new>
#include <type_traits>
#include <utility>

namespace k2 {

// Bytes for a chain of types pushed in one go: each object padded to the arena alignment, plus the alignment slack of
// the first one. A sum, never less than the largest member alone.
template <class... Ts>
constexpr std::size_t arena_bytes() {
    constexpr std::size_t kAlign = alignof(std::max_align_t);
    std::size_t total = 0;
    ((total += (sizeof(Ts) + kAlign - 1) / kAlign * kAlign), ...);
    return total;
}

template <class Base, std::size_t Capacity, std::size_t Depth>
class StackArena {
public:
    StackArena() = default;
    StackArena(const StackArena &) = delete;
    StackArena &operator=(const StackArena &) = delete;
    ~StackArena() { clear(); }

    // Builds a T on top of the stack. Returns nullptr (nothing is built) when the depth is full or T does not fit.
    template <class T, class... A>
    T *push(A &&...a) {
        static_assert(alignof(T) <= alignof(std::max_align_t), "arena objects must not be over-aligned");
        void *mem = (alignof(T) <= 4) ? alloc_slot4(sizeof(T)) : alloc_slot(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        T *obj = ::new (mem) T(std::forward<A>(a)...);
        if constexpr (std::is_trivially_destructible_v<T>) {
            commit_slot(obj);
        } else {
            commit_slot_d(obj, [](Base *b) { static_cast<T *>(b)->~T(); });
        }
        return obj;
    }

    // Destroys the top object; a no-op on an empty stack.
    [[gnu::noinline]] void pop() {
        if (depth_ == 0) return;
        Entry &t = e_[depth_ - 1];
        if (t.destroy) t.destroy(t.obj);
        used_ = t.prev_used;
        depth_--;
    }
    void pop_to(std::size_t depth) {   // down to `depth` live objects (no-op when there are fewer)
        while (depth_ > depth) pop();
    }
    void clear() { pop_to(0); }

    std::size_t depth() const { return depth_; }
    std::size_t used() const { return used_; }
    [[gnu::noinline]] Base *at(std::size_t i) const { return i < depth_ ? e_[i].obj : nullptr; }
    Base *top() const { return depth_ ? e_[depth_ - 1].obj : nullptr; }

    static constexpr std::size_t capacity() { return Capacity; }
    static constexpr std::size_t max_depth() { return Depth; }

private:
    struct Entry {
        Base *obj;
        void (*destroy)(Base *);
        std::size_t prev_used;   // `used_` before this object, restored on pop
    };
    static constexpr std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }
    [[gnu::noinline]] void *alloc_slot4(std::size_t size) { return alloc_slot(size, 4); }
    [[gnu::noinline]] void *alloc_slot(std::size_t size, std::size_t align) {
        if (depth_ >= Depth) return nullptr;
        const std::size_t at = align_up(used_, align);
        if (at + size > Capacity) return nullptr;
        e_[depth_].prev_used = used_;
        used_ = at + size;
        return buf_ + at;
    }
    [[gnu::noinline]] void commit_slot(Base *obj) { commit_slot_d(obj, nullptr); }
    [[gnu::noinline]] void commit_slot_d(Base *obj, void (*destroy)(Base *)) {
        Entry &t = e_[depth_++];
        t.obj = obj;
        t.destroy = destroy;
    }

    alignas(std::max_align_t) std::uint8_t buf_[Capacity];
    Entry e_[Depth];
    std::size_t used_ K2_BSS_INIT(0);
    std::size_t depth_ K2_BSS_INIT(0);
};

}  // namespace k2
