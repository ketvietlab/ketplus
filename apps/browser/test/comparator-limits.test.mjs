import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { PNG } from "pngjs";
import { compareRaster } from "../src/comparator.mjs";

// A valid PNG signature and IHDR chunk claiming the given size, with no image data:
// enough for the header check, which must refuse it before pngjs allocates anything.
function headerOnlyPng(width, height) {
  const bytes = Buffer.alloc(33);
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(bytes, 0);
  bytes.writeUInt32BE(13, 8);
  bytes.write("IHDR", 12, "latin1");
  bytes.writeUInt32BE(width, 16);
  bytes.writeUInt32BE(height, 20);
  bytes[24] = 8;
  bytes[25] = 6;
  return bytes;
}

test("raster comparison refuses oversized or non-PNG input before decoding", async (t) => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-raster-limits-"));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const small = new PNG({ width: 4, height: 4 });
  const reference = path.join(dir, "reference.png");
  await fs.writeFile(reference, PNG.sync.write(small));
  const capture = (file) => ({ path: file, environment: { deviceScaleFactor: 1 } });
  const compareWith = async (bytes) => {
    const candidate = path.join(dir, `candidate-${Math.random()}.png`);
    await fs.writeFile(candidate, bytes);
    return compareRaster(capture(reference), capture(candidate), path.join(dir, "diff.png"), {});
  };

  await assert.rejects(compareWith(headerOnlyPng(40_000, 40_000)), (error) => {
    assert.equal(error.code, "RASTER_TOO_LARGE");
    assert.deepEqual(error.data, { width: 40_000, height: 40_000 });
    return true;
  });
  await assert.rejects(compareWith(headerOnlyPng(12_289, 12_288)), { code: "RASTER_TOO_LARGE" });
  await assert.rejects(compareWith(Buffer.from("not a png at all, just text")), { code: "INVALID_RASTER" });
  const same = await compareWith(PNG.sync.write(small));
  assert.equal(same.mismatchRatio, 0);
});
