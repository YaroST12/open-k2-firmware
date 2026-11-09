// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// Open-K2 bootloader protocol (the FF 55 frames) and the .k2 firmware container,
// for the WebHID flasher page (webhid-flasher/index.html). Plain JS, no dependencies, no DOM.
// Mirrors tools/k2_flasher.py (the reference); byte vectors are shared with
// tests/test_k2boot.mjs and generated from that file. Run under Node:
//   node --test tests/test_k2boot.mjs
//
// Bootloader frame (64 bytes, report id 0 on the wire, prepended by WebHID):
//   FF 55 | t//1000, t%1000, reply? (t//30)|0x80 : (t//30)&0x7F, t//100, t%100, t//80
//   | cmd, len, payload[len] zero-padded to 52 | sum(frame[8:62]) | sum(frame[:62])
// Commands used here (tools/k2_flasher.py Protocol): 3 handshake (ack 2), 22 log reset,
// 5 begin erase/write session, 4 end session, 8 erase sector (no reply), 9 write memory
// (no reply, <= 40 data bytes), 11 read memory (replies in <= 40-byte chunks of cmd 11),
// 10 read marker, 23 reboot. Unsolicited cmd 21 is log noise, cmd 1 is an error.
var K2Boot = (function () {
  var FRAME_SIZE = 64, PAYLOAD_SIZE = 52, WRITE_SIZE = 40, HEADER_SIZE = 48;
  var FLASH_START = 0x08000000, APP_START = 0x08003800, INFO_START = 0x08003000, NAME_START = 0x08001400;
  var SECTOR_SIZE = 2048, SECTOR_COUNT = 447;
  var APP_LIMIT = APP_START + SECTOR_SIZE * SECTOR_COUNT;   // 0x080E3000: what the bootloader is known to serve
  var FS_START = 0x080B3000;      // our littlefs volume (link.ld); an image reaching it costs the volume, not the device
  var FLASH_END = 0x08100000;     // end of the 1 MB flash
  var READ_CHUNK = 1024;
  var NVRAM_START = 0x080E0000;   // the native data area (settings, e-marker records, assets): never erased or written
  var MAX_CONTAINER = NVRAM_START - APP_START;   // erase runs in 2 KB sectors from APP_START over the container length
  var SIG_OFFSET = 2064, K200 = [0x4B, 0x32, 0x30, 0x30];   // model signature word at 0x08004010, checked by the bootloader
  var MARKER_OFFSET = 3072, COMMIT_MARKER = [0xFE, 0xFF, 0xFF, 0xFF];
  var K2_GUID_LE = [34, 133, 63, 90, 107, 54, 204, 70, 170, 204, 219, 251, 226, 254, 224, 42];   // uuid bytes_le
  var CMD = { ERROR: 1, ACK: 2, HANDSHAKE: 3, END_SESSION: 4, BEGIN_SESSION: 5, ERASE: 8,
              WRITE: 9, READ_MARKER: 10, READ: 11, LOG: 21, LOG_RESET: 22, REBOOT: 23 };

  // 256-byte S-box of the .k2 container (tools/k2_flasher.py FIRMWARE_DECODE_TABLE).
  var DECODE_TABLE = Uint8Array.from([
    0x1a, 0xb6, 0xb7, 0xf1, 0x7b, 0x8c, 0xd5, 0x3c, 0x7c, 0x90, 0xd2, 0xf4, 0x35, 0xf2, 0x1b, 0xe9,
    0xdb, 0x12, 0x77, 0x5b, 0x3d, 0x7f, 0x3f, 0xbd, 0x2d, 0x47, 0x2a, 0x50, 0xdc, 0xc3, 0xff, 0x57,
    0x82, 0x83, 0x60, 0xfc, 0x9b, 0x4e, 0x88, 0xd0, 0x00, 0x5f, 0x2e, 0x59, 0x2b, 0xf9, 0xc6, 0x38,
    0xf0, 0xc8, 0x8e, 0x80, 0x44, 0xfd, 0x73, 0x7e, 0x0b, 0x29, 0x19, 0xaf, 0xd8, 0x96, 0x71, 0xb3,
    0x62, 0xe6, 0xe5, 0x11, 0x91, 0x9c, 0x46, 0x15, 0xc4, 0xc2, 0xa5, 0x52, 0xb9, 0x66, 0xac, 0xb2,
    0x22, 0x53, 0xee, 0x54, 0xaa, 0xb4, 0x74, 0xcf, 0xb0, 0x6c, 0x72, 0xa7, 0x92, 0x5a, 0x56, 0xe7,
    0x85, 0xec, 0xc1, 0x9f, 0xce, 0x6e, 0xae, 0x3b, 0x78, 0x8f, 0x5d, 0x4b, 0xe2, 0x64, 0x0c, 0x4c,
    0x13, 0x63, 0xc0, 0x41, 0x4f, 0xba, 0xe4, 0x10, 0x04, 0xa6, 0x3e, 0x31, 0xe1, 0xb8, 0x48, 0x67,
    0x05, 0xd3, 0x98, 0x69, 0xbf, 0x36, 0x65, 0xbe, 0x06, 0x51, 0x5c, 0xc7, 0x8b, 0x09, 0x37, 0xc5,
    0xa9, 0xa8, 0x70, 0xd4, 0x87, 0x1d, 0xda, 0x0d, 0xf3, 0x76, 0x30, 0xad, 0x17, 0x94, 0xe0, 0xb1,
    0x14, 0xed, 0x0e, 0x08, 0x42, 0x9a, 0x33, 0xb5, 0x55, 0xd9, 0x45, 0x39, 0xe3, 0x7d, 0x4d, 0x01,
    0xcb, 0x27, 0xef, 0x25, 0x6a, 0x3a, 0x7a, 0x79, 0x6d, 0x16, 0x40, 0x61, 0xa4, 0x02, 0xab, 0x86,
    0x0f, 0x34, 0x6f, 0xeb, 0x6b, 0xf7, 0x28, 0xde, 0xa3, 0xcc, 0x75, 0x2c, 0xca, 0x5e, 0x81, 0x0a,
    0xfa, 0x68, 0xf8, 0x07, 0x03, 0x24, 0xcd, 0x1f, 0xa2, 0x2f, 0x32, 0x43, 0x9d, 0xf5, 0xd7, 0xbc,
    0x9e, 0xbb, 0x49, 0x4a, 0xd6, 0x8a, 0x84, 0xa1, 0x21, 0xdf, 0x97, 0xfe, 0x20, 0xd1, 0x1e, 0x93,
    0x1c, 0x8d, 0xfb, 0x26, 0x18, 0x95, 0xf6, 0xc9, 0xdd, 0xea, 0x99, 0x23, 0xe8, 0xa0, 0x58, 0x89
  ]);
  var ENCODE_TABLE = new Uint8Array(256);
  for (var i = 0; i < 256; i++) ENCODE_TABLE[DECODE_TABLE[i]] = i;

  function ProtocolError(message) {
    this.name = "ProtocolError"; this.message = message;
  }
  ProtocolError.prototype = Object.create(Error.prototype);

  function nowMs() {
    if (typeof performance !== "undefined" && performance.now) return Math.floor(performance.now());
    return Date.now() & 0xFFFFFFFF;
  }

  // Builds one 64-byte bootloader frame. `tick` (ms, monotonic) is injectable for tests.
  function buildFrame(command, payload, opts) {
    opts = opts || {};
    payload = payload || new Uint8Array(0);
    if (!(command >= 0 && command <= 255) || payload.length > PAYLOAD_SIZE)
      throw new ProtocolError("Invalid command (" + command + ") or payload length (" + payload.length + ")");
    var reply = opts.reply !== false;
    var t = (opts.tick === undefined ? nowMs() : opts.tick) >>> 0;
    var f = new Uint8Array(FRAME_SIZE);
    f[0] = 0xFF; f[1] = 0x55;
    f[2] = Math.floor(t / 1000) & 0xFF; f[3] = t % 1000;
    f[4] = reply ? ((Math.floor(t / 30) | 0x80) & 0xFF) : (Math.floor(t / 30) & 0x7F);
    f[5] = Math.floor(t / 100) & 0xFF; f[6] = t % 100; f[7] = Math.floor(t / 80) & 0xFF;
    f[8] = command; f[9] = payload.length;
    f.set(payload, 10);
    var inner = 0, outer = 0;
    for (var k = 8; k < 62; k++) inner = (inner + f[k]) & 0xFF;
    for (k = 0; k < 62; k++) outer = (outer + f[k]) & 0xFF;
    f[62] = inner; f[63] = outer;
    return f;
  }

  function parseFrame(raw) {
    if (raw.length !== FRAME_SIZE) throw new ProtocolError("Frame length " + raw.length + " != " + FRAME_SIZE);
    if (raw[0] !== 0xFF || raw[1] !== 0x55) throw new ProtocolError("Invalid magic header (expected FF 55)");
    if (raw[9] > PAYLOAD_SIZE) throw new ProtocolError("Payload length exceeds 52");
    var inner = 0, outer = 0, k;
    for (k = 8; k < 62; k++) inner = (inner + raw[k]) & 0xFF;
    for (k = 0; k < 62; k++) outer = (outer + raw[k]) & 0xFF;
    if (raw[62] !== inner) throw new ProtocolError("Inner checksum mismatch");
    if (raw[63] !== outer) throw new ProtocolError("Outer checksum mismatch");
    return { command: raw[8], payload: raw.slice(10, 10 + raw[9]), raw: raw };
  }

  function isBootloaderFrame(raw) { return raw.length > 1 && raw[0] === 0xFF && raw[1] === 0x55; }

  // ---- .k2 firmware container (tools/k2_flasher.py Firmware) ------------------------------------

  function translate(bytes, table) {
    var out = new Uint8Array(bytes.length);
    for (var i = 0; i < bytes.length; i++) out[i] = table[bytes[i]];
    return out;
  }
  function rd16(b, o) { return (b[o] | (b[o + 1] << 8)) >>> 0; }
  function rd32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }
  function wr16(v) { return [(v & 0xFF), ((v >>> 8) & 0xFF)]; }
  function wr32(v) { return [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF]; }
  function xorReduce(b) { var x = 0; for (var i = 0; i < b.length; i++) x = (x ^ b[i]) >>> 0; return x; }
  function sum32(b) { var s = 0; for (var i = 0; i < b.length; i++) s = (s + b[i]) >>> 0; return s >>> 0; }

  function Firmware(source, app, dateStr, version) {
    this.source = source; this.app = app; this.dateStr = dateStr; this.version = version;
  }

  Firmware.parse = function (raw) {
    if (raw.length < HEADER_SIZE) throw new ProtocolError("File is smaller than 48-byte header");
    if (raw.length > SECTOR_SIZE * SECTOR_COUNT) throw new ProtocolError("File exceeds maximum K2 flash sector capacity");
    if (raw.length > MAX_CONTAINER)
      throw new ProtocolError("File (" + raw.length + " B) would be erased into the native data area at 0x" +
                              NVRAM_START.toString(16) + " (limit " + MAX_CONTAINER + " B)");
    var d = translate(raw, DECODE_TABLE);
    var magic = String.fromCharCode.apply(null, d.slice(0, 7));
    if (magic !== "gzutapp") throw new ProtocolError("Invalid firmware magic (expected 'gzutapp')");
    var year = rd16(d, 7), month = d[9], day = d[10];
    var dateStr = year + "-" + (month < 10 ? "0" : "") + month + "-" + (day < 10 ? "0" : "") + day;
    var guidOk = true;
    for (var i = 0; i < 16; i++) if (d[20 + i] !== K2_GUID_LE[i]) { guidOk = false; break; }
    if (!guidOk) throw new ProtocolError("Firmware GUID does not match K2 GUID");
    var size = rd32(d, 36), xorVal = rd32(d, 40), checksum = rd32(d, 44);
    var app = d.slice(HEADER_SIZE);
    if (size !== app.length) throw new ProtocolError("Declared app size " + size + " != actual length " + app.length);
    if (size < MARKER_OFFSET + 4) throw new ProtocolError("App payload lacks version or commit marker area");
    if (xorReduce(app) !== xorVal) throw new ProtocolError("Firmware XOR verification failed");
    if (sum32(app) !== checksum) throw new ProtocolError("Firmware additive checksum verification failed");
    if (app[MARKER_OFFSET] !== 0xFF || app[MARKER_OFFSET + 1] !== 0xFF ||
        app[MARKER_OFFSET + 2] !== 0xFF || app[MARKER_OFFSET + 3] !== 0xFF)
      throw new ProtocolError("Firmware commit marker is not uncommitted (FFFFFFFF)");
    var sp = rd32(app, 0), reset = rd32(app, 4);
    if (!(sp > 0x20000000 && sp <= 0x20040000 && sp % 4 === 0))
      throw new ProtocolError("Invalid Cortex-M stack pointer: 0x" + sp.toString(16));
    if (!(reset & 1) || !(APP_START <= (reset & ~1) && (reset & ~1) < APP_START + size))
      throw new ProtocolError("Reset vector 0x" + reset.toString(16) + " outside flash application range");
    var verByte = app[2048];
    return new Firmware(raw.slice(0), app, dateStr, (verByte >> 4) + "." + (verByte & 15));
  };

  // Wraps a raw app binary (Cortex-M vector table at 0) into a .k2 container.
  Firmware.fromApp = function (app, dateStr, versionStr) {
    versionStr = versionStr || "1.0";
    var buf = new Uint8Array(Math.max(app.length, MARKER_OFFSET + 4));
    buf.set(app);
    for (var i = app.length; i < buf.length; i++) buf[i] = 0xFF;
    if (buf.length % 4 !== 0) {
      var grown = new Uint8Array(buf.length + (4 - buf.length % 4));
      grown.set(buf);
      for (i = buf.length; i < grown.length; i++) grown[i] = 0xFF;
      buf = grown;
    }
    if (buf[2048] === 0xFF) {
      var parts = versionStr.split(".");
      var major = parseInt(parts[0], 10) & 0x0F;
      var minor = parts.length > 1 ? (parseInt(parts[1], 10) & 0x0F) : 0;
      if (isNaN(major)) major = 1;
      buf[2048] = ((isNaN(major) ? 1 : major) << 4) | (isNaN(minor) ? 0 : minor);
    }
    buf[MARKER_OFFSET] = 0xFF; buf[MARKER_OFFSET + 1] = 0xFF; buf[MARKER_OFFSET + 2] = 0xFF; buf[MARKER_OFFSET + 3] = 0xFF;
    // Trim trailing 0xFF beyond MARKER_OFFSET + 4 (same rule as k2_flasher.py).
    var last = MARKER_OFFSET + 4;
    for (i = buf.length - 1; i >= MARKER_OFFSET + 4; i--) if (buf[i] !== 0xFF) { last = i + 1; break; }
    var clean = buf.slice(0, Math.max(MARKER_OFFSET + 4, Math.ceil(last / 4) * 4));
    var header = new Uint8Array(HEADER_SIZE);
    var magic = "gzutapp";
    for (i = 0; i < 7; i++) header[i] = magic.charCodeAt(i);
    var dp = dateStr ? dateStr.split("-").map(Number) : null;
    var y = dp ? dp[0] : 1970, mo = dp ? dp[1] : 1, dy = dp ? dp[2] : 1;
    header.set(wr16(y), 7); header[9] = mo & 0xFF; header[10] = dy & 0xFF;
    for (i = 0; i < 9; i++) header[11 + i] = 0xFF;
    header.set(K2_GUID_LE, 20);
    header.set(wr32(clean.length), 36);
    header.set(wr32(xorReduce(clean)), 40);
    header.set(wr32(sum32(clean)), 44);
    var all = new Uint8Array(HEADER_SIZE + clean.length);
    all.set(header, 0); all.set(clean, HEADER_SIZE);
    return Firmware.parse(translate(all, ENCODE_TABLE));
  };

  // Accepts a .k2 container or a raw app .bin (detected by its vector table).
  Firmware.load = function (bytes) {
    if (bytes.length >= 8) {
      var sp = rd32(bytes, 0), reset = rd32(bytes, 4);
      if (sp > 0x20000000 && sp <= 0x20040000 && (reset & 1) && APP_START <= (reset & ~1)) return Firmware.fromApp(bytes);
    }
    return Firmware.parse(bytes);
  };

  Firmware.prototype.eraseAddresses = function () {
    var count = Math.ceil(this.source.length / SECTOR_SIZE);
    var out = [];
    for (var i = 0; i < count; i++) out.push(APP_START + i * SECTOR_SIZE);
    return out;
  };
  Firmware.prototype.chunks = function (size) {
    size = size || 1024;
    var out = [];
    for (var off = 0; off < this.app.length; off += size)
      out.push({ addr: APP_START + off, data: this.app.slice(off, off + size) });
    return out;
  };

  // ---- bootloader link ---------------------------------------------------------------------------
  // Transport: {send(Uint8Array) -> Promise|void}; the owner calls feed(report) for every IN report.
  function BootLink(transport, opts) {
    opts = opts || {};
    this.transport = transport;
    this.timeoutMs = opts.timeoutMs || 2000;
    this.pending = null;
    this.onLog = null;
  }

  BootLink.prototype.feed = function (raw) {
    var pkt;
    try { pkt = parseFrame(raw); } catch (e) { return; }   // not a bootloader frame: ignore
    if (pkt.command === CMD.LOG) { if (this.onLog) this.onLog(pkt.payload); return; }
    var e = this.pending;
    if (!e) return;
    if (pkt.command === CMD.ERROR) { this._finish(e, new ProtocolError("Device rejected request: " + hex(pkt.payload))); return; }
    if (pkt.command !== e.expect) return;
    clearTimeout(e.timer);
    e.resolve(pkt.payload);   // the handler either finishes via _finish or re-arms the timer itself
  };

  BootLink.prototype._finish = function (e, err, value) {
    if (!e || this.pending !== e) return;
    clearTimeout(e.timer);
    this.pending = null;
    if (err) e.reject(err); else e.resolve(value);
  };

  BootLink.prototype._send = function (command, payload, reply) {
    return this.transport.send(buildFrame(command, payload, { reply: reply !== false }));
  };

  // Sends a command and waits for the ack (cmd 2), draining log noise first.
  BootLink.prototype.command = function (command, payload) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var e = { expect: CMD.ACK, reject: reject, timer: 0,
                resolve: function (payload) { clearTimeout(e.timer); self.pending = null; resolve(payload); } };
      self.pending = e;
      e.timer = setTimeout(function () { self._finish(e, new ProtocolError("Timed out waiting for ack of 0x" + command.toString(16))); }, self.timeoutMs);
      Promise.resolve().then(function () { return self._send(command, payload, true); })
        .catch(function (err) { self._finish(e, err); });
    });
  };

  BootLink.prototype.eraseSector = function (address) {
    return this._send(CMD.ERASE, Uint8Array.from(wr32(address)), false);
  };

  BootLink.prototype.writeMemory = function (address, data) {
    var self = this;
    var p = Promise.resolve();
    for (var off = 0; off < data.length; off += WRITE_SIZE) {
      (function (chunk, at) {
        p = p.then(function () {
          var payload = Uint8Array.from(wr32(address + at).concat([chunk.length]).concat(Array.from(chunk)));
          return self._send(CMD.WRITE, payload, false);
        });
      })(data.slice(off, off + WRITE_SIZE), off);
    }
    return p;
  };

  BootLink.prototype.readMemory = function (address, size) {
    var self = this;
    if (!(size >= 1 && size <= 1024)) throw new ProtocolError("Read size must be between 1 and 1024 bytes");
    return new Promise(function (resolve, reject) {
      var result = new Uint8Array(size), got = 0;
      function finish(err) {
        clearTimeout(e.timer);
        if (self.pending === e) self.pending = null;
        if (err) reject(err); else resolve(result);
      }
      var e = {
        expect: CMD.READ, reject: reject, timer: 0,
        resolve: function (chunk) {           // one cmd-11 chunk arrived
          var expected = Math.min(WRITE_SIZE, size - got);
          if (chunk.length !== expected) { finish(new ProtocolError("Read chunk length " + chunk.length + " != expected " + expected)); return; }
          result.set(chunk, got); got += chunk.length;
          if (got >= size) finish(null);
          else e.timer = setTimeout(function () { self._finish(e, new ProtocolError("Timed out waiting for read data")); }, self.timeoutMs);
        },
      };
      self.pending = e;
      e.timer = setTimeout(function () { self._finish(e, new ProtocolError("Timed out waiting for read data")); }, self.timeoutMs);
      Promise.resolve().then(function () {
        return self._send(CMD.READ, Uint8Array.from(wr32(address).concat(wr16(size))), true);
      }).catch(finish);
    });
  };

  BootLink.prototype.readMarker = function (address) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var e = { expect: CMD.READ_MARKER, reject: reject, timer: 0,
                resolve: function (payload) {
                  clearTimeout(e.timer); self.pending = null;
                  if (payload.length !== 4) reject(new ProtocolError("Marker read length error"));
                  else resolve(payload);
                } };
      self.pending = e;
      e.timer = setTimeout(function () { self._finish(e, new ProtocolError("Timed out waiting for marker")); }, self.timeoutMs);
      Promise.resolve().then(function () {
        return self._send(CMD.READ_MARKER, Uint8Array.from(wr32(address).concat([4])), true);
      }).catch(function (err) { self._finish(e, err); });
    }).then(function (payload) {
      if (payload.length !== 4) throw new ProtocolError("Marker read length error");
      return payload;
    });
  };

  function hex(bytes) {
    var s = "";
    for (var i = 0; i < bytes.length; i++) s += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
    return s;
  }

  // Handshake + bootloader strings + installed version and marker (tools/k2_flasher.py K2Updater._identify).
  // Throws unless the strings name a K2. `info` is the raw 1 KB identity block (hash it for a manifest).
  BootLink.prototype.identify = async function (stage) {
    stage = stage || function () {};
    stage("handshake");
    await this.command(CMD.HANDSHAKE);
    stage("identify");
    var info = await this.readMemory(INFO_START, 1024);
    var names = await this.readMemory(NAME_START, 256);
    var parts = [], cur = [];
    for (var i = 0; i < names.length; i++) {
      if (names[i] === 0) { parts.push(cur); cur = []; if (parts.length === 4) break; }
      else cur.push(names[i]);
    }
    if (parts.length !== 4) throw new ProtocolError("Bootloader strings missing terminators");
    var strings = parts.slice(0, 3).map(function (p) { return new TextDecoder("utf-8").decode(Uint8Array.from(p)).trim(); });
    var confirmed = strings.some(function (s) {
      var u = s.toUpperCase();
      return u === "K2" || u === "WITRN K2" || u === "K2(C)WITRN";
    });
    if (!confirmed) throw new ProtocolError("Connected hardware not confirmed as K2: " + JSON.stringify(strings));
    var verByte = (await this.readMemory(APP_START + 2048, 1))[0];
    var marker = await this.readMarker(APP_START + MARKER_OFFSET);
    return { bootStrings: strings, info: info, currentVersion: (verByte >> 4) + "." + (verByte & 15), currentMarker: hex(marker) };
  };

  // Full flash sequence (tools/k2_flasher.py K2Updater.flash). progress(stage, cur, tot).
  BootLink.prototype.flash = async function (fw, opts) {
    opts = opts || {};
    var progress = opts.progress || function () {};
    var stage = function (name, cur, tot) { progress(name, cur || 0, tot || 0); };

    var ident = await this.identify(stage);

    stage("log-reset");
    await this.command(CMD.LOG_RESET);

    var sectors = fw.eraseAddresses();
    stage("erase", 0, sectors.length);
    await this.command(CMD.BEGIN_SESSION);
    for (var s = 0; s < sectors.length; s++) {
      await this.eraseSector(sectors[s]);
      stage("erase", s + 1, sectors.length);
    }
    await this.command(CMD.END_SESSION);

    var chunks = fw.chunks(1024);
    var totalBytes = fw.app.length;
    stage("write", 0, totalBytes);
    await this.command(CMD.BEGIN_SESSION);
    for (var c = 0; c < chunks.length; c++) {
      await this.writeMemory(chunks[c].addr, chunks[c].data);
      stage("write", chunks[c].addr - APP_START + chunks[c].data.length, totalBytes);
    }
    await this.command(CMD.END_SESSION);

    stage("verify", 0, totalBytes);
    for (c = 0; c < chunks.length; c++) {
      var actual = await this.readMemory(chunks[c].addr, chunks[c].data.length);
      for (i = 0; i < actual.length; i++) {
        if (actual[i] !== chunks[c].data[i]) {
          throw new ProtocolError("Verification mismatch at flash address 0x" +
            (chunks[c].addr + i).toString(16).padStart(8, "0"));
        }
      }
      stage("verify", chunks[c].addr - APP_START + chunks[c].data.length, totalBytes);
    }

    stage("commit");
    await this.command(CMD.BEGIN_SESSION);
    await this.writeMemory(APP_START + MARKER_OFFSET, Uint8Array.from(COMMIT_MARKER));
    var back = await this.readMarker(APP_START + MARKER_OFFSET);
    if (hex(back) !== hex(Uint8Array.from(COMMIT_MARKER))) throw new ProtocolError("Commit marker verification failed: got " + hex(back));
    await this.command(CMD.END_SESSION);

    if (opts.autoReboot !== false) {
      stage("reboot");
      await this.command(CMD.REBOOT);
    }
    stage("complete", totalBytes, totalBytes);
    return ident;
  };

  // ---- backup (tools/k2_flasher.py K2Updater.backup) ---------------------------------------------

  async function sha256Hex(bytes) {
    var subtle = typeof crypto !== "undefined" && crypto.subtle;
    if (!subtle) throw new ProtocolError("SHA-256 is not available (crypto.subtle needs https or localhost)");
    return hex(new Uint8Array(await subtle.digest("SHA-256", bytes)));
  }
  function addr8(a) { return "0x" + a.toString(16).toUpperCase().padStart(8, "0"); }

  // Reads a chunk at or above APP_LIMIT, where the bootloader may refuse. Resolves {data, complete}.
  // A failed read is repeated once (a timeout is not yet a refusal); on a second failure a fresh session is
  // started (handshake, best effort) and the chunk is bisected in 4-byte steps: `data` is the readable
  // prefix, `complete` is false when the chunk was cut short.
  BootLink.prototype.readAboveApp = async function (address, size) {
    var isProto = function (e) { return e instanceof ProtocolError; };
    for (var attempt = 0; attempt < 2; attempt++) {
      try { return { data: await this.readMemory(address, size), complete: true }; }
      catch (e) { if (!isProto(e)) throw e; }
    }
    try { await this.command(CMD.HANDSHAKE); } catch (e) { if (!isProto(e)) throw e; }
    var lo = 0, hi = size, best = new Uint8Array(0);   // hi: known unreadable
    try { best = await this.readMemory(address, 4); lo = 4; }
    catch (e) { if (!isProto(e)) throw e; return { data: new Uint8Array(0), complete: false }; }
    while (hi - lo > 4) {
      var mid = lo + (Math.floor((hi - lo) / 2) & ~3);
      try { best = await this.readMemory(address, mid); lo = mid; }
      catch (e) { if (!isProto(e)) throw e; hi = mid; }
    }
    return { data: best, complete: false };
  };

  // The application the recovery container carries. Entering the bootloader from the app (DFU_ENTER) clears the
  // model signature word "K200" at SIG_OFFSET (0x08004010) so the bootloader stays in DFU; a backup taken after
  // that holds 00000000 there and an image built from it fails the bootloader's boot check. A zeroed word in an
  // image with a valid vector table is put back; any other value is refused, not guessed.
  // Returns {app, restored}; the dump itself (app.bin, flash.bin) stays as read.
  function recoveryApp(app) {
    var word = app.slice(SIG_OFFSET, SIG_OFFSET + 4);
    var intact = true, zero = true;
    for (var i = 0; i < 4; i++) { if (word[i] !== K200[i]) intact = false; if (word[i] !== 0) zero = false; }
    if (intact) return { app: app, restored: false };
    if (!zero) throw new ProtocolError("Model signature at " + addr8(APP_START + SIG_OFFSET) + " is " +
                                       hex(word).toUpperCase() + ", neither K200 nor 00000000: no recovery image built");
    var sp = rd32(app, 0), reset = rd32(app, 4);
    if (!(sp > 0x20000000 && sp <= 0x20040000 && sp % 4 === 0) || !(reset & 1) ||
        !(reset - 1 >= APP_START && reset - 1 < APP_START + app.length))
      throw new ProtocolError("Model signature is zero but the vector table (SP 0x" + sp.toString(16) + ", reset 0x" +
                              reset.toString(16) + ") is not an application's: no recovery image built");
    var fixed = app.slice();
    fixed.set(K200, SIG_OFFSET);
    return { app: fixed, restored: true };
  }

  // ---- ZIP (store only: no compression, no library) ------------------------------------------------
  // makeZip packs [{name, data}] into one archive; readZip reads one back (stored entries, and deflated ones
  // when the runtime has DecompressionStream, so a re-zipped backup still restores).

  var crcTable = null;
  function crc32(bytes) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        crcTable[n] = c >>> 0;
      }
    }
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  function makeZip(entries, when) {
    when = when || new Date();
    var enc = new TextEncoder();
    var year = Math.max(1980, when.getUTCFullYear());
    var dosTime = (when.getUTCHours() << 11) | (when.getUTCMinutes() << 5) | (when.getUTCSeconds() >> 1);
    var dosDate = ((year - 1980) << 9) | ((when.getUTCMonth() + 1) << 5) | when.getUTCDate();
    var parts = [], central = [], offset = 0;
    entries.forEach(function (e) {
      var name = enc.encode(e.name), data = e.data, crc = crc32(data);
      var lh = new Uint8Array(30 + name.length), lv = new DataView(lh.buffer);
      lv.setUint32(0, 0x04034B50, true);
      lv.setUint16(4, 20, true);              // version needed
      lv.setUint16(6, 0x0800, true);          // names are UTF-8
      lv.setUint16(8, 0, true);               // method: stored
      lv.setUint16(10, dosTime, true); lv.setUint16(12, dosDate, true);
      lv.setUint32(14, crc, true); lv.setUint32(18, data.length, true); lv.setUint32(22, data.length, true);
      lv.setUint16(26, name.length, true);
      lh.set(name, 30);
      var ch = new Uint8Array(46 + name.length), cv = new DataView(ch.buffer);
      cv.setUint32(0, 0x02014B50, true);
      cv.setUint16(4, (3 << 8) | 20, true);   // made by: Unix, 2.0
      cv.setUint16(6, 20, true);
      cv.setUint16(8, 0x0800, true);
      cv.setUint16(10, 0, true);
      cv.setUint16(12, dosTime, true); cv.setUint16(14, dosDate, true);
      cv.setUint32(16, crc, true); cv.setUint32(20, data.length, true); cv.setUint32(24, data.length, true);
      cv.setUint16(28, name.length, true);
      cv.setUint32(38, (0o100644 << 16) >>> 0, true);   // regular file, rw-r--r--
      cv.setUint32(42, offset, true);
      ch.set(name, 46);
      central.push(ch);
      parts.push(lh, data);
      offset += lh.length + data.length;
    });
    var cdSize = central.reduce(function (s, c) { return s + c.length; }, 0);
    var eocd = new Uint8Array(22), ev = new DataView(eocd.buffer);
    ev.setUint32(0, 0x06054B50, true);
    ev.setUint16(8, entries.length, true); ev.setUint16(10, entries.length, true);
    ev.setUint32(12, cdSize, true); ev.setUint32(16, offset, true);
    var all = parts.concat(central, [eocd]);
    var out = new Uint8Array(offset + cdSize + 22), at = 0;
    all.forEach(function (p) { out.set(p, at); at += p.length; });
    return out;
  }

  function isZip(bytes) {
    return bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4B &&
           ((bytes[2] === 3 && bytes[3] === 4) || (bytes[2] === 5 && bytes[3] === 6));
  }

  async function inflateRaw(data) {
    if (typeof DecompressionStream === "undefined") throw new ProtocolError("this ZIP is compressed and this browser cannot unpack it");
    var stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function readZip(bytes) {
    if (!isZip(bytes)) throw new ProtocolError("not a ZIP file");
    var dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    var eocd = -1;
    for (var i = bytes.length - 22; i >= 0 && i >= bytes.length - 22 - 65535; i--) {
      if (dv.getUint32(i, true) === 0x06054B50) { eocd = i; break; }
    }
    if (eocd < 0) throw new ProtocolError("not a ZIP file (no end-of-archive record)");
    var count = dv.getUint16(eocd + 10, true), p = dv.getUint32(eocd + 16, true);
    var dec = new TextDecoder(), out = [];
    for (var n = 0; n < count; n++) {
      if (p + 46 > bytes.length || dv.getUint32(p, true) !== 0x02014B50) throw new ProtocolError("ZIP directory is damaged");
      var method = dv.getUint16(p + 10, true), crc = dv.getUint32(p + 16, true);
      var csize = dv.getUint32(p + 20, true), usize = dv.getUint32(p + 24, true);
      var nameLen = dv.getUint16(p + 28, true), extraLen = dv.getUint16(p + 30, true), commentLen = dv.getUint16(p + 32, true);
      var local = dv.getUint32(p + 42, true);
      var name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
      p += 46 + nameLen + extraLen + commentLen;
      if (name.charAt(name.length - 1) === "/") continue;
      if (local + 30 > bytes.length || dv.getUint32(local, true) !== 0x04034B50) throw new ProtocolError("ZIP entry " + name + " is damaged");
      var start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
      var raw = bytes.subarray(start, start + csize);
      var data;
      if (method === 0) data = raw.slice();
      else if (method === 8) data = await inflateRaw(raw);
      else throw new ProtocolError("ZIP entry " + name + " uses compression method " + method + ", which is not supported");
      if (data.length !== usize || crc32(data) !== crc) throw new ProtocolError("ZIP entry " + name + " fails its CRC check");
      out.push({ name: name, data: data });
    }
    return out;
  }

  // The recovery container inside a K2_BACKUP ZIP: {name, data} of its K2_RECOVERY_*.k2.
  async function recoveryFromBackupZip(bytes) {
    var entries = await readZip(bytes);
    for (var i = 0; i < entries.length; i++) {
      if (/^K2_RECOVERY_.*\.k2$/.test(entries[i].name)) return entries[i];
    }
    throw new ProtocolError("no K2_RECOVERY_*.k2 in this ZIP");
  }

  // Two-pass flash backup of the whole 1 MB, 0x08000000..0x08100000: bootloader, application area and the device
  // storage from NVRAM_START up. The part above APP_LIMIT is best effort: if the bootloader refuses there the
  // readable part is kept and the manifest says where it stopped (complete: false, backup_kind "partial"). A
  // failure below APP_LIMIT is an error. opts.appOnly reads 0x08000000..APP_LIMIT only (backup_kind "app-only").
  // Resolves {ident, flash, bootloader, app, nvram, recovery (Firmware), recoveryName, manifest, manifestText,
  // zip, zipName, files}; the caller offers the ZIP (everything in one file, K2_RECOVERY_<ver>.k2 inside is the
  // one to flash back) and, secondarily, the single files.
  // opts: {appOnly, full (accepted, the default), progress(stage, cur, tot), now (Date, for tests)}.
  BootLink.prototype.backup = async function (opts) {
    opts = opts || {};
    var progress = opts.progress || function () {};
    var stage = function (name, cur, tot) { progress(name, cur || 0, tot || 0); };
    var appOnly = opts.appOnly === true || opts.full === false;

    var ident = await this.identify(stage);
    var infoSha = await sha256Hex(ident.info);
    var wantEnd = appOnly ? APP_LIMIT : FLASH_END;
    var total = wantEnd - FLASH_START;
    var buf = new Uint8Array(total);
    var readEnd = wantEnd, refusedAt = null;

    stage("backup-read", 0, total);
    for (var off = 0; off < total; off += READ_CHUNK) {
      var addr = FLASH_START + off, n = Math.min(READ_CHUNK, total - off), chunk, ok = true;
      if (addr < APP_LIMIT) chunk = await this.readMemory(addr, n);
      else { var r = await this.readAboveApp(addr, n); chunk = r.data; ok = r.complete; }
      buf.set(chunk, off);
      if (!ok) { readEnd = addr + chunk.length; refusedAt = readEnd; break; }
      stage("backup-read", off + chunk.length, total);
    }

    var readSize = readEnd - FLASH_START;
    var flash = buf.slice(0, readSize);
    stage("backup-verify", 0, readSize);
    for (off = 0; off < readSize; off += READ_CHUNK) {
      var again = await this.readMemory(FLASH_START + off, Math.min(READ_CHUNK, readSize - off));
      for (var i = 0; i < again.length; i++) {
        if (again[i] !== flash[off + i]) throw new ProtocolError("Backup verification mismatch at " + addr8(FLASH_START + off + i));
      }
      stage("backup-verify", off + again.length, readSize);
    }

    var bootloader = flash.slice(0, APP_START - FLASH_START);
    var app = flash.slice(APP_START - FLASH_START, APP_LIMIT - FLASH_START);
    var nvram = flash.length > NVRAM_START - FLASH_START ? flash.slice(NVRAM_START - FLASH_START) : null;
    ident.currentVersion = (app[2048] >> 4) + "." + (app[2048] & 15);
    ident.currentMarker = hex(app.slice(MARKER_OFFSET, MARKER_OFFSET + 4));

    stage("backup-recovery");
    // the recovery container carries the application code only, APP_START..FS_START (the linker script's FLASH
    // range). Above it sit user data: the littlefs volume (kept in app.bin/flash.bin) and the native device
    // storage (NVRAM_START up, kept in `nvram`). On hardware the volume reaches its last block after some use, so
    // trimming trailing 0xFF no longer shortens the image below NVRAM_START and the erase span of a container
    // that long would run into the native storage; a container that ends at FS_START also leaves the volume
    // intact when it is flashed back.
    var now = opts.now || new Date();
    var rec = recoveryApp(app.slice(0, FS_START - APP_START));
    var recovery = Firmware.fromApp(rec.app, now.toISOString().slice(0, 10));
    var recoveryName = "K2_RECOVERY_" + recovery.version + ".k2";

    var manifest = {
      timestamp_utc: now.toISOString(),
      identity: { boot_strings: ident.bootStrings, info_sha256: infoSha, confirmed_k2: true,
                  current_version: ident.currentVersion, current_marker: ident.currentMarker },
      mode: appOnly ? "app" : "full",
      backup_kind: appOnly ? "app-only" : (refusedAt === null ? "full" : "partial"),
      requested_range: addr8(FLASH_START) + "-" + addr8(wantEnd),
      flash_range: addr8(FLASH_START) + "-" + addr8(readEnd),
      complete: refusedAt === null,
      last_readable_address: addr8(readEnd - 1),
      refused_at: refusedAt === null ? null : addr8(refusedAt),
      flash_size: flash.length,
      flash_sha256: await sha256Hex(flash),
      app_sha256: await sha256Hex(app),
      two_pass_verified: true,
      recovery_file: recoveryName,
      recovery_range: addr8(APP_START) + "-" + addr8(FS_START),
      signature_restored: rec.restored,
      tool: "webhid-flasher"
    };
    if (nvram) {
      manifest.native_nvram_file = "native_nvram.bin";
      manifest.native_nvram_sha256 = await sha256Hex(nvram);
    }
    var manifestText = JSON.stringify(manifest, null, 2) + "\n";
    var enc = new TextEncoder();
    var files = [{ name: recoveryName, data: recovery.source }, { name: "flash.bin", data: flash },
                 { name: "app.bin", data: app }, { name: "bootloader.bin", data: bootloader }];
    if (nvram) files.push({ name: "native_nvram.bin", data: nvram });
    files.push({ name: "manifest.json", data: enc.encode(manifestText) });
    var zipName = "K2_BACKUP_" + ident.currentVersion + "_" + now.toISOString().replace(/[-:]/g, "").slice(0, 15) + "Z.zip";
    stage("backup-complete", readSize, readSize);
    return { ident: ident, flash: flash, bootloader: bootloader, app: app, nvram: nvram, recovery: recovery,
             recoveryName: recoveryName, manifest: manifest, manifestText: manifestText,
             files: files, zip: makeZip(files, now), zipName: zipName };
  };

  return {
    FRAME_SIZE: FRAME_SIZE, PAYLOAD_SIZE: PAYLOAD_SIZE, WRITE_SIZE: WRITE_SIZE, HEADER_SIZE: HEADER_SIZE,
    FLASH_START: FLASH_START, APP_START: APP_START, INFO_START: INFO_START, NAME_START: NAME_START,
    SECTOR_SIZE: SECTOR_SIZE, SECTOR_COUNT: SECTOR_COUNT, APP_LIMIT: APP_LIMIT, FS_START: FS_START,
    NVRAM_START: NVRAM_START, MAX_CONTAINER: MAX_CONTAINER, FLASH_END: FLASH_END, READ_CHUNK: READ_CHUNK,
    MARKER_OFFSET: MARKER_OFFSET, COMMIT_MARKER: COMMIT_MARKER, CMD: CMD,
    DECODE_TABLE: DECODE_TABLE, ProtocolError: ProtocolError,
    buildFrame: buildFrame, parseFrame: parseFrame, isBootloaderFrame: isBootloaderFrame,
    crc32: crc32, makeZip: makeZip, readZip: readZip, isZip: isZip, recoveryFromBackupZip: recoveryFromBackupZip,
    Firmware: Firmware, BootLink: BootLink, hex: hex
  };
})();
if (typeof module !== "undefined" && module.exports) module.exports = K2Boot;
