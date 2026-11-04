#!/usr/bin/env node
// fetch-deps.js -- puts the engine's third-party files into plugin/:
//   plugin/win/x64/libmediapipe.dll        from Google's mediapipe wheels on PyPI (Apache-2.0)
//   plugin/mac/arm64/libmediapipe.dylib
//   plugin/models/face_landmarker.task     MediaPipe's face landmarker model (Apache-2.0)
// All are checked against pinned SHA-256 hashes, cached in .deps-cache/, and
// kept out of git. The addon itself (plugin/win/x64/saycheese.uxpaddon) is
// built from native/ and committed, as Syncitol does with its addon;
// the macOS addon (plugin/mac/arm64/saycheese.uxpaddon) is built by CI.
//
// Zero npm dependencies: fetch, crypto and zlib from Node 22.
"use strict";
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const ROOT = path.resolve(__dirname, "..");
const CACHE = path.join(ROOT, ".deps-cache");

const DEPS = [
  {
    url: "https://files.pythonhosted.org/packages/6a/b0/6d1cb31a04f1726cdcd86cf0ea3eb1e9304e59de03bb083571dcab4ac227/mediapipe-1.2.2-py3-none-win_amd64.whl",
    sha256: "71f44832fdd32b6270bc4379bcdd934287fb9eab974c3e74ef3fa76c7e79b54a",
    extract: "mediapipe/tasks/c/libmediapipe.dll",
    to: "plugin/win/x64/libmediapipe.dll",
  },
  {
    url: "https://files.pythonhosted.org/packages/a3/5c/ce8750cfe2de42ab2433b2d6445f4a16225a63412d991b2970204b047d73/mediapipe-1.2.2-py3-none-macosx_11_0_arm64.whl",
    sha256: "d3bb50923652182344ad31cfd2a09697e52952bd4bca2384ecabc90e21db9a5f",
    extract: "mediapipe/tasks/c/libmediapipe.dylib",
    to: "plugin/mac/arm64/libmediapipe.dylib",
  },
  {
    url: "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task",
    sha256: "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff",
    to: "plugin/models/face_landmarker.task",
  },
];

async function download(dep) {
  const file = path.join(CACHE, path.basename(new URL(dep.url).pathname));
  if (fs.existsSync(file) && sha256(fs.readFileSync(file)) === dep.sha256) return fs.readFileSync(file);
  console.log(`fetch-deps: downloading ${path.basename(file)}`);
  const res = await fetch(dep.url);
  if (!res.ok) throw new Error(`${dep.url}: HTTP ${res.status}`);
  const data = Buffer.from(await res.arrayBuffer());
  if (sha256(data) !== dep.sha256) throw new Error(`${path.basename(file)}: checksum mismatch`);
  fs.mkdirSync(CACHE, { recursive: true });
  fs.writeFileSync(file, data);
  return data;
}

function sha256(buf) {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

/** One file out of a zip (a wheel is a zip), via its central directory. */
function unzipEntry(zip, name) {
  let eocd = zip.length - 22;
  while (eocd >= 0 && zip.readUInt32LE(eocd) !== 0x06054b50) eocd--;
  if (eocd < 0) throw new Error("not a zip file");
  const count = zip.readUInt16LE(eocd + 10);
  let p = zip.readUInt32LE(eocd + 16);
  for (let i = 0; i < count; i++) {
    const method = zip.readUInt16LE(p + 10);
    const size = zip.readUInt32LE(p + 20);
    const nameLen = zip.readUInt16LE(p + 28);
    const extraLen = zip.readUInt16LE(p + 30);
    const commentLen = zip.readUInt16LE(p + 32);
    const local = zip.readUInt32LE(p + 42);
    const entry = zip.toString("utf8", p + 46, p + 46 + nameLen);
    if (entry === name) {
      const start = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
      const data = zip.subarray(start, start + size);
      return method === 8 ? zlib.inflateRawSync(data) : Buffer.from(data);
    }
    p += 46 + nameLen + extraLen + commentLen;
  }
  throw new Error(`${name} not found in archive`);
}

(async () => {
  for (const dep of DEPS) {
    const out = path.join(ROOT, dep.to);
    let data = await download(dep);
    if (dep.extract) data = unzipEntry(data, dep.extract);
    fs.mkdirSync(path.dirname(out), { recursive: true });
    // Write alongside and rename: overwriting a library Premiere has loaded can crash it on macOS.
    fs.writeFileSync(`${out}.new`, data);
    fs.renameSync(`${out}.new`, out);
    console.log(`fetch-deps: ${dep.to} (${(data.length / 1048576).toFixed(1)} MB)`);
  }
})().catch((e) => {
  console.error(`fetch-deps: ${e.message}`);
  process.exit(1);
});
