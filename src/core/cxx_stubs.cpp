// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// libstdc++ throw helpers, replaced for a -fno-exceptions build.
//
// Header code (std::string_view::substr, std::array::at, ...) calls std::__throw_* on a contract
// violation. Without LTO the optimiser usually proves those calls dead; with LTO the IR symbol
// table still lists them at the first link pass, so ld pulls libstdc++'s functexcept.o, and with it
// the exception runtime, std::string, std::system_error and the unwinder (none of which can link
// here). These strong definitions satisfy the references first. A violation is a firmware bug:
// trap, which the HardFault handler reports.
namespace std {
[[noreturn, gnu::cold]] void __throw_out_of_range_fmt(const char*, ...) { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_out_of_range(const char*) { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_length_error(const char*) { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_logic_error(const char*) { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_invalid_argument(const char*) { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_bad_alloc() { __builtin_trap(); }
[[noreturn, gnu::cold]] void __throw_bad_array_new_length() { __builtin_trap(); }
}  // namespace std
