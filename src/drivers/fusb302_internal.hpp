// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Internal state and helpers shared between fusb302.cpp (K2_HOT + core active loop) and fusb302_cold.cpp (cold setup/probes).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "k2/drivers/fusb302.hpp"
#include "k2/pd/emk_ra.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/pd_ccscan.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_interrogate.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_listen.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_phy.hpp"
#include "k2/pd/pd_probe.hpp"
#include "k2/pd/pd_rdseek.hpp"
#include "k2/pd/pd_reread.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vconn.hpp"

namespace k2::fusb302::detail {

namespace phy = pd::phy;
using phy::Reg;

inline constexpr std::uint8_t kIntHardRst = 1u << 0;
inline constexpr std::uint8_t kIntTxSent = 1u << 2;
inline constexpr std::uint8_t kIntRetryFail = 1u << 4;
inline constexpr std::uint8_t kRxEmpty = 1u << 5;
inline constexpr std::uint8_t kComp = 1u << 5;
inline constexpr unsigned kBootNotes = 24;
inline constexpr std::uint32_t kBootNoteSpanMs = 3000;
inline constexpr std::array<std::uint16_t, kBootReadbacks> kReadbackAtMs = {50, 600, 3000};
inline constexpr std::uint32_t kRereadLingerMs = 30000;

extern std::uint8_t g_device_id, g_active_cc;
extern bool g_hr_latched;
extern pd::DetachTracker g_detach;
extern bool g_pd_traffic;
extern bool g_pd_seen_attach;
extern pd::LineFallback g_linefb;
extern pd::IdleWake g_idle_wake;
extern std::uint32_t g_ident_session;
extern volatile bool g_irq;
extern std::int32_t g_vbus_mv;
extern std::int32_t g_cc_sink_mv;
extern std::uint32_t g_attach_ms;
extern std::uint8_t g_bc_max;
extern std::uint32_t g_req_period_ms;
extern bool g_new_source;
extern std::uint8_t g_pin_rd;
extern std::uint8_t g_boot_pin_rd;
extern bool g_rd_inherited;
extern bool g_lab_pin;
extern bool g_pin_moved;
extern pd::RdHold g_rd_hold;
extern bool g_rd_op;
extern bool g_start_requested;
extern bool g_rd_start;
extern bool g_run_owner;
extern bool g_wake_attach;
extern bool g_inline;
extern raguard::WaitKind g_wait_kind;
extern bool g_ident_owns_rd;
extern bool g_ident_wait;
extern bool g_ident_alone;
extern bool g_ident_was_running;
extern bool g_ident_result_valid;
extern bool g_ident_alone_hold;
extern std::uint32_t g_ident_wait_at, g_ident_ready_at;
extern pd::AutoCrcGate g_crc;
extern pd::AutoCrcGate g_rd_gate;
extern bool g_legacy_only;
extern bool g_crc_on;
extern bool g_tx_allowed;
extern pd::Listen g_listen;
extern bool g_cable_read;
extern bool g_hr_tool_armed;
extern pd::Choice g_prep_choice;
extern std::uint32_t g_caps_count;
extern bool g_caps_acked;
extern pd::TriggerPrep g_prep;
extern pd::CapsSession g_session;
extern pd::Engine g_engine;
extern pd::CableLimits g_physical_cable;
extern std::uint32_t g_physical_session;
extern pd::CableLimits g_scanned_cable;
extern bool g_scanned_valid;
extern emk_variant::Answered g_answered;
extern emk_attached::ReadInfo g_read_info;
extern raclass::PullDown g_pull_down;
extern VbusFn g_vbus_source;
extern bool g_probe_log_pending;
extern bool g_reread_log_pending;
extern bool g_reread_owns_rd;
extern bool g_reread_made_contract;
extern std::uint32_t g_reread_release_at;
extern std::uint8_t g_force_cc;
extern SopPrimeHook g_sop_prime_hook;
extern pd::Vconn g_vconn;
extern bool g_vconn_applied;
extern bool g_vconn_give;
extern bool g_ra;
extern emkra::Refuse g_ra_refused;
extern bool g_ra_run;
extern std::uint8_t g_msg_id;
extern std::uint32_t g_last_rx_ms;
extern bool g_getcap_force;
extern bool g_getcap_armed;
extern std::uint32_t g_getcap_at;
extern std::array<BootNote, kBootNotes> g_boot_notes;
extern unsigned g_boot_note_n, g_boot_note_lost;
extern std::array<BootNote, kBootReadbacks> g_readbacks;
extern unsigned g_readback_n;
extern std::uint8_t g_initial_pin_rd;
extern bool g_ra_early;
extern bool g_mark_read;
extern std::uint16_t g_mark_raw;
extern pd::IqPhase g_iq_phase;
extern pd::IqStep g_iq_step;
extern std::uint8_t g_iq_closed;
extern bool g_iq_inited;
#if K2_HOST_REPORT
extern LoopHost* g_lh;
extern bool g_lh_dfp;
extern bool g_lh_idle;
#endif

std::uint8_t cc_mask();
void set_cc_mask(std::uint8_t m);
void set_boot_pin(std::uint8_t pin);

pd::RdSeek& rdseek();
pd::Sink& sink();
pd::Identify& ident();
pd::Probe& probe();
pd::Reread& reread();
pd::Interrogate& iq();
bool ident_running();
bool probe_running();
bool reread_running();
bool iq_running();

// The lab listen attach (fusb302_cold.cpp): the poll tick that ends the window, and the hook the other Rd owners call.
void listen_tick();
void listen_lost();

void boot_note(std::uint8_t kind, std::uint8_t val);
bool wr(Reg r, std::uint8_t v);
bool rd(Reg r, std::uint8_t& v);
bool fifo_read(std::span<std::uint8_t> out);
bool fifo_write_tx(std::uint16_t header, std::span<const std::uint8_t> objs, std::span<const std::uint8_t> stream);
void flush_fifos();
void clear_irqs();
void cc_switches(bool vconn, std::uint8_t mask = 3);
pd::Sop sop_from_token(std::uint8_t token);
std::uint8_t ctl1();
void write_sw1();
void flush_rx();
void configure_passive();
void apply_rd();
std::uint8_t idle_rd();
bool inline_now();
bool downstream_now();
void detect_cc_orientation();
void log_pdo(int n, const pd::Pdo& p);
void log_epr_info(pd::EprInfo s);
void log_ext(unsigned type);
void log_vdm(std::uint16_t header, std::span<const std::uint8_t> payload);
void log_cable(const pd::Emarker& e);
void log_trigger(const pd::Choice& c, pd::Err e);
void log_boot_notes();
void iq_report();
pd::RereadLink reread_link();
void reread_housekeeping(std::uint32_t now);
void ident_wait_refuse();
void ident_result_gate(std::uint32_t now);
void ident_service(std::uint32_t now);
void handle_frame_entry(pd::Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, bool wire);
bool transmit_msg(const pd::TxMsg& m);
bool set_rd_for_run(bool on, bool keep_scan);
bool cc_net_mv(std::uint16_t& cc1, std::uint16_t& cc2);
void vconn_mark();
void vconn_force_off(pd::VconnWhy why);
void trace_sink();
void reset_contract_linger();
void detach_reset(std::uint8_t status0);
bool idle_owner_free();
pd::Err request_inner(const pd::Choice& c, bool over_load);

}  // namespace k2::fusb302::detail
