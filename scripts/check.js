#!/usr/bin/env node
/**
 * check.js -- quick checks run by CI and before a release: the panel script
 * parses, the manifest is valid JSON, and its version matches package.json.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

const ROOT = path.resolve(__dirname, "..");
let failed = false;
const fail = (msg) => {
  console.error(`check: ${msg}`);
  failed = true;
};

for (const f of ["plugin/main.js", "scripts/build.js", "scripts/set-version.js", "scripts/fetch-deps.js"]) {
  try {
    execFileSync(process.execPath, ["--check", path.join(ROOT, f)], { stdio: "pipe" });
  } catch (e) {
    fail(`${f} doesn't parse:\n${e.stderr}`);
  }
}

let manifest = {};
try {
  manifest = JSON.parse(fs.readFileSync(path.join(ROOT, "plugin/manifest.json"), "utf8"));
} catch (e) {
  fail(`plugin/manifest.json: ${e.message}`);
}
const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8"));
if (manifest.version !== pkg.version) {
  fail(`versions disagree: manifest ${manifest.version}, package.json ${pkg.version}. Use npm run version:set -- x.y.z`);
}

if (failed) process.exit(1);
console.log(`check: ok (v${manifest.version})`);
