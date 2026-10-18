import { describe, expect, it } from "vitest";
import { deflateRawSync, inflateRawSync } from "node:zlib";
import { Cabinet } from "../installer/src/cab.ts";
import { inflateRaw } from "../installer/src/mszip.ts";
import { NodeFileSource } from "../installer/src/node.ts";

/** Deterministic pseudo-random bytes (so snapshots of the vector are stable). */
function randomBytes(n: number, seed: number): Uint8Array {
  const out = new Uint8Array(n);
  let state = seed >>> 0;
  for (let i = 0; i < n; i++) {
    state = (state * 1664525 + 1013904223) >>> 0;
    out[i] = state >>> 24;
  }
  return out;
}

describe("MSZIP inflater", () => {
  it("decodes a block whose dictionary is the previous block's last 32 KB", () => {
    const first = randomBytes(40000, 1234);
    const comp1 = deflateRawSync(Buffer.from(first));
    const out1 = inflateRawSync(comp1);
    if (out1.length < 32768) throw new Error("vector must produce more than 32 KB of output");
    const dictionary = Buffer.from(out1.subarray(out1.length - 32768));
    const second = randomBytes(20000, 99);
    const comp2 = deflateRawSync(Buffer.from(second), { dictionary });
    // cross-check the vector itself against zlib's own dictionary mode
    expect(inflateRawSync(comp2, { dictionary })).toEqual(Buffer.from(second));

    // our windowed decoder: the preceding output (block 1) lives in the output buffer,
    // exactly as cabinet folders decode block after block
    const out = new Uint8Array(out1.length + second.length);
    out.set(out1, 0);
    inflateRaw(comp2, out, out1.length, out1.length + second.length);
    expect(out.subarray(out1.length)).toEqual(second);
  });

  it("decodes a first block with an empty dictionary", () => {
    const data = randomBytes(30000, 777);
    const comp = deflateRawSync(Buffer.from(data));
    const out = new Uint8Array(data.length);
    inflateRaw(comp, out, 0, data.length);
    expect(out).toEqual(data);
  });
});

describe("Bella Sara Data1.cab header", () => {
  it("reports 147 folders / 1,101 files with MSZIP folders", () => {
    const cab = new Cabinet(new NodeFileSource("work/ref/Data1.cab"));
    expect(cab.folders.length).toBe(147);
    expect(cab.files.length).toBe(1101);
    expect(cab.folders[0].compression).toBe(1);
  });
});
