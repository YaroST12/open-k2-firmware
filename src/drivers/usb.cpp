// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB glue, see k2/drivers/usb.hpp.
#include "k2/core/tx_queue.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/usb.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <span>

#include "k2/core/fmt.hpp"
#include "k2/core/binlog.hpp"
#include "k2/core/logfmt.hpp"
#include "k2/core/logring.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "tusb.h"
#include "k2/core/build_config.h"
#if K2_NET
#include "k2/net/net.hpp"
#endif
#include "k2/drivers/usbcfg.hpp"

namespace k2::usb {

Stats stats;

namespace {

constexpr std::size_t kQueueSize = 16;
constexpr std::size_t kSlotSize = 64;  // one HID report

TxQueue<kQueueSize, kSlotSize> g_queue;
volatile bool g_started;
bool g_log_on = true;

}  // namespace

void flush() {
    while (const std::uint8_t *r = g_queue.front()) {
        if (!tud_hid_ready() || !tud_hid_report(0, r, kSlotSize)) break;
        g_queue.pop();
        stats.log_sent++;
    }
}

// SE0 before the pull-up appears: long enough for any host to register a clean disconnect, so the
// first attach after a setting change (reset with the cable plugged) starts from a fresh port.
constexpr std::uint32_t kDetachHoldMs = 150;

void dbg_init_mark();   // [DEBUG-ue1]

void init() {
    using namespace reg;
    Crm& c = crm();
    dbg_init_mark();

    // 1. GPIOA clock; keep the USB core and ACC off while PA11/PA12 are reconfigured.
    c.APB2EN |= Crm::APB2EN_GPIOA;
    c.APB1EN &= ~Crm::APB1EN_USB;
    c.APB2EN &= ~Crm::APB2EN_ACC;

    // 2. Force a detach: D- and D+ driven low as push-pull so the host sees the device go.
    pins::usb_dm.mode(Mode::OutPp50);
    pins::usb_dp.mode(Mode::OutPp50);
    pins::usb_dm.clear();
    pins::usb_dp.clear();
    systick::delay_ms(kDetachHoldMs);

    // 3. D+ internal pull-up (PA12 input, ODR high) announces the device;
    //    D- floats. Mode first, ODR second: keeps D+ off the push-pull high.
    pins::usb_dm.mode(Mode::InFloating);
    pins::usb_dp.mode(Mode::InPull);
    pins::usb_dp.set();

    // 4. ACC trims HICK to 48 MHz from the USB SOF: limits 7990 / 8000 / 8010 around the
    //    8000 counts expected per 1 ms frame.
    c.APB2EN |= Crm::APB2EN_ACC;
    acc().C1 = 7990;
    acc().C2 = 8000;
    acc().C3 = 8010;
    acc().CTRL1 = Acc::CTRL1_CALON | Acc::CTRL1_ENTRIM | Acc::CTRL1_CALRSEL_HICK | Acc::CTRL1_SOFSEL;  // 0x33

    // 5. Route the 48 MHz HICK to the USB core and select PLL/5 = 48 MHz (CFG bit 27 and bit 23,
    //    USBDIV; bit 22 clear). MISC1 bit 25 and MISC2 bit 8 are the Artery HICK-to-USB gates.
    c.MISC1 |= 1u << 25;
    c.MISC2 |= 1u << 8;
    c.CFG = (c.CFG & ~0x08C00000u) | 0x08800000u;

    // 6. USB peripheral clock on APB1.
    c.APB1EN |= Crm::APB1EN_USB;

    // 7. TinyUSB: core reset, BTABLE, interrupts, pull-up connect, NVIC.
    const tusb_rhport_init_t rh_init{.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    tusb_init(0, &rh_init);
#if K2_NET
    if (usbcfg::current.net) net::init();
#endif
    g_started = true;
}

void detach(std::uint32_t hold_ms) {
    using namespace reg;
    if (!g_started) return;
    g_started = false;                 // service() from the delay below must not touch the core
    tud_disconnect();                  // pull-up off where the core drives it
    nvic().ICER[0] = (1u << 19) | (1u << 20);   // USB_HP / USB_LP_CAN1_RX0
    crm().APB1EN &= ~Crm::APB1EN_USB;
    pins::usb_dm.mode(Mode::OutPp50);  // D+/D- low: the host sees SE0, i.e. a detach
    pins::usb_dp.mode(Mode::OutPp50);
    pins::usb_dm.clear();
    pins::usb_dp.clear();
    systick::delay_ms(hold_ms);
}

bool configured() { return tud_mounted(); }
bool host_active() { return g_started && tud_mounted() && !tud_suspended(); }

Lines lines() {
    Lines l;
    l.core = g_started;
    if (g_started) {
        const std::uint16_t fnr = reg::usb_fnr();
        l.configured = tud_mounted();
        l.rx_dp = (fnr & reg::kUsbFnrRxdp) != 0;
        l.rx_dm = (fnr & reg::kUsbFnrRxdm) != 0;
        l.frame = fnr & reg::kUsbFnrFrame;
    }
    l.pin_dp = pins::usb_dp.read();
    l.pin_dm = pins::usb_dm.read();
    return l;
}

namespace {
// What dp_pullup() found before its first change: the core's pull-up bit, or PA12's mode nibble and output latch.
struct PullSaved {
    Pull kind = Pull::None;
    std::uint32_t cfg = 0;
    std::uint32_t mode = 0;
    bool odr = false;
};
PullSaved g_pull;

std::uint32_t pa12_mode() { return (reg::gpio(pins::usb_dp.port).CRH >> ((pins::usb_dp.n & 7u) * 4u)) & 0xFu; }
}  // namespace

Pull dp_pullup(bool on) {
    if (g_pull.kind == Pull::None) {
        g_pull.kind = g_started ? Pull::Core : Pull::Gpio;
        g_pull.cfg = reg::usb_cfg() & reg::kUsbCfgPullupOff;
        g_pull.mode = pa12_mode();
        g_pull.odr = (reg::gpio(pins::usb_dp.port).ODR & pins::usb_dp.mask()) != 0;
    }
    // init() leaves PA12 an input with its weak pull-up next to the core's 1.5k: "off" releases both, "on" with the core
    // brings back the pin as found, without it the weak pull-up is the only one there is.
    if (g_pull.kind == Pull::Core) {
        if (on) {
            reg::usb_cfg() &= ~reg::kUsbCfgPullupOff;
            pins::usb_dp.write(g_pull.odr);
            pins::usb_dp.mode(reg::Mode(g_pull.mode));
        } else {
            reg::usb_cfg() |= reg::kUsbCfgPullupOff;
            pins::usb_dp.mode(reg::Mode::InFloating);
        }
    } else if (on) {
        pins::usb_dp.pull_up();
    } else {
        pins::usb_dp.mode(reg::Mode::InFloating);
    }
    return g_pull.kind;
}

void dp_pullup_restore() {
    if (g_pull.kind == Pull::None) return;
    if (g_pull.kind == Pull::Core) reg::usb_cfg() = (reg::usb_cfg() & ~reg::kUsbCfgPullupOff) | g_pull.cfg;
    pins::usb_dp.write(g_pull.odr);
    pins::usb_dp.mode(reg::Mode(g_pull.mode));
    g_pull = {};
}

// [DEBUG-ue1] enumeration probe: samples for the first seconds after init(), dumped once later.
namespace dbg {
struct Sample { std::uint16_t t; std::uint16_t trim; std::uint16_t fnr; std::uint8_t acc; std::uint8_t errs; std::uint8_t rst; std::uint8_t gap; std::uint16_t isr; };
struct Early { std::uint32_t ms, apb1, cfg, cntr, daddr, crh, idr; };
std::array<Early, 2> early;
struct Ev { std::uint16_t ms; std::uint16_t v; };
std::array<Ev, 48> ev;
std::size_t nev;
std::uint16_t lastv = 0xFFFF;
std::array<Sample, 160> s;
std::size_t n;
std::uint32_t t0, init_ms, init_trim, errs, gapw, last_t;
bool dumped;
void sample(std::uint32_t now, std::uint32_t gap) {
    const std::uint16_t istr = reg::usb_istr();
    if (istr & (1u << 13)) { errs++; reg::usb_istr() = static_cast<std::uint16_t>(~(1u << 13)); }
    if (gap > gapw) gapw = gap;
    const std::uint32_t t = now - t0;
    if (t < 6000 && n < s.size() && (n == 0 || t - last_t >= 25)) {
        last_t = t;
        s[n++] = {std::uint16_t(t), std::uint16_t(reg::crm().CTRL & 0xFFFF), reg::usb_fnr(),
                  std::uint8_t(reg::acc().STS), std::uint8_t(errs), std::uint8_t(stats.resets),
                  std::uint8_t(std::min<std::uint32_t>(gapw, 255)), std::uint16_t(stats.isrs)};
        gapw = 0;
    }
    static std::size_t di;
    if (!dumped && t > 9000) {
        dumped = true;
        log_fmt("[DEBUG-ue1] init_ms=%d trim0=%04X n=%d\n", int(init_ms), unsigned(init_trim), int(n));
        for (const Early& e : early) {
            log_fmt("[DEBUG-ue1] E ms=%d apb1=%08X cfg=%X cntr=%X\n", int(e.ms), unsigned(e.apb1), unsigned(e.cfg),
                    unsigned(e.cntr));
            log_fmt("[DEBUG-ue1] E daddr=%X crh=%08X idr=%04X\n", unsigned(e.daddr), unsigned(e.crh), unsigned(e.idr));
        }
    }
    static std::size_t vi;
    if (dumped && vi < nev && tud_mounted() && g_queue.free() > 8) {
        log_fmt("[DEBUG-ue1] V ms=%d v=%03X\n", int(ev[vi].ms), unsigned(ev[vi].v));
        vi++;
    } else if (dumped && vi >= nev && di < n && tud_mounted() && g_queue.free() > 8) {
        {
            const Sample& x = s[di++];
            log_fmt("[DEBUG-ue1] t=%d tr=%04X f=%04X a=%X e=%d r=%d g=%d i=%d\n", int(x.t), unsigned(x.trim),
                    unsigned(x.fnr), unsigned(x.acc), int(x.errs), int(x.rst), int(x.gap), int(x.isr));
        }
    }
}
void take(Early& e) {
    reg::crm().APB2EN |= reg::Crm::APB2EN_GPIOA;
    e.ms = systick::millis();
    e.apb1 = reg::crm().APB1EN;
    reg::crm().APB1EN |= reg::Crm::APB1EN_USB;   // read the core with its clock on, then restore the gate
    e.cfg = reg::usb_cfg();
    e.cntr = *reinterpret_cast<volatile std::uint32_t*>(0x40005C40);
    if (!(e.apb1 & reg::Crm::APB1EN_USB)) reg::crm().APB1EN &= ~reg::Crm::APB1EN_USB;
    e.daddr = reg::gpio(pins::usb_dp.port).ODR;   // ODR (PA12 = bit 12)
    e.crh = reg::gpio(pins::usb_dp.port).CRH;
    e.idr = reg::gpio(pins::usb_dp.port).IDR;
}
}  // namespace dbg
// [DEBUG-ue1] from SysTick: PA12/PA11 levels, PA12 mode nibble + ODR, USB clock bit, on every change.
void dbg_tick() {
    const std::uint32_t ms = systick::millis();
    if (ms > 6000 || dbg::nev >= dbg::ev.size()) return;
    const auto& g = reg::gpio(pins::usb_dp.port);
    std::uint16_t v = 0;
    if (g.IDR & (1u << 12)) v |= 1;
    if (g.IDR & (1u << 11)) v |= 2;
    if (g.ODR & (1u << 12)) v |= 4;
    if (reg::crm().APB1EN & reg::Crm::APB1EN_USB) v |= 8;
    v |= std::uint16_t(((g.CRH >> 16) & 0xF) << 4);
    if (v != dbg::lastv) { dbg::lastv = v; dbg::ev[dbg::nev++] = {std::uint16_t(ms), v}; }
}
void dbg_boot_mark() {
    dbg::take(dbg::early[0]);
    // [DEBUG-ue1] experiment: SE0 from the first instruction until init() (D+/D- push-pull low).
    pins::usb_dm.mode(reg::Mode::OutPp50);
    pins::usb_dp.mode(reg::Mode::OutPp50);
    pins::usb_dm.clear();
    pins::usb_dp.clear();
}
void dbg_init_mark() {
    dbg::take(dbg::early[1]); dbg::t0 = dbg::init_ms = systick::millis(); dbg::init_trim = reg::crm().CTRL & 0xFFFF; }

void poll() {
    if (!g_started) return;            // USB link off (k2/drivers/usbcfg.hpp): the core was never started
    static std::uint32_t last_ms;
    static bool first = true;
    const std::uint32_t now = systick::millis();
    if (!first && now - last_ms > stats.poll_gap_max) stats.poll_gap_max = now - last_ms;
    dbg::sample(now, first ? 0 : now - last_ms);
    first = false;
    last_ms = now;

    tud_task();
#if K2_NET
    if (usbcfg::current.net) net::poll();
#endif
    flush();
}

void pump() {
    if (!g_started) return;
    tud_int_handler(0);
    poll();
}

void service() {
    static bool busy;
    if (!g_started || busy) return;
    busy = true;
    poll();
    busy = false;
}

// Copy one 64-byte report into the queue. Log lines never evict queued frames (tx_queue.hpp).
static void push(std::span<const std::uint8_t, kSlotSize> report, bool is_log) {
    if (is_log) g_queue.push_log(report);
    else g_queue.push_report(report);
    stats.log_dropped = g_queue.dropped();
    flush();
}

static void (*g_tap)(std::string_view);
void set_log_tap(void (*fn)(std::string_view)) { g_tap = fn; }

static LogRing<2048> g_ring;
static BinLog<kLogBinBytes> g_bin;
const BinLog<kLogBinBytes>& log_bin_ring() { return g_bin; }
std::size_t log_text(std::span<char> out) { return g_ring.snapshot(out); }
std::size_t log_bin(std::span<std::uint8_t> out, std::uint32_t& base_ms, bool& older) {
    const BinLogSnapshot s = g_bin.snapshot(out);
    base_ms = s.base_ms;
    older = s.older;
    return s.len;
}
std::array<std::string_view, 2> log_view() { return g_ring.view(); }

void log(std::string_view msg) {
    g_ring.push(msg.substr(0, kSlotSize - 1));   // what the HID line would carry
    if (g_tap) g_tap(msg.substr(0, kSlotSize - 1));
    if (!g_log_on) return;
    std::array<std::uint8_t, kSlotSize> slot{};
    std::copy_n(msg.begin(), std::min(msg.size(), kSlotSize - 1), slot.begin());
    push(slot, true);
}

void send_report(const std::uint8_t* report) { push(std::span<const std::uint8_t, kSlotSize>(report, kSlotSize), false); }

std::uint8_t tx_free() { return g_queue.free(); }

void log_enable(bool on) { g_log_on = on; }
bool log_enabled() { return g_log_on; }

void vlog_fmt(const char* fmt, std::va_list ap) {
    char buf[kSlotSize];
    log({buf, fmt::vformat(buf, fmt, ap)});
}

void log_fmt(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    vlog_fmt(fmt, ap);
    va_end(ap);
}

void log_packed(std::uint16_t id, std::uint32_t sig, ...) {
    char buf[kSlotSize];
    std::va_list ap, bin;
    va_start(ap, sig);
    va_copy(bin, ap);
    g_bin.push(systick::millis(), id, sig, bin);
    va_end(bin);
    const std::size_t n = logfmt::vencode(buf, id, sig, ap);
    va_end(ap);
    log({buf, n});
}

StatsSnapshot stats_snapshot() {
    return {stats.resets, stats.isrs, stats.log_sent, stats.log_dropped, stats.poll_gap_max};
}

void log_stats() {
    K2LOG("[USB] gap=%dms isr=%d rst=%d sent=%d drop=%d\n", int(stats.poll_gap_max), int(stats.isrs),
            int(stats.resets), int(stats.log_sent), int(stats.log_dropped));
    stats.poll_gap_max = 0;
}

}  // namespace k2::usb

// ---- Interrupts and TinyUSB callbacks (C linkage: named by the vector table / TinyUSB) ----

extern "C" {

// TinyUSB needs a millisecond clock when there is no RTOS.
std::uint32_t tusb_time_millis_api(void) { return k2::systick::millis(); }

// Shared USB/CAN vectors. The USB core raises the low-priority interrupt for everything
// except double-buffered/isochronous CTR (high-priority vector); TinyUSB services both.
void USB_LP_CAN1_RX0_IRQHandler(void) {
    k2::usb::stats.isrs++;
    if (k2::reg::usb_istr() & k2::reg::kUsbIstrReset) k2::usb::stats.resets++;
    tud_int_handler(0);
}

void USB_HP_CAN1_TX_IRQHandler(void) { tud_int_handler(0); }

// TinyUSB enables the wake-up vector; nothing routes EXTI18 to it here, but a handler
// must exist because the default one spins forever.
void USBWakeUp_IRQHandler(void) {}

void tud_hid_report_complete_cb(std::uint8_t, const std::uint8_t*, std::uint16_t) { k2::usb::flush(); }

std::uint16_t tud_hid_get_report_cb(std::uint8_t, std::uint8_t, hid_report_type_t, std::uint8_t*, std::uint16_t) {
    return 0;
}

// Host-to-device reports (EP1 OUT / SET_REPORT) go to the remote-control module, which only
// queues them; they are parsed in the main loop. Runs inside tud_task(), not in the ISR.
void tud_hid_set_report_cb(std::uint8_t, std::uint8_t, hid_report_type_t, const std::uint8_t* buffer,
                           std::uint16_t bufsize) {
    k2::usb::on_remote_report(buffer, bufsize);
}

}  // extern "C"
