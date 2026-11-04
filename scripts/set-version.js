#!/usr/bin/env node
/**
 * set-version.js -- single-source version bumper.
 * Usage: npm run version:set -- 0.2.0
 * Updates the plugin manifest and package.json so they can never drift.
 */
"use strict";
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..");
const version = process.argv[2];
if (!/^\d+\.\d+\.\d+$/.test(version || "")) {
  console.error("Usage: node scripts/set-version.js <major.minor.patch>");
  process.exit(1);
}

function edit(rel, fn) {
  const file = path.join(ROOT, rel);
  const before = fs.readFileSync(file, "utf8");
  const after = fn(before);
  if (before === after && !before.includes(version)) {
    console.error(`set-version: NO CHANGE in ${rel} -- pattern not found?`);
    process.exit(1);
  }
  fs.writeFileSync(file, after);
  console.log(`set-version: ${rel} -> ${version}`);
}

edit("plugin/manifest.json", (s) => s.replace(/("version":\s*")[^"]+(")/, `$1${version}$2`));
edit("package.json", (s) => s.replace(/("version":\s*")[^"]+(")/, `$1${version}$2`));
console.log("set-version: done.");
