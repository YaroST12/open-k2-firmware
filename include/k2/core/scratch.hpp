// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One transient scratch arena for the main loop: big working buffers that are live only inside one call.
//
// Big local arrays put kilobytes on the stack of call chains that already sit deep (the UI redraw, a remote
// command, littlefs underneath), and one static per user would spend RAM that is live for microseconds. This is
// the one place those buffers come from: a single aligned byte arena, leased for the duration of one call.
//
//     scratch::Lease lease("emlib");                 // takes the arena; the tag names the holder
//     auto& s = scratch::as<Scratch>();              // a trivially constructible view of it (contents undefined)
//     ...                                            // released when `lease` goes out of scope
//
// Rules, enforced at run time (a violation logs "[SCRATCH] ..." and traps into the [FAULT] handler, so a
// mistake shows at the first use, never as silent corruption):
//   - main loop only: a lease taken from an exception handler is a violation (VECTACTIVE != 0);
//   - one holder at a time: a lease taken while another is held is a violation, so a lease may not span a call
//     that can lease again (keep it to the leaf work that needs the buffer, take it after callbacks return);
//   - the contents are undefined on every acquire (not zeroed) and never outlive the lease.
// A buffer that must live across loop iterations is not scratch.
//
// The arena is sized for the largest user (kSize); each user static_asserts its own need. Host tests use the
// same code; a test can observe a violation through set_violation_handler().
#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace k2::scratch {

// >= the largest user: QrTestPage::paint() (3428 B: v25 code[1713] + union(temp[1713], line[240])), then the charger-journal
// replay (about 2790 B on the device, 2808 on a 64-bit host; boot_restore), then ChargerTestPage result_layout (~2456 B), then
// bbpersist (lz::bound(2048) = 2072). There is one holder at a time. A lease must not span a call that can run the LCD
// idle hook or the systick wait hook (both call transcap::poll()), except where the hooks are lease-free by contract:
// the only lease those hooks can take is the transcap save, and it defers (persist_due) while the arena is held.
inline constexpr std::size_t kSize = 3432;
inline constexpr std::size_t kAlign = 8;

// Why a lease was refused. The default handler logs and traps; a test handler may return, after which
// the lease is granted anyway.
struct Violation {
    const char* want;     // tag of the refused lease
    const char* held;     // tag of the current holder, null when the cause is an exception handler
    bool in_isr;
};
using ViolationHandler = void (*)(const Violation&);
void set_violation_handler(ViolationHandler h);        // null restores the default

void acquire(const char* owner);
void release();
bool held();
const char* owner();                                    // tag of the holder, null when free
std::uint8_t* arena();                                  // the bytes of the current lease; a violation when none is held

class Lease {
public:
    explicit Lease(const char* owner_tag) { acquire(owner_tag); }
    ~Lease() { release(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
};

// The arena as a T (trivially default constructible, so no object is constructed and the contents are undefined).
template <class T>
T& as() {
    static_assert(sizeof(T) <= kSize, "scratch arena too small for this type");
    static_assert(alignof(T) <= kAlign, "scratch arena alignment");
    static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>);
    return *std::launder(reinterpret_cast<T*>(arena()));
}

// The arena as a freshly value-initialised T (for types with default member initialisers; costs one memset).
template <class T>
T& make() {
    static_assert(sizeof(T) <= kSize, "scratch arena too small for this type");
    static_assert(alignof(T) <= kAlign, "scratch arena alignment");
    static_assert(std::is_trivially_destructible_v<T>);
    return *::new (static_cast<void*>(arena())) T();
}

}  // namespace k2::scratch
