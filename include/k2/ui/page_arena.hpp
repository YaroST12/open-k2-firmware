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
#include <new>
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
        if (depth_ >= Depth) return nullptr;
        std::size_t at = align_up(used_, alignof(T));
        if (at + sizeof(T) > Capacity) return nullptr;
        T *obj = ::new (static_cast<void *>(buf_ + at)) T(std::forward<A>(a)...);
        e_[depth_] = {obj, [](Base *b) { static_cast<T *>(b)->~T(); }, used_};
        used_ = at + sizeof(T);
        depth_++;
        return obj;
    }

    // Destroys the top object; a no-op on an empty stack.
    void pop() {
        if (depth_ == 0) return;
        Entry &t = e_[depth_ - 1];
        t.destroy(t.obj);
        used_ = t.prev_used;
        depth_--;
    }
    void pop_to(std::size_t depth) {   // down to `depth` live objects (no-op when there are fewer)
        while (depth_ > depth) pop();
    }
    void clear() { pop_to(0); }

    std::size_t depth() const { return depth_; }
    std::size_t used() const { return used_; }
    Base *at(std::size_t i) const { return i < depth_ ? e_[i].obj : nullptr; }
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

    alignas(std::max_align_t) std::uint8_t buf_[Capacity];
    Entry e_[Depth] = {};
    std::size_t used_ = 0;
    std::size_t depth_ = 0;
};

}  // namespace k2
