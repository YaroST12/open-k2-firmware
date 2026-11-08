// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host test of the page arena (include/k2/ui/page_arena.hpp): a LIFO stack of objects built in one aligned buffer.
#include <cstdint>
#include <cstdio>

#include "k2/ui/page_arena.hpp"
#include "test_util.h"

using namespace k2;

namespace {

int g_live = 0, g_built = 0, g_destroyed = 0;

struct Item {
    int id;
    explicit Item(int i) : id(i) {}
    virtual int value() const = 0;   // virtual like the real pages, but no virtual destructor
protected:
    ~Item() = default;
};

struct Small final : Item {
    int counter = 0;
    explicit Small(int i) : Item(i) { g_live++; g_built++; }
    ~Small() { g_live--; g_destroyed++; }
    int value() const override { return 1; }
};

struct Big final : Item {
    std::uint8_t pad[200] = {};
    explicit Big(int i) : Item(i) { g_live++; g_built++; pad[0] = 0x5A; }
    ~Big() { g_live--; g_destroyed++; }
    int value() const override { return 2; }
};

struct alignas(16) Wide final : Item {
    explicit Wide(int i) : Item(i) { g_live++; g_built++; }
    ~Wide() { g_live--; g_destroyed++; }
    int value() const override { return 3; }
};

using Arena = StackArena<Item, 512, 3>;

void test_push_pop_lifo() {
    g_live = g_built = g_destroyed = 0;
    {
        Arena a;
        CHECK_EQ(int(a.depth()), 0);
        CHECK_EQ(a.top() == nullptr, 1);
        Small *s = a.push<Small>(1);
        CHECK_EQ(s != nullptr, 1);
        CHECK_EQ(int(a.depth()), 1);
        CHECK_EQ(a.top() == s, 1);
        Big *b = a.push<Big>(2);
        CHECK_EQ(b != nullptr, 1);
        CHECK_EQ(int(a.depth()), 2);
        CHECK_EQ(a.top() == b, 1);
        CHECK_EQ(a.at(0) == s, 1);
        CHECK_EQ(a.at(1) == b, 1);
        CHECK_EQ(a.at(2) == nullptr, 1);
        CHECK_EQ(b->pad[0], 0x5A);
        CHECK_EQ(a.top()->value(), 2);
        CHECK_EQ(g_live, 2);
        a.pop();
        CHECK_EQ(g_live, 1);
        CHECK_EQ(int(a.depth()), 1);
        CHECK_EQ(a.top() == s, 1);
        a.pop();
        CHECK_EQ(g_live, 0);
        CHECK_EQ(int(a.depth()), 0);
        a.pop();   // popping an empty arena is harmless
        CHECK_EQ(g_destroyed, 2);
    }
}

void test_repush_reconstructs() {
    g_live = g_built = g_destroyed = 0;
    Arena a;
    Small *s = a.push<Small>(7);
    s->counter = 99;
    a.pop();
    Small *again = a.push<Small>(8);
    CHECK_EQ(again == s, 1);          // the same storage is reused
    CHECK_EQ(again->counter, 0);      // but the object is fresh
    CHECK_EQ(again->id, 8);
    CHECK_EQ(g_built, 2);
    CHECK_EQ(g_destroyed, 1);
}

void test_refusals() {
    g_live = g_built = g_destroyed = 0;
    Arena a;
    CHECK_EQ(a.push<Big>(1) != nullptr, 1);
    CHECK_EQ(a.push<Big>(2) != nullptr, 1);
    // Two Big objects use about 440 of 512 bytes: a third does not fit and is refused, nothing is built.
    int built = g_built;
    CHECK_EQ(a.push<Big>(3) == nullptr, 1);
    CHECK_EQ(g_built, built);
    CHECK_EQ(int(a.depth()), 2);
    a.pop();
    // The freed bytes are available again.
    CHECK_EQ(a.push<Big>(4) != nullptr, 1);
    // The depth limit (3) refuses even a small object once three are live.
    a.pop();
    a.pop();
    CHECK_EQ(a.push<Small>(1) != nullptr, 1);
    CHECK_EQ(a.push<Small>(2) != nullptr, 1);
    CHECK_EQ(a.push<Small>(3) != nullptr, 1);
    CHECK_EQ(a.push<Small>(4) == nullptr, 1);
    CHECK_EQ(int(a.depth()), 3);
}

void test_alignment() {
    g_live = g_built = g_destroyed = 0;
    Arena a;
    CHECK_EQ(a.push<Small>(1) != nullptr, 1);
    Wide *w = a.push<Wide>(2);
    CHECK_EQ(w != nullptr, 1);
    CHECK_EQ(int(reinterpret_cast<std::uintptr_t>(w) % 16), 0);
    Small *s = a.push<Small>(3);
    CHECK_EQ(s != nullptr, 1);
    CHECK_EQ(int(reinterpret_cast<std::uintptr_t>(s) % alignof(Small)), 0);
    CHECK_EQ(a.used() <= 512, 1);
}

void test_pop_to_and_clear() {
    g_live = g_built = g_destroyed = 0;
    Arena a;
    a.push<Small>(1);
    a.push<Small>(2);
    a.push<Small>(3);
    a.pop_to(1);
    CHECK_EQ(int(a.depth()), 1);
    CHECK_EQ(g_live, 1);
    CHECK_EQ(a.top()->id, 1);
    a.pop_to(5);   // more than the depth: nothing happens
    CHECK_EQ(int(a.depth()), 1);
    a.push<Small>(4);
    a.clear();
    CHECK_EQ(int(a.depth()), 0);
    CHECK_EQ(g_live, 0);
    CHECK_EQ(int(a.used()), 0);
}

void test_destroys_top_first() {
    static int order[4];
    static int n;
    n = 0;
    struct Tag final : Item {
        explicit Tag(int i) : Item(i) {}
        ~Tag() { order[n++] = id; }
        int value() const override { return 0; }
    };
    {
        StackArena<Item, 128, 3> a;
        a.push<Tag>(1);
        a.push<Tag>(2);
        a.push<Tag>(3);
    }   // the arena destroys what is still live, topmost first
    CHECK_EQ(n, 3);
    CHECK_EQ(order[0], 3);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 1);
}

void test_capacity_helper() {
    static_assert(arena_bytes<Small, Big>() >= sizeof(Small) + sizeof(Big));
    static_assert(arena_bytes<Big>() >= sizeof(Big));
    CHECK_EQ(int(arena_bytes<Big>() % alignof(std::max_align_t) == 0), 1);
}

}  // namespace

int main() {
    test_push_pop_lifo();
    test_repush_reconstructs();
    test_refusals();
    test_alignment();
    test_pop_to_and_clear();
    test_destroys_top_first();
    test_capacity_helper();
    return test_report("test_page_arena");
}
