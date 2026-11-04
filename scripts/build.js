#!/usr/bin/env node
// build.js -- package a release into dist/:
//   SayCheese-<version>.ccx         the panel; Creative Cloud installs it on double-click
//   SayCheese-Engine-<version>.zip  the analysis engine (Python sources + launchers)
//
// The engine zip holds no virtualenv or models: start.bat / start.command run
// the one-time setup on first launch, which installs dependencies and
// downloads the checksum-verified models.
//
// Zero npm dependencies, and the zip writer is plain Node so the same build
// runs on Windows and macOS and keeps start.command executable.
"use strict";
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const ROOT = path.resolve(__dirname, "..");
const DIST = path.join(ROOT, "dist");
const version = JSON.parse(fs.readFileSync(path.join(ROOT, "plugin", "manifest.json"), "utf8")).version;

const PLUGIN_FILES = ["manifest.json", "index.html", "main.js", "styles.css", "icons"];
const ENGINE_FILES = ["analyzer.py", "server.py", "requirements.txt", "setup.bat", "start.bat", "start.command"];
const EXECUTABLE = new Set(["start.command"]);

/** Files under `dir` for each name (folders expanded), as [zip path, absolute path]. */
function collect(dir, names, prefix = "") {
  const out = [];
  for (const name of names) {
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

fs.mkdirSync(DIST, { recursive: true });

// The .ccx has manifest.json at the archive root.
const ccx = path.join(DIST, `SayCheese-${version}.ccx`);
writeZip(ccx, collect(path.join(ROOT, "plugin"), PLUGIN_FILES));

// The engine unzips into its own folder.
const engineZip = path.join(DIST, `SayCheese-Engine-${version}.zip`);
writeZip(engineZip, collect(path.join(ROOT, "engine"), ENGINE_FILES, "SayCheese-Engine/"));

for (const f of [ccx, engineZip]) {
  const kb = (fs.statSync(f).size / 1024).toFixed(0);
  console.log(`build: ${path.relative(ROOT, f)} (${kb} KB)`);
}
