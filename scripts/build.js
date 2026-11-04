#!/usr/bin/env node
// build.js -- package a release as dist/SayCheese-<version>.ccx, which
// Creative Cloud installs on double-click. It carries everything: the panel,
// the analysis addon for each platform (win/x64 and mac/arm64), MediaPipe's
// library for each, and the face model, so there is nothing else to install.
// UXP loads the addon from the folder for the running platform.
//
// Run `npm run deps` first to fetch the MediaPipe library and the model.
// Zero npm dependencies; the zip writer is plain Node so it runs anywhere.
"use strict";
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const ROOT = path.resolve(__dirname, "..");
const DIST = path.join(ROOT, "dist");
const version = JSON.parse(fs.readFileSync(path.join(ROOT, "plugin", "manifest.json"), "utf8")).version;

const PLUGIN_FILES = ["manifest.json", "index.html", "main.js", "styles.css", "icons", "win", "mac", "models"];
const REQUIRED = [
  "win/x64/saycheese.uxpaddon", "win/x64/libmediapipe.dll",
  "mac/arm64/saycheese.uxpaddon", "mac/arm64/libmediapipe.dylib",
  "models/face_landmarker.task",
];
const EXECUTABLE = new Set();

/** Files under `dir` for each name (folders expanded), as [zip path, absolute path]. */
function collect(dir, names, prefix = "") {
  const out = [];
  for (const name of names) {
    // Addon copies moved aside by native/build-addon-win.bat, half-written files, Finder litter.
    if (name.endsWith(".old") || name.endsWith(".new") || name === ".DS_Store") continue;
    const abs = path.join(dir, name);
    if (fs.statSync(abs).isDirectory()) {
      out.push(...collect(abs, fs.readdirSync(abs).sort(), `${prefix}${name}/`));
    } else {
      out.push([`${prefix}${name}`, abs]);
    }
  }
  return out;
}

function dosDateTime(d) {
  return {
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
    date: ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
  };
}

/** Minimal zip writer: deflate, forward-slash names, Unix modes recorded. */
function writeZip(out, entries) {
  const locals = [];
  const central = [];
  let offset = 0;
  const { time, date } = dosDateTime(new Date());
  for (const [name, abs] of entries) {
    const data = fs.readFileSync(abs);
    const packed = zlib.deflateRawSync(data, { level: 9 });
    const crc = zlib.crc32(data);
    const nameBuf = Buffer.from(name, "utf8");
    const mode = EXECUTABLE.has(path.basename(name)) ? 0o100755 : 0o100644;

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);        // version needed
    local.writeUInt16LE(0x0800, 6);    // UTF-8 names
    local.writeUInt16LE(8, 8);         // deflate
    local.writeUInt16LE(time, 10);
    local.writeUInt16LE(date, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(packed.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    locals.push(local, nameBuf, packed);

    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(0x02014b50, 0);
    cen.writeUInt16LE((3 << 8) | 20, 4); // made by Unix, so the mode below counts
    cen.writeUInt16LE(20, 6);
    cen.writeUInt16LE(0x0800, 8);
    cen.writeUInt16LE(8, 10);
    cen.writeUInt16LE(time, 12);
    cen.writeUInt16LE(date, 14);
    cen.writeUInt32LE(crc, 16);
    cen.writeUInt32LE(packed.length, 20);
    cen.writeUInt32LE(data.length, 24);
    cen.writeUInt16LE(nameBuf.length, 28);
    cen.writeUInt32LE((mode << 16) >>> 0, 38);
    cen.writeUInt32LE(offset, 42);
    central.push(cen, nameBuf);

    offset += local.length + nameBuf.length + packed.length;
  }
  const cenSize = central.reduce((n, b) => n + b.length, 0);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cenSize, 12);
  end.writeUInt32LE(offset, 16);
  fs.writeFileSync(out, Buffer.concat([...locals, ...central, end]));
}

const missing = REQUIRED.filter((f) => !fs.existsSync(path.join(ROOT, "plugin", f)));
if (missing.length) {
  console.error(`build: missing plugin/${missing.join(", plugin/")}. Run npm run deps, and build the addon from native/ (the macOS one comes from CI, or native/build-addon-mac.sh on a Mac).`);
  process.exit(1);
}
fs.mkdirSync(DIST, { recursive: true });

// The .ccx has manifest.json at the archive root.
const ccx = path.join(DIST, `SayCheese-${version}.ccx`);
writeZip(ccx, collect(path.join(ROOT, "plugin"), PLUGIN_FILES));
console.log(`build: ${path.relative(ROOT, ccx)} (${(fs.statSync(ccx).size / 1048576).toFixed(1)} MB)`);
