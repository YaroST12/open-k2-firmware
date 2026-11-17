// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/report_card_store.hpp"

namespace k2::report_card {

std::size_t Store::list(std::span<Card> out) const {
    ensure();
    Order o = order();
    std::size_t n = 0;
    for (unsigned i = 0; i < o.n && n < out.size(); ++i)
        if (load(idx_[o.at[i]], out[n])) ++n;
    return n;
}

std::size_t Store::numbers(std::span<std::uint16_t> out) const {
    ensure();
    Order o = order();
    std::size_t n = 0;
    for (unsigned i = 0; i < o.n && n < out.size(); ++i) out[n++] = idx_[o.at[i]].card_no;
    return n;
}

bool Store::find(std::uint32_t fp, Card& c) const {
    ensure();
    const int i = by_fp(fp);
    return i >= 0 && load(idx_[unsigned(i)], c);
}

bool Store::get(std::uint16_t card_no, Card& c) const {
    ensure();
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no == card_no) return load(idx_[i], c);
    return false;
}

std::size_t Store::count() const {
    ensure();
    return n_;
}

Err Store::put(Card& c) {
    Card prev;
    bool had = false;
    return put(c, prev, had);
}

std::span<const std::uint8_t> Store::ext(std::uint16_t card_no) const {
    ensure();
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no == card_no) return ext_of(idx_[i]);
    return {};
}

Err Store::put(Card& c, Card& prev, bool& had, std::span<const std::uint8_t> blk) {
    had = false;
    if (c.id.fp == 0) return Err::Invalid;
    ensure();
    if (const int i = by_fp(c.id.fp); i >= 0) had = load(idx_[unsigned(i)], prev);
    c.times = 1; c.last_boots = 0; c.last_uptime_s = 0;
    std::uint32_t top_stamp = 0;
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].stamp > top_stamp) top_stamp = idx_[i].stamp;
    if (had && same_results(prev, c)) {
        fold_repeat(prev, c);
        return write(c, top_stamp + 1, blk.empty() ? ext_of(idx_[unsigned(by_fp(c.id.fp))]) : blk);
    }
    if (had) {
        c.card_no = prev.card_no;
        c.runs = prev.runs == 0xFFFF ? prev.runs : std::uint16_t(prev.runs + 1);
        inherit(c, prev);
        return write(c, top_stamp + 1, blk);
    }
    std::uint16_t top = 0;
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no > top) top = idx_[i].card_no;
    c.card_no = std::uint16_t(top + 1) ? std::uint16_t(top + 1) : std::uint16_t(1);
    c.runs = 1;
    const bool full = n_ >= kMaxCards;
    const std::uint32_t oldest = full ? idx_[lowest()].fp : 0;
    if (const Err e = write(c, top_stamp + 1, blk); e != Err::Ok) return e;
    if (full) gone(oldest);
    return Err::Ok;
}

Err Store::rename(std::uint16_t card_no, std::string_view name) {
    if (!valid_name(name)) return Err::Invalid;
    Card c;
    if (!get(card_no, c)) return Err::NotFound;
    set_name(c, name);
    c.name_auto = false;
    const Ent& e = idx_[unsigned(by_fp(c.id.fp))];
    return write(c, e.stamp, ext_of(e));
}

Err Store::remove(std::uint16_t card_no) {
    Card c;
    if (!get(card_no, c)) return Err::NotFound;
    return gone(c.id.fp);
}

bool Store::maintain(bool may_erase) {
    if (!may_erase || !ring_.scanned()) return false;
    if (const std::uint32_t d = ring_.first_dirty()) return ring_.erase(d);
    if (ring_.blank() >= kSpare) return false;
    ensure();
    const std::uint32_t v = victim();
    if (v == 0) return false;
    const bool tail = v == ring_.tail();
    bool ok = true;
    ring_.each_in(v, [&](const ringlog::Record& r) {
        const int i = keep(r, tail);
        if (i == kDrop) return true;
        const std::size_t n = carried(r, i);
        if (!ring_.append(r.type, r.payload().first(n), {}, true)) { ok = false; return false; }
        if (i >= 0) { idx_[unsigned(i)].at = ring_.last(); idx_[unsigned(i)].deep = n > kRecBytes; }
        return true;
    });
    if (!ok) { valid_ = false; return false; }
    return ring_.erase(v);
}

bool Store::needs_maintenance() const {
    return ring_.scanned() && (ring_.first_dirty() != 0 || ring_.blank() < kSpare);
}

int Store::by_fp(std::uint32_t fp) const {
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].fp == fp) return int(i);
    return -1;
}

bool Store::is_card(const ringlog::Record& r) {
    return r.type == kRecCard &&
           (r.len == kRecBytes || (r.len > kRecBytes && deep::blob_valid({r.data + kRecBytes, r.len - kRecBytes})));
}

bool Store::block_kept(int i) const {
    unsigned newer = 0;
    for (unsigned j = 0; j < n_; ++j) if (idx_[j].deep && idx_[j].stamp > idx_[unsigned(i)].stamp) ++newer;
    return newer < kMaxDeep;
}

std::size_t Store::carried(const ringlog::Record& r, int i) const {
    return i >= 0 && r.len > kRecBytes && !block_kept(i) ? kRecBytes : std::size_t(r.len);
}

int Store::keep(const ringlog::Record& r, bool tail) const {
    if (is_card(r)) {
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].at == r.at) return int(i);
        return kDrop;
    }
    return !tail && r.type == kRecGone && r.len == 4 && by_fp(detail::get32(r.data)) < 0 ? kGone : kDrop;
}

bool Store::fits(std::uint32_t s, bool slack, std::uint32_t& need) const {
    constexpr std::uint32_t kCap = ringlog::kSector - ringlog::kSectorHeader;
    std::uint32_t room = ring_.head_free();
    unsigned blank = ring_.blank();
    if (slack) {
        if (blank == 0) return false;
        --blank;
    }
    bool ok = true;
    need = 0;
    const auto place = [&](std::uint32_t b) {
        need += b;
        if (b > room) {
            if (blank == 0) { ok = false; return; }
            --blank;
            room = kCap;
        }
        room -= b;
    };
    const bool tail = s == ring_.tail();
    ring_.each_in(s, [&](const ringlog::Record& r) {
        if (const int i = keep(r, tail); i != kDrop) place(ringlog::record_bytes(std::uint32_t(carried(r, i))));
        return ok;
    }, false);
    return ok;
}

std::uint32_t Store::victim() const {
    const std::uint32_t head = ring_.head(), tail = ring_.tail();
    std::uint32_t need = 0;
    if (tail == 0 || tail == head) return 0;
    if (fits(tail, true, need)) return tail;
    for (const bool slack : {true, false}) {
        std::uint32_t best = 0, best_need = 0;
        for (unsigned i = 0; i < ring_.sectors(); ++i) {
            const std::uint32_t s = ring_.sector_addr(i);
            if (ring_.state(i) != ringlog::Sec::Live || s == head || !fits(s, slack, need)) continue;
            if (best == 0 || need < best_need) { best = s; best_need = need; }
        }
        if (best) return best;
    }
    return 0;
}

unsigned Store::lowest() const {
    unsigned lo = 0;
    for (unsigned i = 1; i < n_; ++i) if (idx_[i].stamp < idx_[lo].stamp) lo = i;
    return lo;
}

bool Store::load(const Ent& e, Card& c) const {
    const std::uint8_t* p = ring_view(e.at).p;
    if (!p) return false;
    if (!decode_card({record_of(p) + 4, kPayload}, c) || c.id.fp != e.fp) return false;
    migrate_to_current(c);
    c.card_no = e.card_no;
    return true;
}

Store::View Store::ring_view(std::uint32_t at) const {
    View v;
    ring_.each_in(at - (at - ring_.base()) % ringlog::kSector, [&](const ringlog::Record& r) {
        if (r.at != at) return true;
        if (is_card(r)) v = {r.data, r.len};
        return false;
    }, false);
    return v;
}

std::span<const std::uint8_t> Store::ext_of(const Ent& e) const {
    if (!e.deep) return {};
    const View v = ring_view(e.at);
    return v.p && v.len > kRecBytes ? std::span<const std::uint8_t>(v.p + kRecBytes, v.len - kRecBytes)
                                    : std::span<const std::uint8_t>();
}

void Store::ensure() const {
    if (valid_) return;
    n_ = 0;
    ring_.each([this](const ringlog::Record& r) {
        if (r.type == kRecGone && r.len == 4) drop_ent(detail::get32(r.data));
        else if (is_card(r)) index_card(r);
        return true;
    });
    renumber();
    valid_ = true;
}

[[gnu::noinline]] void Store::index_card(const ringlog::Record& r) const {
    Card c;
    if (!decode_card({record_of(r.data) + 4, kPayload}, c) || c.id.fp == 0) return;
    const Ent e{c.id.fp, detail::get32(r.data), r.at, c.card_no, c.format >= kFormat, r.len > kRecBytes};
    if (const int i = by_fp(e.fp); i >= 0) { idx_[unsigned(i)] = e; return; }
    if (n_ < kMaxCards) { idx_[n_++] = e; return; }
    const unsigned lo = lowest();
    if (e.stamp > idx_[lo].stamp) idx_[lo] = e;
}

void Store::drop_ent(std::uint32_t fp) const {
    const int i = by_fp(fp);
    if (i < 0) return;
    idx_[unsigned(i)] = idx_[--n_];
}

void Store::renumber() const {
    std::uint16_t top = 0;
    for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no > top) top = idx_[i].card_no;
    const Order o = order();
    std::array<std::uint8_t, kMaxCards> r{};
    unsigned m = 0;
    for (const bool cur : {true, false})
        for (unsigned i = 0; i < o.n; ++i) if (idx_[o.at[i]].current == cur) r[m++] = o.at[i];
    for (unsigned i = 1; i < m; ++i) {
        Ent& e = idx_[r[i]];
        for (unsigned j = 0; j < i; ++j)
            if (idx_[r[j]].card_no == e.card_no) {
                top = std::uint16_t(top + 1) ? std::uint16_t(top + 1) : top;
                e.card_no = top;
                break;
            }
    }
}

Store::Order Store::order() const {
    Order o;
    for (unsigned i = 0; i < n_; ++i) {
        unsigned j = o.n++;
        for (; j > 0 && idx_[o.at[j - 1]].stamp < idx_[i].stamp; --j) o.at[j] = o.at[j - 1];
        o.at[j] = std::uint8_t(i);
    }
    return o;
}

[[gnu::noinline]] Err Store::write(const Card& c, std::uint32_t stamp, std::span<const std::uint8_t> blk) {
    std::array<std::uint8_t, kRecBytes> b{};
    detail::put32(&b[0], stamp);
    encode_card(std::span<std::uint8_t>(b).subspan(4), c);
    bool with = !blk.empty() && deep::blob_valid(blk);
    if (!with || !ring_.append(kRecCard, b, blk)) {
        with = false;
        if (!ring_.append(kRecCard, b)) return ring_.scanned() ? Err::Full : Err::Flash;
    }
    const Ent e{c.id.fp, stamp, ring_.last(), c.card_no, true, with};
    if (const int i = by_fp(c.id.fp); i >= 0) idx_[unsigned(i)] = e;
    else if (n_ < kMaxCards) idx_[n_++] = e;
    else idx_[lowest()] = e;
    return Err::Ok;
}

Err Store::gone(std::uint32_t fp) {
    std::array<std::uint8_t, 4> b{};
    detail::put32(b.data(), fp);
    if (!ring_.append(kRecGone, b)) return ring_.scanned() ? Err::Full : Err::Flash;
    drop_ent(fp);
    return Err::Ok;
}

}  // namespace k2::report_card
