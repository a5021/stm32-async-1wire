/* 1-Wire browser visualizer core (no DOM, no dependencies).
 * Port of the validated Python slot decoder (probe.sr: bit-exact vs sigrok
 * onewire_link): falling edge starts a slot, low >= 300 us is a Reset,
 * otherwise the bit is the line level 20 us after the edge; next search
 * starts 55 us later. Presence = a >= 30 us low pulse within 200 us after
 * the reset release. All thresholds scale with the samplerate.
 *
 * .sr layout (sigrok session): zip with central directory; entries
 * "metadata" (text: samplerate, unitsize) and "logic-1*" (unitsize bytes
 * per sample, bit N = channel DN). Compression is raw deflate (or stored).
 */
"use strict";

const OWVis = {};

OWVis.FAMILY = { 0x28: "DS18B20", 0x2d: "DS2431", 0x3a: "DS2413" };

/** Dallas/Maxim CRC-8 (poly 0x8C reversed) over byte array. */
OWVis.crc8 = function (bytes) {
  let crc = 0;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] & 0xff;
    for (let b = 0; b < 8; b++) {
      crc = crc & 1 ? (crc >> 1) ^ 0x8c : crc >> 1;
    }
  }
  return crc & 0xff;
};

/** Reflected Maxim 1-Wire CRC-16 (polynomial 0xA001). */
OWVis.crc16 = function (bytes) {
  let crc = 0;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] & 0xff;
    for (let b = 0; b < 8; b++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc & 0xffff;
};

OWVis.familyName = function (code) {
  return OWVis.FAMILY[code & 0xff] || "unknown";
};

/* ---------- raw DEFLATE fallback (no DecompressionStream) ---------- */

/**
 * Inflate one raw DEFLATE stream (RFC 1951) without browser APIs.
 * Used when DecompressionStream is missing or fails; expectedSize is the
 * uncompressed size from the zip central directory (0 = unknown).
 */
OWVis.inflateRaw = function (input, expectedSize) {
  const src = input;
  let bitPos = 0;
  const totalBits = src.length * 8;
  const readBit = () => {
    if (bitPos >= totalBits) throw new Error("inflate: unexpected end of stream");
    const v = (src[bitPos >> 3] >> (bitPos & 7)) & 1;
    bitPos++;
    return v;
  };
  const readBits = (n) => {
    let v = 0;
    for (let k = 0; k < n; k++) v |= readBit() << k;
    return v;
  };
  const alignByte = () => { bitPos = (bitPos + 7) & ~7; };

  const expected = expectedSize > 0 ? expectedSize : 0;
  let out = new Uint8Array(expected || Math.max(1024, src.length * 4));
  let outLen = 0;
  const ensure = (extra) => {
    if (outLen + extra <= out.length) return;
    let next = out.length || 1024;
    while (next < outLen + extra) next *= 2;
    const grown = new Uint8Array(next);
    grown.set(out.subarray(0, outLen));
    out = grown;
  };

  // Canonical Huffman decoder (the classic puff algorithm).
  const build = (lengths, maxBits) => {
    const counts = new Uint16Array(maxBits + 1);
    for (let i = 0; i < lengths.length; i++) if (lengths[i]) counts[lengths[i]]++;
    const offs = new Uint16Array(maxBits + 2);
    for (let len = 1; len <= maxBits; len++) offs[len + 1] = offs[len] + counts[len];
    const symbols = new Uint16Array(lengths.length);
    const cursor = offs.slice();
    for (let sym = 0; sym < lengths.length; sym++) {
      const len = lengths[sym];
      if (len) symbols[cursor[len]++] = sym;
    }
    return { counts, symbols, maxBits };
  };
  const decode = (h) => {
    let code = 0, first = 0, index = 0;
    for (let len = 1; len <= h.maxBits; len++) {
      code |= readBit();
      const count = h.counts[len];
      if (code - first < count) return h.symbols[index + (code - first)];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw new Error("inflate: bad Huffman code");
  };

  const LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577];
  const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

  const inflateBlock = (litHuff, distHuff) => {
    for (;;) {
      const sym = decode(litHuff);
      if (sym < 256) {
        ensure(1);
        out[outLen++] = sym;
      } else if (sym === 256) {
        return;
      } else if (sym >= 257 && sym <= 285) {
        const li = sym - 257;
        const len = LEN_BASE[li] + readBits(LEN_EXTRA[li]);
        const dsym = decode(distHuff);
        if (dsym >= 30) throw new Error("inflate: bad distance code");
        const dist = DIST_BASE[dsym] + readBits(DIST_EXTRA[dsym]);
        if (dist > outLen) throw new Error("inflate: distance beyond output");
        ensure(len);
        for (let k = 0; k < len; k++) {
          out[outLen] = out[outLen - dist];
          outLen++;
        }
      } else {
        throw new Error("inflate: bad literal/length code");
      }
    }
  };

  const fixedLit = new Uint8Array(288);
  let sym = 0;
  for (; sym <= 143; sym++) fixedLit[sym] = 8;
  for (; sym <= 255; sym++) fixedLit[sym] = 9;
  for (; sym <= 279; sym++) fixedLit[sym] = 7;
  for (; sym <= 287; sym++) fixedLit[sym] = 8;
  const fixedDist = new Uint8Array(32).fill(5);
  let fixedLitHuff = null, fixedDistHuff = null;

  let final = 0;
  while (!final) {
    final = readBit();
    const type = readBits(2);
    if (type === 0) {
      alignByte();
      const len = readBits(16);
      const nlen = readBits(16);
      if (((~len) & 0xffff) !== nlen) throw new Error("inflate: bad stored length");
      const off = bitPos >> 3;
      if (off + len > src.length) throw new Error("inflate: stored block past input");
      ensure(len);
      out.set(src.subarray(off, off + len), outLen);
      outLen += len;
      bitPos += len * 8;
    } else if (type === 1) {
      if (!fixedLitHuff) {
        fixedLitHuff = build(fixedLit, 15);
        fixedDistHuff = build(fixedDist, 15);
      }
      inflateBlock(fixedLitHuff, fixedDistHuff);
    } else if (type === 2) {
      const hlit = readBits(5) + 257;
      const hdist = readBits(5) + 1;
      const hclen = readBits(4) + 4;
      const order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
      const clLens = new Uint8Array(19);
      for (let k = 0; k < hclen; k++) clLens[order[k]] = readBits(3);
      const clHuff = build(clLens, 7);
      const lens = new Uint8Array(hlit + hdist);
      let n = 0;
      while (n < lens.length) {
        const s = decode(clHuff);
        if (s < 16) {
          lens[n++] = s;
        } else if (s === 16) {
          if (n === 0) throw new Error("inflate: repeat with no previous length");
          const prev = lens[n - 1];
          let rep = 3 + readBits(2);
          while (rep-- && n < lens.length) lens[n++] = prev;
        } else if (s === 17) {
          let rep = 3 + readBits(3);
          while (rep-- && n < lens.length) lens[n++] = 0;
        } else if (s === 18) {
          let rep = 11 + readBits(7);
          while (rep-- && n < lens.length) lens[n++] = 0;
        } else {
          throw new Error("inflate: bad code-length symbol");
        }
      }
      inflateBlock(build(lens.subarray(0, hlit), 15), build(lens.subarray(hlit), 15));
    } else {
      throw new Error("inflate: reserved block type");
    }
  }
  if (expected && outLen !== expected) {
    throw new Error("inflate: size mismatch (" + outLen + " != " + expected + ")");
  }
  return out.slice(0, outLen);
};

/* ---------- minimal zip reader (via central directory) ---------- */

function u16(d, o) {
  return d[o] | (d[o + 1] << 8);
}
function u32(d, o) {
  return (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
}

OWVis.unzip = async function (buf) {
  const d = new Uint8Array(buf);
  let eocd = -1;
  for (let i = d.length - 22; i >= 0; i--) {
    if (d[i] === 0x50 && d[i + 1] === 0x4b && d[i + 2] === 0x05 && d[i + 3] === 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error("not a zip/.sr file (EOCD missing)");
  const count = u16(d, eocd + 10);
  const cdOff = u32(d, eocd + 16);
  const out = {};
  let o = cdOff;
  for (let k = 0; k < count; k++) {
    if (d[o] !== 0x50 || d[o + 1] !== 0x4b || d[o + 2] !== 0x01 || d[o + 3] !== 0x02) {
      throw new Error("bad central directory entry " + k);
    }
    const method = u16(d, o + 10);
    const cs = u32(d, o + 20);
    const us = u32(d, o + 24);
    const nl = u16(d, o + 28);
    const el = u16(d, o + 30);
    const cl = u16(d, o + 32);
    const lho = u32(d, o + 42);
    let name = "";
    for (let i = 0; i < nl; i++) name += String.fromCharCode(d[o + 46 + i]);
    const lnl = u16(d, lho + 26);
    const lel = u16(d, lho + 28);
    const start = lho + 30 + lnl + lel;
    const comp = d.slice(start, start + cs);
    let raw;
    if (method === 0) {
      raw = comp;
    } else if (method === 8) {
      const streamInflate = async () => {
        const ds = new DecompressionStream("deflate-raw");
        const w = ds.writable.getWriter();
        w.write(comp);
        w.close();
        const chunks = [];
        const r = ds.readable.getReader();
        for (;;) {
          const { done, value } = await r.read();
          if (done) break;
          chunks.push(value);
        }
        let len = 0;
        for (const c of chunks) len += c.length;
        const raw = new Uint8Array(len);
        let p = 0;
        for (const c of chunks) {
          raw.set(c, p);
          p += c.length;
        }
        return raw;
      };
      if (typeof DecompressionStream === "function") {
        try {
          raw = await streamInflate();
          if (us && raw.length !== us) throw new Error("unexpected inflated size");
        } catch (e) {
          // Older Safari / some embedded webviews: pure-JS fallback.
          raw = OWVis.inflateRaw(comp, us);
        }
      } else {
        raw = OWVis.inflateRaw(comp, us);
      }
    } else {
      throw new Error("unsupported zip method " + method + " for " + name);
    }
    out[name] = raw;
    o += 46 + nl + el + cl;
  }
  return out;
};

function parseMeta(text) {
  const m = { samplerate: 0, unitsize: 1, probes: 8 };
  for (const line of text.split("\n")) {
    const t = line.trim();
    if (t.startsWith("samplerate=")) {
      const v = t.split("=")[1].trim().split(" ");
      m.samplerate = parseFloat(v[0]) * (v[1] && v[1][0] === "M" ? 1e6 : v[1] && v[1][0] === "k" ? 1e3 : 1);
    } else if (t.startsWith("unitsize=")) {
      m.unitsize = parseInt(t.split("=")[1], 10);
    } else if (t.startsWith("total probes=")) {
      m.probes = parseInt(t.split("=")[1], 10);
    }
  }
  return m;
}

/** Parse a sigrok .sr ArrayBuffer -> {samplerate, unitsize, channel, samples}. */
OWVis.parseSR = async function (buf, channel) {
  const files = await OWVis.unzip(buf);
  const names = Object.keys(files);
  const metaName = names.find((n) => n === "metadata");
  const logicNames = names.filter((n) => n.startsWith("logic-1")).sort();
  if (!metaName || !logicNames.length) throw new Error(".sr has no metadata/logic entries");
  const meta = parseMeta(new TextDecoder().decode(files[metaName]));
  let samples = files[logicNames[0]];
  if (logicNames.length > 1) {
    let total = 0;
    for (const n of logicNames) total += files[n].length;
    const cat = new Uint8Array(total);
    let p = 0;
    for (const n of logicNames) {
      cat.set(files[n], p);
      p += files[n].length;
    }
    samples = cat;
  }
  return { samplerate: meta.samplerate, unitsize: meta.unitsize, probes: meta.probes, samples, channel: channel || 0 };
};

/* ---------- slot decoder (validated vs sigrok onewire_link) ---------- */

/**
 * Detect the bus speed from the longest low pulse in the capture.
 * Standard reset lows are >= 300 us; overdrive resets are >= 30 us while
 * overdrive slots stay <= ~10 us. A capture that starts mid-slot (no reset)
 * falls back to standard, since an OD reset cannot be proven.
 */
OWVis.detectSpeed = function (samples, samplerate, channel) {
  const ch = channel || 0;
  const us = samplerate / 1e6;
  const stdReset = Math.max(2, Math.round(300 * us));
  const odReset = Math.max(2, Math.round(30 * us));
  let sawOd = false;
  for (let i = 0; i < samples.length - 1; i++) {
    if (((samples[i] >> ch) & 1) === 1 && ((samples[i + 1] >> ch) & 1) === 0) {
      let j = i + 1;
      while (j < samples.length && ((samples[j] >> ch) & 1) === 0) j++;
      const low = j - (i + 1);
      if (low >= stdReset) return "standard";
      if (low >= odReset) sawOd = true;
      i = j - 1;
    }
  }
  return sawOd ? "overdrive" : "standard";
};

function decodeSlotsProfile(samples, samplerate, channel, overdrive) {
  const ch = channel || 0;
  const n = samples.length; // unitsize=1 assumed (fx2lafw 8ch)
  const us = samplerate / 1e6; // samples per microsecond
  const lvl = (i) => (samples[i] >> ch) & 1;
  const ev = [];
  let i = 0;
  const RESET_LOW = Math.max(2, Math.round((overdrive ? 24 : 300) * us));
  const SAMPLE_AT = Math.max(1, Math.round((overdrive ? 3 : 20) * us));
  const ADVANCE = Math.max(2, Math.round((overdrive ? 5 : 55) * us));
  const PRES_WIN = Math.round((overdrive ? 40 : 200) * us);
  const PRES_LOW = Math.max(1, Math.round((overdrive ? 4 : 30) * us));
  const SKIP_AFTER_RESET = Math.round(400 * us);
  while (i < n - 1) {
    if (lvl(i) === 1 && lvl(i + 1) === 0) {
      let j = i + 1;
      while (j < n && lvl(j) === 0) j++;
      const low = j - (i + 1);
      if (low >= RESET_LOW) {
        let pres = false, presT = 0, presLen = 0;
        let e = j;
        const end = Math.min(n, j + PRES_WIN);
        while (e < end - 1) {
          if (lvl(e) === 1 && lvl(e + 1) === 0) {
            let f = e + 1;
            while (f < n && lvl(f) === 0) f++;
            if (f - (e + 1) >= PRES_LOW) {
              pres = true;
              presT = e + 1;
              presLen = f - (e + 1);
              break;
            }
            e = f;
          } else {
            e++;
          }
        }
        ev.push({ t: i + 1, type: "reset", presence: pres, presT, presLen, lowLen: low });
        // At overdrive speed the first slot can start ~15 us after release,
        // so resume right after the presence pulse instead of skipping 400 us.
        i = overdrive ? (pres ? presT + presLen : j) : j + SKIP_AFTER_RESET;
      } else {
        const t = i + 1 + SAMPLE_AT;
        ev.push({ t: i + 1, type: "bit", bit: t < n ? lvl(t) : 1, lowLen: low });
        i = i + 1 + ADVANCE;
      }
    } else {
      i++;
    }
  }
  return ev;
}

OWVis.decodeSlots = function (samples, samplerate, channel, speed) {
  let mode = speed || "auto";
  if (mode === "auto") mode = OWVis.detectSpeed(samples, samplerate, channel);
  return decodeSlotsProfile(samples, samplerate, channel, mode === "overdrive");
};

OWVis.SLOT_PITCH_US = 120;  // fallback start-to-start period for a lone slot
OWVis.SLOT_MAX_US = 500;    // never shade across a host idle gap

OWVis.buildTransactions = function (events, samplerate) {
  const rate = samplerate > 0 ? samplerate : 1e6;
  const us = rate / 1e6;
  const defaultPitch = Math.max(1, Math.round(OWVis.SLOT_PITCH_US * us));
  const maxSpan = Math.max(defaultPitch, Math.round(OWVis.SLOT_MAX_US * us));
  const txns = [];
  let cur = null;
  for (const e of events) {
    if (e.type === "reset") {
      cur = { presence: e.presence, tReset: e.t, resetLowLen: e.lowLen || 0, presT: e.presT || 0, presLen: e.presLen || 0, bits: [] };
      txns.push(cur);
    } else {
      if (!cur) {
        cur = { presence: null, tReset: null, bits: [] };
        txns.push(cur);
      }
      cur.bits.push(e);
    }
  }
  txns.forEach((txn) => {
    // The last slot has no successor; estimate its period from the median
    // start-to-start spacing of the preceding slots instead of a fixed count
    // of samples (which was wrong at every samplerate but 1 MHz).
    const deltas = [];
    for (let i = 1; i < txn.bits.length; i++) {
      const d = txn.bits[i].t - txn.bits[i - 1].t;
      if (d > 0) deltas.push(d);
    }
    deltas.sort((a, b) => a - b);
    const pitch = deltas.length ? deltas[deltas.length >> 1] : defaultPitch;
    txn.bits.forEach((slot, i) => {
      const next = i + 1 < txn.bits.length ? txn.bits[i + 1].t : slot.t + pitch;
      slot.endT = Math.min(next, slot.t + maxSpan);
      slot.role = "unknown";
      slot.phase = "unclassified";
      slot.byteIndex = -1;
      slot.bitIndex = i % 8;
      slot.tripletPart = "";
    });
    annotateRoles(txn);
  });
  return txns;
};

/** Add protocol-aware slot ownership to a transaction. */
function annotateRoles(txn) {
  const b = OWVis.bytesOf(txn.bits);
  if (!b.length) return;
  const set = (start, end, role, phase) => {
    for (let i = Math.max(0, start); i < Math.min(txn.bits.length, end); i++) {
      txn.bits[i].role = role;
      txn.bits[i].phase = phase;
      txn.bits[i].byteIndex = (i - start) >> 3;
      txn.bits[i].bitIndex = (i - start) & 7;
    }
  };
  const byteValue = (bitOffset) => {
    if (bitOffset + 8 > txn.bits.length) return null;
    let v = 0;
    for (let k = 0; k < 8; k++) v |= txn.bits[bitOffset + k].bit << k;
    return v;
  };

  const romCommand = b[0].value;
  if (romCommand === 0xf0 || romCommand === 0xec) {
    set(0, 8, "master-write", "ROM command");
    for (let i = 0; i < 64; i++) {
      const start = 8 + i * 3;
      for (let p = 0; p < 3 && start + p < txn.bits.length; p++) {
        const slot = txn.bits[start + p];
        slot.role = p < 2 ? "slave-read" : "master-write";
        slot.phase = p === 0 ? "Search ROM · ID" : p === 1 ? "Search ROM · complement" : "Search ROM · direction";
        slot.byteIndex = (i / 8) | 0;
        slot.bitIndex = i & 7;
        slot.tripletPart = p === 0 ? "id" : p === 1 ? "complement" : "direction";
      }
    }
    return;
  }

  if (romCommand === 0x55) {
    set(0, 8, "master-write", "ROM command");
    set(8, 72, "master-write", "Match ROM address");
  } else if (romCommand === 0xcc) {
    set(0, 8, "master-write", "ROM command");
  } else {
    // Raw/single-device transaction: first byte is a master command.
    set(0, 8, "master-write", "command");
    return;
  }

  const family = romCommand === 0x55 && b.length >= 9 ? b[1].value : 0;
  const fnOffset = romCommand === 0x55 ? 72 : 8;
  const fn = byteValue(fnOffset);
  if (fn == null) return;
  set(fnOffset, fnOffset + 8, "master-write", "function command");
  const dataStart = fnOffset + 8;

  if (family === 0x28) { // DS18B20
    if (fn === 0xbe) set(dataStart, txn.bits.length, "slave-read", "DS18B20 scratchpad reply");
    else if (fn === 0xb4) set(dataStart, dataStart + 1, "slave-read", "Read Power Supply reply");
    else if (fn === 0x4e) set(dataStart, txn.bits.length, "master-write", "DS18B20 scratchpad payload");
  } else if (family === 0x2d) { // DS2431
    if (fn === 0xf0) {
      set(dataStart, dataStart + 16, "master-write", "DS2431 address");
      set(dataStart + 16, txn.bits.length, "slave-read", "DS2431 memory data");
    } else if (fn === 0x0f) set(dataStart, txn.bits.length, "master-write", "DS2431 scratchpad payload");
    else if (fn === 0xaa) set(dataStart, txn.bits.length, "slave-read", "DS2431 scratchpad reply");
    else if (fn === 0x55) {
      // Copy Scratchpad: TA1/TA2/ES are still driven by the master; after the
      // tPROG hold-off the device answers with one completion byte (AA/FF).
      set(dataStart, dataStart + 24, "master-write", "DS2431 copy address");
      set(dataStart + 24, txn.bits.length, "slave-read", "DS2431 copy completion");
    }
  } else if (family === 0x3a) { // DS2413
    if (fn === 0xf5) set(dataStart, txn.bits.length, "slave-read", "DS2413 PIO reply");
    else if (fn === 0x5a) {
      set(dataStart, Math.min(dataStart + 16, txn.bits.length), "master-write", "DS2413 PIO data/complement");
      set(dataStart + 16, txn.bits.length, "slave-read", "DS2413 acknowledgement");
    }
  } else if (romCommand === 0xcc) { // broadcast: family is not encoded
    const info = OWVis.broadcastInfo(fn);
    if (info && info.role !== "unknown") {
      set(dataStart, txn.bits.length, info.role, "broadcast " + info.name);
    }
  }
}

OWVis.bytesOf = function (bits) {
  const out = [];
  for (let k = 0; k + 8 <= bits.length; k += 8) {
    let v = 0;
    for (let b = 0; b < 8; b++) v |= (bits[k + b].bit & 1) << b;
    out.push({ value: v, t0: bits[k].t, t1: bits[k + 7].t });
  }
  return out;
};

/** Decode Search ROM triplets (bytes[0] must be 0xF0). */
OWVis.searchROMs = function (bytes) {
  const bits = [];
  for (let k = 1; k < bytes.length; k++) {
    for (let b = 0; b < 8; b++) bits.push((bytes[k].value >> b) & 1);
  }
  const roms = [];
  let pos = 0;
  while (pos + 192 <= bits.length) {
    const rom = [0, 0, 0, 0, 0, 0, 0, 0];
    let valid = true;
    for (let i = 0; i < 64; i++) {
      const a = bits[pos + i * 3];
      const c = bits[pos + i * 3 + 1];
      const dd = bits[pos + i * 3 + 2];
      if (a === 1 && c === 1) valid = false;
      if (a !== c && dd !== a) valid = false;
      if (dd) rom[(i / 8) | 0] |= 1 << (i % 8);
    }
    // A 192-bit all-ones run is trailing filler, not a device ROM. Do not
    // report it as an all-FF device with a failed CRC.
    if (rom.every((v) => v === 0xff)) break;
    // verify: recompute CRC over the ROM itself
    const crcOk = OWVis.crc8(rom.slice(0, 7)) === rom[7];
    roms.push({ rom, tripletsValid: valid, crcOk });
    pos += 192;
    // stop at trailing idle (all-ones filler after the last pass)
    let rest = true;
    for (let k = pos; k < Math.min(pos + 192, bits.length); k++) {
      if (bits[k] !== 1) {
        rest = false;
        break;
      }
    }
    if (rest && roms.length > 0) {
      // check whether remaining bits are all ones (filler)
      let allOne = true;
      for (let k = pos; k < bits.length; k++) {
        if (bits[k] !== 1) {
          allOne = false;
          break;
        }
      }
      if (allOne) break;
    }
  }
  return roms;
};

const CMD = {
  0xf0: "Search ROM",
  0xec: "Alarm Search",
  0x55: "Match ROM",
  0xcc: "Skip ROM",
  0x44: "Convert T",
  0xbe: "Read Scratchpad",
  0x4e: "Write Scratchpad",
  0xa5: "Reserved",
  0xf5: "PIO Access Read",
  0x5a: "PIO Access Write",
  0x0f: "Write Scratchpad",
  0xaa: "Read Scratchpad",
};

OWVis.cmdName = function (b) {
  return CMD[b & 0xff] || "0x" + (b & 0xff).toString(16).padStart(2, "0").toUpperCase();
};

OWVis.functionName = function (family, command) {
  const perFamily = {
    0x28: { 0x44: "Convert T", 0xbe: "Read Scratchpad", 0x4e: "Write Scratchpad", 0xb4: "Read Power Supply", 0x48: "Copy EEPROM" },
    0x2d: { 0xf0: "Read Memory", 0x0f: "Write Scratchpad", 0xaa: "Read Scratchpad", 0x55: "Copy Scratchpad" },
    0x3a: { 0xf5: "PIO Access Read", 0x5a: "PIO Access Write" },
  };
  return (perFamily[family] && perFamily[family][command]) || OWVis.cmdName(command);
};

/**
 * Function commands that may follow Skip ROM. On a mixed bus the family is
 * not encoded in the frame, so keep the family in the label instead of
 * silently assuming DS18B20.
 */
OWVis.BROADCAST = {
  0x44: { name: "Convert T", family: "DS18B20", role: "master-write" },
  0xbe: { name: "Read Scratchpad", family: "DS18B20", role: "slave-read" },
  0x4e: { name: "Write Scratchpad", family: "DS18B20", role: "master-write" },
  0xb4: { name: "Read Power Supply", family: "DS18B20", role: "slave-read" },
  0x48: { name: "Copy Scratchpad", family: "DS18B20", role: "unknown" },
  0x0f: { name: "Write Scratchpad", family: "DS2431", role: "master-write" },
  0xaa: { name: "Read Scratchpad", family: "DS2431", role: "slave-read" },
  0x55: { name: "Copy Scratchpad", family: "DS2431", role: "unknown" },
  0xf0: { name: "Read Memory", family: "DS2431", role: "slave-read" },
  0xf5: { name: "PIO Access Read", family: "DS2413", role: "slave-read" },
  0x5a: { name: "PIO Access Write", family: "DS2413", role: "master-write" },
};

OWVis.broadcastInfo = function (command) {
  return OWVis.BROADCAST[command & 0xff] || null;
};

/** Human summary of one transaction (bytes + presence). */
OWVis.interpret = function (txn) {
  const bytes = OWVis.bytesOf(txn.bits);
  if (!bytes.length) {
    return { bytes, kind: "empty", note: txn.presence ? "presence only" : "no presence" };
  }
  const first = bytes[0].value;
  if (first === 0xf0 || first === 0xec) {
    const roms = OWVis.searchROMs(bytes);
    const fams = [...new Set(roms.map((r) => OWVis.familyName(r.rom[0])))];
    return {
      bytes,
      kind: first === 0xf0 ? "search" : "alarm",
      note: roms.length + " device(s)" + (fams.length ? ": " + fams.join(", ") : ""),
      roms,
      command: first,
    };
  }
  if (first === 0x55 && bytes.length >= 9) {
    const rom = bytes.slice(1, 9).map((b) => b.value);
    const fn = bytes[9] ? bytes[9].value : null;
    return {
      bytes,
      kind: "match",
      note: OWVis.familyName(rom[0]) + " " + rom.map((v) => v.toString(16).padStart(2, "0").toUpperCase()).join(" ") +
        (fn !== null ? " · " + OWVis.functionName(rom[0], fn) + " (0x" + fn.toString(16).padStart(2, "0").toUpperCase() + ")" : ""),
      rom,
      romCrcOk: OWVis.crc8(rom) === 0,
      functionCommand: bytes[9] ? bytes[9].value : null,
      functionName: bytes[9] ? OWVis.functionName(rom[0], bytes[9].value) : "",
      crc: rom[0] === 0x2d && bytes[9] && bytes[9].value === 0xaa
        ? OWVis.ds2431ScratchpadMark(bytes, 9) : OWVis.scratchpadMark(bytes, 9),
    };
  }
  if (first === 0xcc) {
    const fn = bytes[1] ? bytes[1].value : null;
    const info = fn === null ? null : OWVis.broadcastInfo(fn);
    const fnName = fn === null ? "" : (info ? info.name : OWVis.cmdName(fn));
    const note = fn === null ? "Skip ROM"
      : info ? "Skip ROM · " + fnName + " · " + info.family +
        " (0x" + fn.toString(16).padStart(2, "0").toUpperCase() + ")"
      : "Skip ROM · function 0x" + fn.toString(16).padStart(2, "0").toUpperCase();
    return {
      bytes,
      kind: "skip",
      note,
      rom: null,
      functionCommand: fn,
      functionName: fnName,
      broadcast: info,
      crc: fn === 0xaa ? OWVis.ds2431ScratchpadMark(bytes, 1) : OWVis.scratchpadMark(bytes, 1),
    };
  }
  const shown = bytes.slice(0, 16).map((b) => "0x" + b.value.toString(16).padStart(2, "0").toUpperCase()).join(" ");
  return {
    bytes,
    kind: "raw",
    note: shown + (bytes.length > 16 ? " …(+" + (bytes.length - 16) + " bytes)" : ""),
    rom: null,
    crc: null,
  };
};

/** Group adjacent search passes and same-ROM addressed transactions. */
OWVis.groupTransactions = function (txns, decoded) {
  const groups = [];
  for (let i = 0; i < txns.length;) {
    const d = decoded[i];
    let j = i + 1;
    let type = "transaction";
    if (d.kind === "search" || d.kind === "alarm") {
      type = d.kind;
      const seenPass = new Set((d.roms || []).map((r) => r.rom.join(",")));
      while (j < txns.length && decoded[j].kind === type) {
        const nextRoms = decoded[j].roms || [];
        // A ROM leaf repeated inside one uninterrupted Search ROM sequence
        // means the host has started a fresh search pass set (or the capture
        // contains a repeated scan); split the groups at that boundary.
        if (nextRoms.some((r) => seenPass.has(r.rom.join(",")))) break;
        nextRoms.forEach((r) => seenPass.add(r.rom.join(",")));
        j++;
      }
    } else if (d.kind === "match" && d.rom) {
      const key = d.rom.join(",");
      while (j < txns.length && decoded[j].kind === "match" && decoded[j].rom && decoded[j].rom.join(",") === key) j++;
      if (j - i > 1) type = "device-chain";
    }
    const indices = [];
    for (let k = i; k < j; k++) indices.push(k);
    const romMap = new Map();
    for (const k of indices) {
      for (const found of decoded[k].roms || []) {
        const key = found.rom.join(",");
        if (!romMap.has(key)) romMap.set(key, { ...found, pass: k });
      }
      if (decoded[k].kind === "match" && decoded[k].rom) {
        const key = decoded[k].rom.join(",");
        if (!romMap.has(key)) romMap.set(key, { rom: decoded[k].rom, crcOk: decoded[k].romCrcOk, tripletsValid: true, pass: k });
      }
    }
    groups.push({ id: i, type, indices, roms: [...romMap.values()] });
    i = j;
  }
  return groups;
};

/**
 * Find a DS18B20 Read Scratchpad reply (command 0xBE followed by 9 bytes)
 * and check its trailing CRC-8. Returns {index, ok} with index into the
 * decoded byte array, or null when there is no 9-byte reply to check.
 */
OWVis.scratchpadMark = function (bytes, commandIndex) {
  const k = commandIndex | 0;
  if (!bytes[k] || bytes[k].value !== 0xbe) return null;
  if (k + 10 > bytes.length) return { index: -1, ok: false, partial: true };
  const data = bytes.slice(k + 1, k + 10).map((b) => b.value);
  return { index: k + 9, indices: [k + 9], width: 1, algorithm: "CRC-8", ok: OWVis.crc8(data.slice(0, 8)) === data[8] };
};

/** Verify DS2431 Read Scratchpad frame CRC-16 (inverted on wire, low byte first). */
OWVis.ds2431ScratchpadMark = function (bytes, commandIndex) {
  const k = commandIndex | 0;
  if (!bytes[k] || bytes[k].value !== 0xaa) return null;
  if (k + 4 > bytes.length) return { indices: [], width: 2, algorithm: "CRC-16", ok: false, partial: true };
  const ta1 = bytes[k + 1].value;
  const es = bytes[k + 3].value;
  const n = ((es & 7) - (ta1 & 7) + 1) & 7 || 8;
  const crcLoIndex = k + 4 + n;
  const crcHiIndex = crcLoIndex + 1;
  if (crcHiIndex >= bytes.length) return { indices: [], width: 2, algorithm: "CRC-16", ok: false, partial: true };
  const input = bytes.slice(k, crcLoIndex).map((b) => b.value); // AA + TA1/TA2/ES + data
  const expected = (~OWVis.crc16(input)) & 0xffff;
  const got = bytes[crcLoIndex].value | (bytes[crcHiIndex].value << 8);
  return { indices: [crcLoIndex, crcHiIndex], width: 2, algorithm: "CRC-16", ok: got === expected };
};

OWVis.transcript = function (txns) {
  const lines = [];
  txns.forEach((t, i) => {
    const interp = OWVis.interpret(t);
    const pres = t.presence === null ? "Presence: ?" : t.presence ? "Presence: true" : "Presence: false";
    const hex = interp.bytes.map((b) => b.value.toString(16).padStart(2, "0").toUpperCase()).join(" ");
    lines.push("T" + (i + 1) + "  " + pres + "  " + hex + (interp.note ? "   # " + interp.note : ""));
  });
  return lines.join("\n");
};

/* ---------- synthetic demo waveform (1 MHz, 1 byte/sample, D0) ---------- */

OWVis.genDemo = function () {
  const rate = 1000000;
  const out = [];
  const hi = (us) => {
    for (let k = 0; k < us; k++) out.push(1);
  };
  const low = (us) => {
    for (let k = 0; k < us; k++) out.push(0);
  };
  const slot = (bit) => {
    low(bit ? 6 : 60);
    hi(bit ? 64 : 10);
  };
  const byte = (v, dir) => {
    for (let b = 0; b < 8; b++) slot((v >> b) & 1);
  };
  const reset = () => {
    low(480);
    hi(30);
    low(100);
    hi(400);
  };
  const searchTriplet = (idBit, dirBit) => {
    // id slot (device), cmp slot (device), direction slot (master)
    slot(idBit);
    slot(idBit ? 0 : 1);
    slot(dirBit);
  };
  hi(1000);
  // T1: Match ROM DS18B20 + Read Scratchpad
  reset();
  byte(0x55);
  const rom1 = [0x28, 0x20, 0x78, 0x92, 0x07, 0x00, 0x00, 0x67];
  rom1.forEach(byte);
  byte(0xbe);
  const pad = [0x90, 0x01, 0x4b, 0x46, 0x7f, 0xff, 0x0c, 0x10, 0x33];
  pad.forEach(byte);
  hi(2000);
  // T2: Skip ROM + Convert T
  reset();
  byte(0xcc);
  byte(0x44);
  hi(2000);
  // T3: Search ROM, single device (rom1) on the bus
  reset();
  byte(0xf0);
  for (let i = 0; i < 64; i++) {
    const bit = (rom1[(i / 8) | 0] >> (i % 8)) & 1;
    searchTriplet(bit, bit); // id=bit, cmp=!bit, direction=bit
  }
  hi(2000);
  return { samplerate: rate, unitsize: 1, probes: 8, samples: Uint8Array.from(out), channel: 0, demo: true };
};

if (typeof module !== "undefined" && module.exports) {
  module.exports = OWVis;
}
