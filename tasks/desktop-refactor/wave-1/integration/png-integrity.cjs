const { inflateSync } = require("node:zlib");

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Capture interchange is deliberately narrow: non-interlaced 8-bit RGB/RGBA PNG.
// This validates encoding integrity, not image semantics or pixel correctness.
function inspectPng(bytes) {
  if (!Buffer.isBuffer(bytes) || !bytes.subarray(0, 8).equals(Buffer.from("89504e470d0a1a0a", "hex"))) throw new Error("invalid PNG signature");
  let offset = 8, header, ended = false, seenData = false, dataEnded = false;
  const data = [];
  while (offset < bytes.length) {
    if (offset + 12 > bytes.length) throw new Error("truncated PNG chunk");
    const length = bytes.readUInt32BE(offset);
    if (length > 64 * 1024 * 1024 || offset + length + 12 > bytes.length) throw new Error("invalid PNG chunk length");
    const type = bytes.toString("ascii", offset + 4, offset + 8);
    const chunk = bytes.subarray(offset + 8, offset + 8 + length);
    if (crc32(bytes.subarray(offset + 4, offset + 8 + length)) !== bytes.readUInt32BE(offset + 8 + length)) throw new Error("PNG CRC mismatch");
    if (!header && type !== "IHDR") throw new Error("PNG must start with IHDR");
    if (type === "IHDR") {
      if (header || length !== 13) throw new Error("invalid PNG IHDR");
      header = { width: chunk.readUInt32BE(0), height: chunk.readUInt32BE(4), channels: chunk[9] === 6 ? 4 : 3 };
      if (header.width < 1 || header.height < 1 || header.width * header.height > 16000000 || chunk[8] !== 8 || ![2, 6].includes(chunk[9]) || chunk[10] || chunk[11] || chunk[12]) throw new Error("unsupported PNG capture encoding/dimensions");
    } else if (type === "IDAT") {
      if (dataEnded) throw new Error("non-contiguous PNG data");
      seenData = true;
      data.push(chunk);
    } else if (type === "IEND") {
      if (length || !seenData || offset + 12 !== bytes.length) throw new Error("invalid PNG end");
      ended = true;
    } else {
      if (seenData) dataEnded = true;
      if (type[0] === type[0].toUpperCase() && type !== "PLTE") throw new Error("unknown critical PNG chunk");
    }
    offset += length + 12;
  }
  if (!ended) throw new Error("missing PNG end");
  const stride = header.width * header.channels + 1;
  const decoded = inflateSync(Buffer.concat(data), { maxOutputLength: stride * header.height });
  if (decoded.length !== stride * header.height) throw new Error("invalid PNG scanline length");
  for (let row = 0; row < header.height; row++) if (decoded[row * stride] > 4) throw new Error("invalid PNG filter");
  return { width: header.width, height: header.height };
}
module.exports = { inspectPng, crc32 };
