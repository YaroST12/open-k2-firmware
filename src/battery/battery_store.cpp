// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Battery efficiency recorder: the stored runs (/bt/<id>.rec) and the service that writes them and keeps the SRAM
// checkpoint. See k2/battery/battery.hpp.
#include <cstring>

#include "k2/battery/battery.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/ringlog.hpp"

namespace k2::battery {

namespace store {

Path path_of(std::uint16_t id) {
    Path p{'/', 'b', 't', '/'};
    char d[5];
    int n = 0;
    do { d[n++] = char('0' + id % 10); id /= 10; } while (id);
    std::size_t i = 4;
    while (n) p[i++] = d[--n];
    for (const char c : {'.', 'r', 'e', 'c'}) p[i++] = c;
    p[i] = 0;
    return p;
}

namespace {
// "<digits>.rec" -> the id; 0 for any other name.
std::uint16_t id_of(const char *name) {
    std::uint32_t v = 0;
    int digits = 0;
    for (; *name >= '0' && *name <= '9'; name++, digits++) v = v * 10 + std::uint32_t(*name - '0');
    if (digits == 0 || digits > 5 || v > 0xFFFF || std::strcmp(name, ".rec") != 0) return 0;
    return std::uint16_t(v);
}
}  // namespace

int list(fs::Fs &f, std::uint16_t *ids, int max) {
    int n = 0;
    f.list(kDir, [&](const fs::Info &i) {
        const std::uint16_t id = i.dir ? 0 : id_of(i.name);
        if (id == 0) return true;
        int at = n < max ? n : max;               // insertion into the newest-first window
        while (at > 0 && ids[at - 1] < id) at--;
        if (at >= max) return true;
        for (int k = (n < max ? n : max - 1); k > at; k--) ids[k] = ids[k - 1];
        ids[at] = id;
        if (n < max) n++;
        return true;
    });
    return n;
}

bool load(fs::Fs &f, std::uint16_t id, Record &r) {
    std::uint8_t buf[kRecordBytes];
    std::size_t n = 0;
    if (f.read_file(path_of(id).data(), buf, n) != fs::Err::Ok) return false;
    return decode(std::span<const std::uint8_t>(buf, n), r) && r.id == id;
}

bool save(fs::Fs &f, const Record &r) {
    std::uint8_t buf[kRecordBytes];
    if (encode(r, buf) != kRecordBytes || f.mkdir(kDir) != fs::Err::Ok) return false;
    return f.write_file(path_of(r.id).data(), buf) == fs::Err::Ok;
}

void prune(fs::Fs &f, int keep) {
    constexpr int kWindow = kKeep + 8;
    std::uint16_t ids[kWindow];
    const int n = list(f, ids, kWindow);
    for (int i = keep < 0 ? 0 : keep; i < n; i++) f.remove(path_of(ids[i]).data());
}

std::uint16_t next_id(fs::Fs &f) {
    std::uint16_t top = 0;
    list(f, &top, 1);
    return top == 0xFFFF ? 1 : std::uint16_t(top + 1);
}

}  // namespace store

namespace {
std::uint32_t crc_of(const Recorder &r) {
    std::uint8_t b[sizeof(Recorder)];
    std::memcpy(b, &r, sizeof b);
    return ringlog::crc16(0xFFFF, b);
}
}  // namespace

Service::Boot Service::boot(fs::Fs *f, Checkpoint *ckpt, bool warm) {
    fs_ = f && f->ready() ? f : nullptr;
    ckpt_ = ckpt;
    rec_ = Recorder{};
    if (warm && ckpt_ && ckpt_->magic == kMagic && ckpt_->crc == crc_of(ckpt_->rec)) {
        if (ckpt_->rec.active()) {
            std::memcpy(static_cast<void *>(&rec_), &ckpt_->rec, sizeof rec_);
            rec_.resumed();
            cfg_ = rec_.config();
            wiring_ = rec_.wiring();
            return Boot::ResumedRam;
        }
        if (ckpt_->rec.dirty()) {                 // a run ended and the reset came before its write
            std::memcpy(static_cast<void *>(&rec_), &ckpt_->rec, sizeof rec_);
            return Boot::Closed;
        }
    }
    if (!fs_) return Boot::Nothing;
    std::uint16_t id = 0;
    Record r;
    if (store::list(*fs_, &id, 1) == 0 || !store::load(*fs_, id, r) || r.state == RunState::Done) return Boot::Nothing;
    if (!rec_.restore(r)) return Boot::Nothing;
    cfg_ = rec_.config();
    wiring_ = rec_.wiring();
    return rec_.state() == State::Between ? Boot::ResumedBetween : Boot::Lost;
}

bool Service::flush() {
    if (!fs_) return false;
    Record r;
    rec_.to_record(r);
    if (!fs_->exists(store::path_of(r.id).data())) store::prune(*fs_, store::kKeep - 1);   // room first: at most kKeep files
    if (!store::save(*fs_, r)) return false;
    rec_.clean();
    return true;
}

void Service::checkpoint() {
    if (!ckpt_) return;
    std::memcpy(static_cast<void *>(&ckpt_->rec), &rec_, sizeof rec_);
    ckpt_->crc = crc_of(ckpt_->rec);
    ckpt_->magic = kMagic;
    ckpt_state_ = rec_.state();
}

void Service::poll(std::uint32_t now_ms, std::int32_t mv, std::int32_t ma, bool online, bool may_write) {
    may_write_ = may_write;
    if (rec_.active()) rec_.sample(now_ms, mv, ma, online);
    if (rec_.dirty() && fs_ && may_write && std::int32_t(now_ms - retry_ms_) >= 0) {
        failed_ = !flush();
        if (failed_) retry_ms_ = now_ms + 10'000;
    }
    // the checkpoint once a second, and at once when the run changes state (a warm reset never resumes an ended run)
    if (ckpt_ && (rec_.state() != ckpt_state_ || std::int32_t(now_ms - ckpt_ms_) >= 0)) {
        checkpoint();
        ckpt_ms_ = now_ms + 1000;
    }
}

Recorder::Start Service::start(Mode m, Wiring w, bool monitor_online) {
    if (rec_.active()) return Recorder::Start::Busy;
    if (!monitor_online) return Recorder::Start::NoMonitor;
    if (rec_.dirty() && fs_) {                    // the last run's record is not on flash yet
        if (!may_write_ || !flush()) return Recorder::Start::Busy;
    }
    std::uint16_t id = fs_ ? store::next_id(*fs_) : 1;
    if (rec_.id() >= id) id = std::uint16_t(rec_.id() + 1);
    if (id == 0) id = 1;
    const Recorder::Start s = rec_.start(m, w, cfg_, id);
    if (s == Recorder::Start::Ok) {
        wiring_ = w;
        ckpt_ms_ = 0;
        retry_ms_ = 0;
        failed_ = false;
    }
    return s;
}

}  // namespace k2::battery
