/**
 * Raw DEFLATE inflater used for MSZIP cabinet folders (compression type 1).
 *
 * MSZIP stores in each CFDATA block: a 2-byte checksum XOR'd with magic 0x4b43 ("CK") and then
 * a DEFLATE stream. The stream's 32 KB dictionary is the last 32 KB of the folder's previously
 * decompressed output (during the first block it is the compressor's initial all-zero window).
 * Blocks decode back-to-back into the folder's single output buffer, so the buffer itself
 * provides the history across blocks; back-references reaching before the folder start fall
 * back to the zero-filled initial window. The DEFLATE streams of MSZIP chunks typically end
 * mid-stream (no terminating final block), so decoding is driven by the declared block size.
 *
 * Pure TypeScript/Uint8Array (no Buffer, no Node-only imports) so it also runs in the browser
 * installer worker.
 */

const LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CODELEN_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

interface Huffman {
  counts: number[];
  symbols: number[];
}

const FIXED_LIT_LENS: number[] = [];
for (let i = 0; i < 144; i++) FIXED_LIT_LENS.push(8);
for (let i = 144; i < 256; i++) FIXED_LIT_LENS.push(9);
for (let i = 256; i < 280; i++) FIXED_LIT_LENS.push(7);
for (let i = 280; i < 288; i++) FIXED_LIT_LENS.push(8);
const FIXED_LIT = build(FIXED_LIT_LENS);
const FIXED_DIST = build(new Array<number>(30).fill(5), true);

function build(lengths: number[], allowIncomplete = false): Huffman {
  const counts = new Array<number>(16).fill(0);
  let max = 0;
  for (const l of lengths) {
    counts[+l]++;
    if (l > max) max = l;
  }
  let left = 1;
  for (let len = 1; len <= 15; len++) {
    left = (left << 1) - counts[len];
    if (left < 0) throw new Error("MSZIP stream: over-subscribed Huffman code");
  }
  // Incomplete codes are normally an error (zlib tolerates a lone short code); the fixed
  // distance table is incomplete by design (RFC 1951) and decodes the leftover codes as
  // an error.
  if (left > 0 && max !== 1 && !allowIncomplete) throw new Error("MSZIP stream: incomplete Huffman code");
  const offs = new Array<number>(17);
  offs[1] = 0;
  for (let len = 1; len < 16; len++) offs[len + 1] = offs[len] + counts[len];
  const symbols = new Array<number>(lengths.filter((l) => l > 0).length);
  for (let sym = 0; sym < lengths.length; sym++) if (lengths[sym] > 0) symbols[offs[lengths[sym]]++] = sym;
  return { counts, symbols };
}

class BitReader {
  private readonly src: Uint8Array;
  private pos = 0;
  private buf = 0;
  private cnt = 0;

  constructor(src: Uint8Array) {
    this.src = src;
  }

  readBit(): number {
    if (this.cnt === 0) {
      if (this.pos >= this.src.length) throw new Error("MSZIP stream: unexpected end of data");
      this.buf = this.src[this.pos++];
      this.cnt = 8;
    }
    const bit = this.buf & 1;
    this.buf >>>= 1;
    this.cnt--;
    return bit;
  }

  readBits(n: number): number {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.readBit() << i;
    return v;
  }

  /** Skip n bytes from the current byte-aligned position. */
  skipBytes(n: number): void {
    if (this.cnt !== 0) throw new Error("MSZIP stream: internal alignment error");
    this.pos += n;
  }

  /** Byte offset of the next byte-aligned position (after aligning). */
  get alignedPos(): number {
    return this.pos;
  }

  alignToByte(): void {
    this.cnt = 0;
  }
}

function decodeSym(br: BitReader, h: Huffman): number {
  let code = 0;
  let first = 0;
  let index = 0;
  for (let len = 1; len <= 15; len++) {
    code |= br.readBit();
    const count = h.counts[len];
    if (code - first < count) return h.symbols[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  throw new Error("MSZIP stream: invalid Huffman code");
}

function copyBlock(br: BitReader, out: Uint8Array, pos: number, end: number, lit: Huffman, dist: Huffman): number {
  for (;;) {
    const sym = decodeSym(br, lit);
    if (sym === 256) return pos; // end of DEFLATE block
    if (sym < 256) {
      if (pos >= end) throw new Error("MSZIP stream: extra output beyond declared size");
      out[pos++] = sym;
      continue;
    }
    const li = sym - 257;
    if (li >= 29) throw new Error("MSZIP stream: invalid length symbol");
    const len = LENGTH_BASE[li] + br.readBits(LENGTH_EXTRA[li]);
    if (pos + len > end) throw new Error("MSZIP stream: match overruns declared output size");
    const dsym = decodeSym(br, dist);
    if (dsym >= 30) throw new Error("MSZIP stream: invalid distance symbol");
    const distVal = DIST_BASE[dsym] + br.readBits(DIST_EXTRA[dsym]);
    if (distVal < 1 || distVal > 32768) throw new Error("MSZIP stream: invalid distance");
    let cur = pos;
    for (let i = 0; i < len; i++) {
      // Back-references before the folder start read the compressor's initial all-zero window.
      out[cur] = cur - distVal >= 0 ? out[cur - distVal] : 0;
      cur++;
    }
    pos = cur;
  }
}

export function inflateRaw(src: Uint8Array, out: Uint8Array, pos: number, end: number): number {
  const br = new BitReader(src);
  let o = pos;
  for (;;) {
    if (o >= end) break; // count-driven stop: trailing flush padding/leftover bits are ignored
    br.readBit(); // BFINAL: MSZIP blocks are truncated mid-stream, so finality is ignored
    const btype = br.readBits(2);
    if (btype === 0) {
      br.alignToByte();
      const len = br.readBits(16);
      const nlen = br.readBits(16);
      if ((len ^ 0xffff) !== nlen) throw new Error("MSZIP stream: stored block length check failed");
      const start = br.alignedPos;
      if (start + len > src.length) throw new Error("MSZIP stream: stored block overruns source");
      const n = Math.min(len, end - o);
      out.set(src.subarray(start, start + n), o);
      o += n;
      br.skipBytes(n);
    } else if (btype === 1) {
      o = copyBlock(br, out, o, end, FIXED_LIT, FIXED_DIST);
    } else if (btype === 2) {
      const hlit = br.readBits(5) + 257;
      const hdist = br.readBits(5) + 1;
      const hclen = br.readBits(4) + 4;
      const cl = new Array<number>(19).fill(0);
      for (let i = 0; i < hclen; i++) cl[CODELEN_ORDER[i]] = br.readBits(3);
      const clTable = build(cl);
      const lens = new Array<number>(hlit + hdist);
      let i = 0;
      while (i < lens.length) {
        const sym = decodeSym(br, clTable);
        if (sym < 16) {
          lens[i++] = sym;
        } else {
          let run: number;
          let value: number;
          if (sym === 16) {
            if (i === 0) throw new Error("MSZIP stream: repeat with no previous length");
            run = br.readBits(2) + 3;
            value = lens[i - 1];
          } else if (sym === 17) {
            run = br.readBits(3) + 3;
            value = 0;
          } else {
            run = br.readBits(7) + 11;
            value = 0;
          }
          if (i + run > lens.length) throw new Error("MSZIP stream: length run overruns table");
          lens.fill(value, i, i + run);
          i += run;
        }
      }
      if (lens[256] === 0) throw new Error("MSZIP stream: missing end-of-block code");
      o = copyBlock(br, out, o, end, build(lens.slice(0, hlit)), build(lens.slice(hlit)));
    } else throw new Error("MSZIP stream: invalid block type 3");
  }
  return o;
}

/** Decode one MSZIP block's DEFLATE stream (already stripped of the "CK"/checksum header). */
export function mszipInflate(stream: Uint8Array, out: Uint8Array, pos: number, size: number): number {
  const got = inflateRaw(stream, out, pos, pos + size);
  if (got !== pos + size) throw new Error(`MSZIP stream: produced ${got - pos} bytes, expected ${size}`);
  return got;
}
