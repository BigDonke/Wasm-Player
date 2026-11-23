// Runs FFmpeg off the main thread. Reads the source with synchronous range
// requests, which are allowed in workers and let the C side read on demand.
'use strict';

let M, src, size = -1;
let gen = 0;
let outs = [[], []], texts = [], bitmaps = [], raps = [];
let readError = ''; // last failed read during the current call into the core

/* ---------- byte source with an adaptive read-ahead cache ---------- */

// Remote reads start larger because each request pays the server's latency
const MAX_CHUNK = 8 << 20;
const minChunk = () => (src.file ? 256 << 10 : 1 << 20);
let ranges = [], cached = 0, chunk = 0, lastEnd = -1;

function fetchBytes(start, end) {
  if (src.file) return new Uint8Array(new FileReaderSync().readAsArrayBuffer(src.file.slice(start, end + 1)));
  const x = new XMLHttpRequest();
  x.open('GET', src.url, false);
  x.responseType = 'arraybuffer';
  x.setRequestHeader('Range', `bytes=${start}-${end}`);
  // A synchronous request throws when the browser blocks it, and then status stays 0
  try { x.send(); } catch (e) { /* handled by the status check below */ }
  if (x.status === 206) {
    const cr = x.getResponseHeader('Content-Range');
    const m = cr && /\/(\d+)/.exec(cr);
    if (m) size = +m[1];
    rangesWork = true;
    return new Uint8Array(x.response);
  }
  if (x.status === 200) {
    // The server ignored Range and sent everything. Keep it as one big range. probeRanges
    // normally catches this first. A null response means the browser could not allocate it.
    if (!x.response) throw new Error(TOO_BIG);
    const all = new Uint8Array(x.response);
    size = all.length;
    ranges = [{ start: 0, data: all }];
    cached = all.length;
    return all.subarray(start, end + 1);
  }
  if (x.status === 416) return new Uint8Array(0);
  // Status 0 means the browser blocked the response (no CORS headers) or the connection failed.
  // The two look the same from here.
  throw new Error(x.status === 0
    ? "Could not read the link. The server doesn't let other websites read its files, or the connection failed. "
      + 'You can download the file and open it here instead.'
    : x.status === 404 ? 'The server has no file at this link (HTTP 404).'
    : x.status === 401 || x.status === 403 ? `The server refused access to the file (HTTP ${x.status}). The link may have expired.`
    : `The server answered with an error (HTTP ${x.status}).`);
}

function findRange(pos) {
  for (let i = 0; i < ranges.length; i++) {
    const r = ranges[i];
    if (pos >= r.start && pos < r.start + r.data.length) {
      if (i) { ranges.splice(i, 1); ranges.unshift(r); }
      return r;
    }
  }
  return null;
}

// Adds a range, then evicts down to the cache limit. Ranges outside the prefetch window go
// first, then the least recently used.
function addRange(r, front) {
  if (front) ranges.unshift(r); else ranges.push(r);
  cached += r.data.length;
  const limit = (pf.on ? pf.limit : 0) + (96 << 20);
  const lo = pf.pos - (8 << 20), hi = pf.pos + pf.limit;
  for (let i = ranges.length - 1; i >= 0 && cached > limit; i--) {
    const q = ranges[i];
    if (q !== r && (q.start + q.data.length < lo || q.start > hi)) { ranges.splice(i, 1); cached -= q.data.length; }
  }
  while (cached > limit && ranges.length > 1) {
    const q = ranges[ranges.length - 1] === r ? ranges.splice(-2, 1)[0] : ranges.pop();
    cached -= q.data.length;
  }
}

function readAt(pos, ptr, len) {
  if (size >= 0 && pos >= size) return 0;
  let r = findRange(pos);
  if (!r) {
    chunk = pos === lastEnd ? Math.min(chunk * 2, MAX_CHUNK) : minChunk();
    let end = pos + chunk - 1;
    // A background download of these bytes cannot be awaited here, so take over its range
    for (const f of pf.flights) {
      if (pos >= f.start && pos < f.end) { f.ctrl.abort(); pf.flights.delete(f); end = f.end - 1; }
    }
    if (size >= 0) end = Math.min(end, size - 1);
    const data = fetchBytes(pos, end);
    if (!data.length) return 0;
    lastEnd = pos + data.length;
    if (!findRange(pos)) addRange({ start: pos, data }, true);
    r = findRange(pos);
  }
  const off = pos - r.start, n = Math.min(len, r.data.length - off);
  M.HEAPU8.set(r.data.subarray(off, off + n), ptr);
  if (pumping) pf.pos = pos + n;
  return n;
}

/* ---------- background prefetch for links ---------- */

// The demuxer reads synchronously, so it cannot wait for an async fetch. Instead the worker
// downloads ahead of the demuxer between pumps, and a pump starts once its bytes are cached.
// The SourceBuffers hold only a few seconds of a high-bitrate file, and this cache rides out
// network drops. It holds 60 seconds of media, between 32 and 256 MB. Requests grow from 1 MB
// next to the read position to 8 MB further out, so a start or seek waits for little data.
const PF_CHUNK = 8 << 20, PF_PARALLEL = 2, PF_MARGIN = 2 << 20;
const pf = { on: false, pos: -1, limit: 0, flights: new Set() };
let rangesWork = false, pumping = false;

function rangeAt(pos) {
  for (const r of ranges) if (pos >= r.start && pos < r.start + r.data.length) return r;
  return null;
}

// End of the cached bytes that run without a gap from pos
function cachedUntil(pos) {
  for (let r; (r = rangeAt(pos)); ) pos = r.start + r.data.length;
  return pos;
}

function prefetch() {
  if (!pf.on) return;
  const hi = pf.pos < 0 ? -1 : Math.min(pf.pos + pf.limit, size);
  // Drop downloads the demuxer has moved away from, e.g. after a seek
  for (const f of pf.flights) if (f.end < pf.pos || f.start > hi) { f.ctrl.abort(); pf.flights.delete(f); }
  while (pf.pos >= 0 && pf.flights.size < PF_PARALLEL) {
    let next = cachedUntil(pf.pos);
    for (const f of pf.flights) if (f.start <= next && f.end > next) next = f.end;
    if (next >= hi) return;
    const len = Math.min(PF_CHUNK, Math.max(1 << 20, next - pf.pos));
    const f = { start: next, end: Math.min(next + len, size), ctrl: new AbortController() };
    f.done = fetch(src.url, { headers: { Range: `bytes=${f.start}-${f.end - 1}` }, signal: f.ctrl.signal })
      .then((res) => (res.status === 206 ? res.arrayBuffer() : Promise.reject(new Error('no range'))))
      .then((buf) => { if (!rangeAt(f.start)) addRange({ start: f.start, data: new Uint8Array(buf) }, false); })
      .catch(() => { /* the synchronous path fetches whatever is missing */ })
      .finally(() => { pf.flights.delete(f); prefetch(); });
    pf.flights.add(f);
  }
}

// Waits until the next bytes the demuxer will read are cached, or no download can provide them
async function prefetched(need) {
  if (pf.pos < 0) return;
  const want = Math.min(pf.pos + need + PF_MARGIN, size);
  prefetch();
  while (cachedUntil(pf.pos) < want && pf.flights.size) await Promise.race([...pf.flights].map((f) => f.done));
}

function probeSize() {
  if (src.file) { size = src.file.size; return; }
  readAt(0, 0, 0);
  if (size >= 0) return;
  try {
    const x = new XMLHttpRequest();
    x.open('HEAD', src.url, false);
    x.send();
    const len = x.getResponseHeader('Content-Length');
    if (x.status < 300 && len) size = +len;
  } catch (e) { /* size stays unknown, seeking may be limited */ }
}

// A server that ignores Range sends the whole file. Find that out with one async request
// and stream the body into memory with a cap and progress, so a synchronous request never
// pulls an unbounded body. Network and CORS failures are left to the synchronous path,
// which explains them.
const WHOLE_MAX = 256 << 20;
const TOO_BIG = "The server doesn't support range requests, so the player would have to hold the whole file "
  + 'in memory, and this file is over 256 MB. You can download the file and open it here instead.';

async function probeRanges() {
  if (src.file || typeof fetch !== 'function') return;
  const ctrl = new AbortController();
  let res;
  try {
    res = await fetch(src.url, { headers: { Range: `bytes=0-${minChunk() - 1}` }, signal: ctrl.signal });
  } catch (e) { return; }
  if (res.status === 206) {
    const cr = res.headers.get('Content-Range');
    const m = cr && /\/(\d+)/.exec(cr);
    if (m) size = +m[1];
    rangesWork = true;
    try { addRange({ start: 0, data: new Uint8Array(await res.arrayBuffer()) }, true); } catch (e) { /* read again later */ }
    return;
  }
  if (res.status !== 200 || !res.body) { ctrl.abort(); return; }
  // Content-Length is the compressed size when the body is compressed, so it only sizes the buffer
  const total = +res.headers.get('Content-Length') || 0;
  if (total > WHOLE_MAX) { ctrl.abort(); throw new Error(TOO_BIG); }
  let buf = new Uint8Array(total || 16 << 20), n = 0, shown = 0;
  const reader = res.body.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (n + value.length > WHOLE_MAX) { ctrl.abort(); throw new Error(TOO_BIG); }
    if (n + value.length > buf.length) {
      const grown = new Uint8Array(Math.min(WHOLE_MAX, Math.max(buf.length * 2, n + value.length)));
      grown.set(buf.subarray(0, n));
      buf = grown;
    }
    buf.set(value, n);
    n += value.length;
    if (Date.now() - shown > 250) { shown = Date.now(); postMessage({ type: 'whole', loaded: n, total }); }
  }
  const all = n === buf.length ? buf : buf.slice(0, n);
  ranges = [{ start: 0, data: all }];
  cached = size = n;
}

/* ---------- output collection ---------- */

function concat(list) {
  if (!list.length) return null;
  if (list.length === 1) return list[0];
  const out = new Uint8Array(list.reduce((n, a) => n + a.length, 0));
  let o = 0;
  for (const a of list) { out.set(a, o); o += a.length; }
  return out;
}

function flush(extra) {
  const v = concat(outs[0]), a = concat(outs[1]);
  const transfer = [];
  if (v) transfer.push(v.buffer);
  if (a) transfer.push(a.buffer);
  for (const b of bitmaps) for (const r of b.rects) transfer.push(r.rgba.buffer);
  postMessage({ type: 'data', gen, v, a, texts, bitmaps, raps, ...extra }, transfer);
  outs = [[], []]; texts = []; bitmaps = []; raps = [];
}

async function init() {
  if (M) return;
  importScripts('ff.js');
  M = await FFCore({
    onRead: (pos, ptr, len) => {
      try { return readAt(pos, ptr, len); } catch (e) { readError = e.message || String(e); return -1; }
    },
    // Reads exactly len bytes without caching them, for small header probes
    onPeek: (pos, ptr, len) => {
      try {
        const end = size >= 0 ? Math.min(pos + len, size) : pos + len;
        const r = rangeAt(pos);
        const data = r && r.start + r.data.length >= end ? r.data.subarray(pos - r.start, end - r.start) : fetchBytes(pos, end - 1);
        M.HEAPU8.set(data.subarray(0, len), ptr);
        return Math.min(data.length, len);
      } catch (e) { return -1; }
    },
    onSize: () => size,
    onOut: (kind, bytes) => outs[kind].push(bytes),
    onText: (idx, start, end, bytes) => texts.push({ idx, start, end, bytes }),
    onRap: (t) => raps.push(t),
    onBitmap: (idx, start, end, cw, ch, x, y, w, h, rgba) => {
      let ev = bitmaps[bitmaps.length - 1];
      if (!ev || ev.idx !== idx || ev.start !== start) bitmaps.push(ev = { idx, start, end, cw, ch, rects: [] });
      if (rgba) ev.rects.push({ x, y, w, h, rgba });
    },
  });
}

onmessage = async ({ data: msg }) => {
  try {
    if (msg.type === 'open') {
      await init();
      src = msg.src;
      await probeRanges();
      probeSize();
      const json = M.UTF8ToString(M._fp_open());
      if (!json) throw new Error('Could not read this file. Unsupported container or the server refused the request.');
      const info = JSON.parse(json);
      pf.on = !src.file && rangesWork && size > 0 && typeof fetch === 'function';
      pf.limit = Math.max(32 << 20, Math.min(256 << 20, info.duration > 0 ? (size / info.duration) * 60 : 0));
      postMessage({ type: 'info', info, size });
    } else if (msg.type === 'start') {
      gen = msg.gen;
      outs = [[], []]; texts = []; bitmaps = []; raps = [];
      // The read position is unknown until the first pump after the seek
      pf.pos = -1;
      prefetch();
      let aMode = msg.aMode;
      readError = '';
      let ret = M._fp_start(msg.t, msg.v, msg.a, aMode, msg.maxCh, msg.sub, msg.dv, msg.idr | 0);
      if (ret === -2 && aMode === 0) {
        aMode = 1;
        outs = [[], []];
        ret = M._fp_start(msg.t, msg.v, msg.a, aMode, msg.maxCh, msg.sub, msg.dv, msg.idr | 0);
      }
      // A seek that failed on a read is retried by the page, like a failed pump
      if (ret < 0 && readError) { outs = [[], []]; return flush({ readError }); }
      if (ret < 0) throw new Error(['', 'Video stream could not be prepared.', 'Audio stream could not be prepared.', 'Seek failed.'][-ret]);
      postMessage({ type: 'started', gen, aMode });
      flush({});
    } else if (msg.type === 'pump') {
      if (msg.gen !== gen) return;
      if (pf.on) {
        await prefetched(msg.bytes);
        if (msg.gen !== gen) return;
      }
      pumping = true;
      readError = '';
      let ret;
      try { ret = M._fp_pump(msg.bytes, msg.sec); } finally { pumping = false; }
      prefetch();
      // The core is stuck after a read error. The page restarts it once the source answers again.
      if (ret < 0) return flush({ readError: readError || 'Reading the source failed.' });
      flush({ eof: ret === 1 });
    }
  } catch (e) {
    postMessage({ type: 'error', gen: msg.gen, message: e.message || String(e) });
  }
};
