/*
 * SayCheese — Premiere Pro UXP panel.
 *
 * The panel works out which pieces of source media are on screen (whole
 * sequence or selected clips), hands them to the local analysis engine
 * (engine/server.py) and shows the ranked smiles it finds. Times go to the
 * engine as source-file seconds plus the mapping back to sequence time, so
 * results come back already placed on the timeline.
 */
const ppro = require("premierepro");
const uxp = require("uxp");
const os = require("os");

const fs = uxp.storage.localFileSystem;
const nfs = require("fs");
const TICKS_PER_SECOND = 254016000000;
const MAX_NEST_DEPTH = 4;
// Card layout, matching .card and #results in styles.css.
const CARD_GAP = 8;
const CARD_PITCH = 190 + CARD_GAP; // card width incl. border, plus gap
const RESULTS_PADDING = 24;
// Graphics and stills are skipped: they are never interview footage, and
// PNG/PSD layers above the interview (lower thirds) must not count as covering it.
const STILL_EXT =/\.(png|jpe?g|tiff?|psd|psb|ai|eps|gif|bmp|exr|dpx|tga|svg|heic|webp|aegraphic|mogrt)$/i;

const $ = (id) => document.getElementById(id);

const settings = loadSettings();
const state = {
  mode: "sequence",
  sort: "score",
  view: "frame",
  engineOk: false,
  job: null,            // { id, sequence, timebase, zeroPoint, frameRect }
  results: [],
  selected: new Set(),
  currentId: null,
  polling: null,
  summary: "",
  perRow: 0,
  engineKey: "",
  footerKey: "",
  version: "",
};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

function loadSettings() {
  const defaults = {
    port: 47820,
    sampleFps: "4",
    maxResults: "24",
    priority: "balanced",
    minGap: "3",
    skipCovered: true,
    useInOut: false,
    exportFormat: "png",
    exportFolders: {}, // project path -> folder picked for that project
    engineLauncher: null, // start.bat / start.command picked when the plugin is installed from a .ccx
  };
  try {
    return Object.assign(defaults, JSON.parse(localStorage.getItem("saycheese") || "{}"));
  } catch (e) {
    return defaults;
  }
}

function saveSettings() {
  try {
    localStorage.setItem("saycheese", JSON.stringify(settings));
  } catch (e) { /* non-fatal */ }
}

function engineUrl(path) {
  return `http://127.0.0.1:${settings.port}${path}`;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

function secs(tickTime) {
  if (!tickTime) return 0;
  if (typeof tickTime.ticksNumber === "number") return tickTime.ticksNumber / TICKS_PER_SECOND;
  return Number(tickTime.seconds) || 0;
}

/** Sequence seconds -> TickTime on the nearest sequence frame. */
function frameTick(seconds, timebase) {
  const ticks = Math.round((seconds * TICKS_PER_SECOND) / timebase) * timebase;
  return ppro.TickTime.createWithTicks(String(Math.max(0, ticks)));
}

function timecode(seconds, timebase) {
  const fps = TICKS_PER_SECOND / timebase;
  const nominal = Math.round(fps);
  let frames = Math.round(seconds * fps);
  const ff = frames % nominal;
  frames = Math.floor(frames / nominal);
  const pad = (n) => String(n).padStart(2, "0");
  return `${pad(Math.floor(frames / 3600))}:${pad(Math.floor(frames / 60) % 60)}:${pad(frames % 60)}:${pad(ff)}`;
}

// ---------------------------------------------------------------------------
// Collecting footage from the timeline
// ---------------------------------------------------------------------------

async function describeItem(item) {
  let projectItem;
  try {
    projectItem = await item.getProjectItem();
  } catch (e) {
    return { kind: "none" };
  }
  if (!projectItem) return { kind: "none" };
  let clip = null;
  try {
    clip = ppro.ClipProjectItem.cast(projectItem);
  } catch (e) { /* not a clip */ }
  if (!clip) return { kind: "none" };
  try {
    if (await clip.isMulticamClip()) return { kind: "multicam" };
    if (await clip.isSequence()) return { kind: "sequence", sequence: await clip.getSequence() };
    if (await clip.isOffline()) return { kind: "offline" };
    const path = await clip.getMediaFilePath();
    if (!path) return { kind: "none" };
    if (STILL_EXT.test(path)) return { kind: "still", path };
    return { kind: "media", path };
  } catch (e) {
    return { kind: "none" };
  }
}

/** All enabled video clips of a sequence, with timing and what they contain. */
async function readClips(sequence, { skipMutedTracks }) {
  const clips = [];
  const trackCount = await sequence.getVideoTrackCount();
  for (let t = 0; t < trackCount; t++) {
    const track = await sequence.getVideoTrack(t);
    if (!track) continue;
    if (skipMutedTracks && (await track.isMuted())) continue;
    const items = await track.getTrackItems(ppro.Constants.TrackItemType.CLIP, false);
    for (const item of items || []) {
      if (await item.isDisabled()) continue;
      const start = secs(await item.getStartTime());
      const end = secs(await item.getEndTime());
      if (end - start <= 0) continue;
      const inPoint = secs(await item.getInPoint());
      const outPoint = secs(await item.getOutPoint());
      clips.push({
        item,
        track: t,
        start,
        end,
        inPoint,
        // Source seconds per timeline second; derived from the trim so it
        // doesn't depend on how getSpeed() reports percentages.
        speed: outPoint > inPoint ? (outPoint - inPoint) / (end - start) : 1,
        reversed: Boolean(await item.isSpeedReversed()),
        selected: Boolean(await item.getIsSelected()),
        name: await item.getName(),
        content: await describeItem(item),
      });
    }
  }
  return clips;
}

function subtractIntervals(start, end, holes) {
  let pieces = [[start, end]];
  for (const [hs, he] of holes) {
    const next = [];
    for (const [s, e] of pieces) {
      if (he <= s || hs >= e) {
        next.push([s, e]);
        continue;
      }
      if (hs > s) next.push([s, hs]);
      if (he < e) next.push([he, e]);
    }
    pieces = next;
  }
  return pieces.filter(([s, e]) => e - s > 0.04);
}

/**
 * Segments of source media visible in `sequence`, in that sequence's time.
 * Each segment: { mediaPath, srcIn, srcOut, seqStart, speed, clipName, track }.
 */
async function sequenceSegments(sequence, opts, warnings, depth) {
  const clips = await readClips(sequence, { skipMutedTracks: !opts.selectedOnly || depth > 0 });
  const chosen = opts.selectedOnly && depth === 0 ? clips.filter((c) => c.selected) : clips;
  const segments = [];

  for (const clip of chosen) {
    const { content } = clip;
    if (content.kind === "multicam") {
      warnings.push(`"${clip.name}" is a multicam clip — flatten it (right-click › Multi-Camera › Flatten) to analyse it.`);
      continue;
    }
    if (content.kind === "offline") {
      warnings.push(`"${clip.name}" is offline.`);
      continue;
    }
    if (content.kind !== "media" && content.kind !== "sequence") continue;
    if (clip.reversed) {
      warnings.push(`"${clip.name}" plays in reverse and was skipped.`);
      continue;
    }

    let pieces = [[clip.start, clip.end]];
    if (opts.skipCovered && !(opts.selectedOnly && depth === 0)) {
      const covers = clips
        .filter((o) => o.track > clip.track && (o.content.kind === "media" || o.content.kind === "sequence" || o.content.kind === "multicam"))
        .map((o) => [o.start, o.end]);
      pieces = subtractIntervals(clip.start, clip.end, covers);
    }

    for (const [ps, pe] of pieces) {
      const innerStart = clip.inPoint + (ps - clip.start) * clip.speed;
      const innerEnd = clip.inPoint + (pe - clip.start) * clip.speed;

      if (content.kind === "media") {
        segments.push({
          mediaPath: content.path,
          srcIn: innerStart,
          srcOut: innerEnd,
          seqStart: ps,
          speed: clip.speed,
          clipName: clip.name,
          track: clip.track,
        });
        continue;
      }

      // Nested sequence: map its segments through this clip's window.
      if (depth >= MAX_NEST_DEPTH || !content.sequence) continue;
      const inner = await sequenceSegments(content.sequence, { ...opts, selectedOnly: false }, warnings, depth + 1);
      for (const s of inner) {
        const a = s.seqStart;
        const b = s.seqStart + (s.srcOut - s.srcIn) / s.speed;
        const lo = Math.max(a, innerStart);
        const hi = Math.min(b, innerEnd);
        if (hi - lo <= 0.04) continue;
        segments.push({
          mediaPath: s.mediaPath,
          srcIn: s.srcIn + (lo - a) * s.speed,
          srcOut: s.srcIn + (hi - a) * s.speed,
          seqStart: ps + (lo - innerStart) / clip.speed,
          speed: s.speed * clip.speed,
          clipName: s.clipName,
          track: clip.track,
        });
      }
    }
  }
  return segments;
}

function clipToRange(seg, lo, hi) {
  const segEnd = seg.seqStart + (seg.srcOut - seg.srcIn) / seg.speed;
  const s = Math.max(seg.seqStart, lo);
  const e = Math.min(segEnd, hi);
  if (e - s <= 0.04) return null;
  return {
    ...seg,
    srcIn: seg.srcIn + (s - seg.seqStart) * seg.speed,
    srcOut: seg.srcIn + (e - seg.seqStart) * seg.speed,
    seqStart: s,
  };
}

async function collect() {
  const project = await ppro.Project.getActiveProject();
  if (!project) throw new Error("Open a project first.");
  const sequence = await project.getActiveSequence();
  if (!sequence) throw new Error("Open a sequence in the timeline first.");

  const warnings = [];
  const selectedOnly = state.mode === "selection";
  let segments = await sequenceSegments(
    sequence,
    { selectedOnly, skipCovered: settings.skipCovered },
    warnings,
    0
  );

  if (settings.useInOut) {
    const lo = secs(await sequence.getInPoint());
    const hi = secs(await sequence.getOutPoint());
    if (hi > lo) segments = segments.map((s) => clipToRange(s, lo, hi)).filter(Boolean);
  }

  if (!segments.length) {
    if (selectedOnly) throw new Error("Select one or more video clips in the timeline.");
    throw new Error("No video footage found in this sequence.");
  }

  const timebase = Number(await sequence.getTimebase()) || TICKS_PER_SECOND / 25;
  let zeroPoint = 0;
  try {
    zeroPoint = secs(await sequence.getZeroPoint());
  } catch (e) { /* older builds */ }
  let frameRect = null;
  try {
    frameRect = await (await sequence.getSettings()).getVideoFrameRect();
  } catch (e) {
    try { frameRect = await sequence.getFrameSize(); } catch (e2) { /* fall back to 1920x1080 */ }
  }

  segments.forEach((s, i) => (s.id = `s${i}`));
  return { project, sequence, segments, warnings, timebase, zeroPoint, frameRect };
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

async function api(path, body) {
  const res = await fetch(engineUrl(path), body === undefined ? {} : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || `Engine error ${res.status}`);
  return data;
}

async function checkEngine() {
  let cls, text, title = "";
  try {
    const h = await api("/health");
    state.engineOk = h.ok;
    cls = h.ok ? (h.busy ? "busy" : "ok") : "busy";
    text = h.ok ? (h.busy ? "Engine busy" : "Engine ready") : `Engine: ${h.model}`;
    title = h.error || "";
    // Releases bump the panel and engine together, so any difference means one is stale.
    if (h.ok && state.version && h.version && h.version !== state.version) {
      cls = "busy";
      text = `Engine v${h.version}`;
      title = `This panel is v${state.version}. Download the matching SayCheese-Engine from the release page.`;
    }
  } catch (e) {
    state.engineOk = false;
    cls = "off";
    text = "Engine offline";
  }
  setEngineStatus(cls, text, title);
  $("startEngine").classList.toggle("hidden", state.engineOk);
  setDisabled("analyze", !state.engineOk || Boolean(state.polling));
}

function setEngineStatus(cls, text, title = "") {
  // Called every few seconds, so only touch the DOM when something changed.
  const key = `${cls}|${text}|${title}`;
  if (key === state.engineKey) return;
  state.engineKey = key;
  $("engine").className = `status ${cls}`;
  $("engineText").textContent = text;
  $("engine").title = title;
}

/**
 * The engine's launcher: one picked earlier, else engine/ next to the plugin
 * folder (a development checkout). An installed .ccx lives in Creative Cloud's
 * own folder, so there the user points at start.bat once.
 */
async function engineLauncher() {
  const isMac = os.platform() === "darwin";
  const name = isMac ? "start.command" : "start.bat";
  const candidates = [];
  if (settings.engineLauncher) candidates.push(settings.engineLauncher);
  try {
    const plugin = (await fs.getPluginFolder()).nativePath.replace(/[\\/]+$/, "");
    const sep = isMac ? "/" : "\\";
    candidates.push(`${plugin.replace(/[\\/][^\\/]+$/, "")}${sep}engine${sep}${name}`);
  } catch (e) { /* no plugin folder path */ }
  for (const c of candidates) {
    try {
      if ((await nfs.lstat(c)).isFile()) return c;
    } catch (e) { /* not there */ }
  }
  const file = await fs.getFileForOpening({ types: [isMac ? "command" : "bat"] });
  if (!file) return null;
  settings.engineLauncher = file.nativePath;
  saveSettings();
  return file.nativePath;
}

async function startEngine() {
  try {
    const path = await engineLauncher();
    if (!path) {
      setStatus("Download SayCheese-Engine from the release page, unzip it, then press Start engine and pick its start.bat.");
      return;
    }
    const err = await uxp.shell.openPath(path, "Starts the SayCheese analysis engine on this computer.");
    if (err) throw new Error(err);
    setStatus("Starting engine… the first start installs it, which takes a few minutes.");
  } catch (e) {
    setStatus(`Couldn't start the engine (${e.message || e}). Run its start.bat yourself.`);
  }
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

async function analyze() {
  if (state.polling) return;
  setWarnings([]);
  note("");
  let ctx;
  try {
    setStatus("Reading timeline…");
    showProgress(true, 0);
    ctx = await collect();
  } catch (e) {
    showProgress(false);
    setStatus("");
    renderEmpty("Nothing to analyse", e.message || String(e));
    return;
  }

  const total = ctx.segments.reduce((n, s) => n + (s.srcOut - s.srcIn), 0);
  setStatus(`Sending ${ctx.segments.length} segment(s), ${Math.round(total)}s of footage…`);
  try {
    const { id } = await api("/jobs", {
      segments: ctx.segments,
      options: {
        sample_fps: Number(settings.sampleFps),
        max_results: Number(settings.maxResults),
        min_gap: Number(settings.minGap),
        priority: settings.priority,
      },
    });
    state.job = { id, ...ctx, preWarnings: ctx.warnings };
  } catch (e) {
    showProgress(false);
    setStatus("");
    renderEmpty("The engine rejected the job", e.message);
    return;
  }

  state.results = [];
  state.selected.clear();
  state.currentId = null;
  cards.clear();
  layoutKey = "";
  state.polling = setInterval(poll, 700);
  renderResults();
  $("cancel").classList.remove("hidden");
  setDisabled("analyze", true);
}

async function poll() {
  const job = state.job;
  if (!job) return;
  let snap;
  try {
    snap = await api(`/jobs/${job.id}`);
  } catch (e) {
    return stopPolling(`Lost contact with the engine: ${e.message}`);
  }
  setEngineStatus("busy", "Analysing");
  showProgress(true, snap.progress);
  setStatus(`${snap.message} · ${Math.round(snap.elapsed)}s`);
  setWarnings([...job.preWarnings, ...snap.warnings]);
  if (snap.status === "running" || snap.status === "queued") return;

  if (snap.status === "done") {
    const n = snap.results.length;
    state.results = snap.results;
    state.summary = `${n} smile${n === 1 ? "" : "s"}`;
    stopPolling(n ? "" : "No smiles found in this footage.");
    note(n ? `Found in ${Math.round(snap.elapsed)} s` : "");
  } else {
    stopPolling(snap.message);
  }
}

function stopPolling(message) {
  clearInterval(state.polling);
  state.polling = null;
  showProgress(false);
  $("cancel").classList.add("hidden");
  setStatus("");
  checkEngine();
  if (state.results.length) renderResults();
  else renderEmpty("No results", message || "No smiles found.");
}

async function cancel() {
  if (state.job) {
    try { await api(`/jobs/${state.job.id}/cancel`, {}); } catch (e) { /* engine gone */ }
  }
}

// ---------------------------------------------------------------------------
// Export folder: <project folder>/Exported/Prints, unless one was picked for
// this project
// ---------------------------------------------------------------------------

async function exportTarget() {
  let projectPath = "";
  try {
    const project = await ppro.Project.getActiveProject();
    projectPath = (project && project.path) || "";
  } catch (e) { /* no project open */ }
  const custom = settings.exportFolders[projectPath];
  if (custom) return { projectPath, path: custom, custom: true };
  if (!/[\\/]/.test(projectPath)) return { projectPath, path: null, custom: false }; // unsaved
  const sep = projectPath.includes("\\") ? "\\" : "/";
  const dir = projectPath.replace(/[\\/][^\\/]*$/, "");
  return { projectPath, path: `${dir}${sep}Exported${sep}Prints`, custom: false };
}

/** mkdir -p. UXP's fs module reaches arbitrary paths with the fullAccess permission. */
async function ensureFolder(path) {
  const sep = path.includes("\\") ? "\\" : "/";
  const parts = path.split(/[\\/]/);
  let cur = "";
  for (let i = 0; i < parts.length; i++) {
    cur = i === 0 ? parts[0] : `${cur}${sep}${parts[i]}`;
    if (!parts[i] || /^[A-Za-z]:$/.test(cur)) continue;
    try {
      await nfs.lstat(cur);
    } catch (e) {
      try { await nfs.mkdir(cur); } catch (e2) { /* share roots etc.; checked below */ }
    }
  }
  if (!(await nfs.lstat(path)).isDirectory()) throw new Error("not a folder");
}

async function chooseFolder() {
  const { projectPath } = await exportTarget();
  const folder = await fs.getFolder();
  if (!folder) return;
  settings.exportFolders[projectPath] = folder.nativePath;
  saveSettings();
  renderFooter();
}

async function resetFolder() {
  const { projectPath } = await exportTarget();
  delete settings.exportFolders[projectPath];
  saveSettings();
  renderFooter();
}

function shortPath(path) {
  const sep = path.includes("\\") ? "\\" : "/";
  const parts = path.split(/[\\/]/);
  return parts.length > 4 ? `…${sep}${parts.slice(-3).join(sep)}` : path;
}

// ---------------------------------------------------------------------------
// Actions on results
// ---------------------------------------------------------------------------

async function goTo(r) {
  const { sequence, timebase } = state.job;
  try {
    await sequence.setPlayerPosition(frameTick(r.seqTime, timebase));
    state.currentId = r.id;
    syncCards();
  } catch (e) {
    note(`Couldn't move the playhead: ${e.message || e}`);
  }
}

async function addMarkers(list) {
  if (!list.length) return;
  const { project, sequence, timebase } = state.job;
  try {
    const markers = await ppro.Markers.getMarkers(sequence);
    const pending = list.map((r) => ({
      time: frameTick(r.seqTime, timebase),
      name: `Smile #${r.rank} · ${Math.round(r.score)}`,
      comment:
        `SayCheese score ${r.score}. Smile ${pct(r.scores.smile)}, sharpness ${pct(r.scores.sharpness)}, ` +
        `stillness ${pct(r.scores.stillness)}, eyes ${pct(r.scores.eyes)}. Clip: ${r.clipName}`,
    }));
    let ok = false;
    project.lockedAccess(() => {
      ok = project.executeTransaction((compound) => {
        for (const m of pending) {
          compound.addAction(markers.createAddMarkerAction(
            m.name, ppro.Marker.MARKER_TYPE_COMMENT, m.time, ppro.TickTime.TIME_ZERO, m.comment
          ));
        }
      }, `SayCheese: add ${pending.length} smile marker(s)`);
    });
    note(ok ? `Added ${plural(pending.length, "marker")}` : "Premiere refused the markers");
  } catch (e) {
    note(`Markers failed: ${e.message || e}`);
  }
}

async function exportFrames(list) {
  if (!list.length) return;
  const { path: folder } = await exportTarget();
  if (!folder) {
    note("Save the project first, or choose a folder below.");
    return;
  }
  try {
    await ensureFolder(folder);
  } catch (e) {
    note(`Couldn't create ${folder}: ${e.message || e}`);
    return;
  }
  const { sequence, timebase, zeroPoint, frameRect } = state.job;
  const width = Math.round((frameRect && frameRect.width) || 1920);
  const height = Math.round((frameRect && frameRect.height) || 1080);
  const base = sequence.name.replace(/[\\/:*?"<>|]+/g, "_").trim() || "sequence";
  let done = 0;
  const failed = [];
  for (const r of list) {
    const tc = timecode(zeroPoint + r.seqTime, timebase).replace(/:/g, "-");
    const name = `${base}_${tc}_smile${String(r.rank).padStart(2, "0")}.${settings.exportFormat}`;
    note(`Exporting ${done + failed.length + 1} of ${list.length}…`);
    try {
      const ok = await ppro.Exporter.exportSequenceFrame(
        sequence, frameTick(r.seqTime, timebase), name, folder, width, height
      );
      if (ok) done++;
      else failed.push(r.rank);
    } catch (e) {
      failed.push(r.rank);
    }
  }
  note(`Exported ${plural(done, "frame")} to ${shortPath(folder)}` + (failed.length ? ` · failed: #${failed.join(", #")}` : ""));
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

const pct = (v) => `${Math.round(v * 100)}%`;
const plural = (n, word) => `${n} ${word}${n === 1 ? "" : "s"}`;
const clamp01 = (v) => Math.max(0, Math.min(1, v));

function scoreColor(score) {
  if (score >= 70) return "#4fd18f";
  if (score >= 50) return "#f5b83d";
  return "#ff6b61";
}

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

function metric(name, value, color) {
  const m = el("div", "metric");
  const top = el("div", "top");
  top.appendChild(el("span", "", name));
  top.appendChild(el("span", "val", String(Math.round(clamp01(value) * 100))));
  m.appendChild(top);
  const track = el("div", "track");
  const fill = el("div", "fill");
  fill.style.width = `${Math.round(clamp01(value) * 100)}%`;
  fill.style.background = color;
  track.appendChild(fill);
  m.appendChild(track);
  return m;
}

function metricRow(a, b) {
  const row = el("div", "mrow");
  row.appendChild(a);
  row.appendChild(b);
  return row;
}

function renderEmpty(title, text) {
  const main = $("results");
  main.innerHTML = "";
  layoutKey = "";
  const box = el("div", "empty");
  const img = document.createElement("img");
  img.src = "icons/plugin@2x.png";
  box.appendChild(img);
  box.appendChild(el("div", "title", title));
  if (text) box.appendChild(el("div", "text", text));
  main.appendChild(box);
  $("toolbar").classList.add("hidden");
}

// Cards are built once per result and view, then only moved between rows or
// restyled. Rebuilding them, and re-fetching every thumbnail, on each click
// was what made the panel lag.
const cards = new Map(); // "<view>:<result id>" -> card element
let layoutKey = "";

function buildCard(r) {
  const face = state.view === "face";
  const { timebase, zeroPoint, id: jobId } = state.job;
  const card = el("div", face ? "card facecard" : "card");
  card.setAttribute("data-id", r.id);
  card.classList.toggle("sel", state.selected.has(r.id));
  card.classList.toggle("current", state.currentId === r.id);

  const thumb = el("div", face ? "thumb face" : "thumb");
  const file = face && r.face ? r.face : r.thumb;
  if (file) {
    const img = document.createElement("img");
    img.src = engineUrl(`/jobs/${jobId}/files/${file}`);
    thumb.appendChild(img);
  }
  thumb.appendChild(el("div", "badge rank", `#${r.rank}`));
  const score = el("div", "badge score", String(Math.round(r.score)));
  score.style.color = scoreColor(r.score);
  thumb.appendChild(score);
  const pick = el("div", "pick");
  pick.appendChild(el("div", "tick"));
  pick.title = "Select";
  pick.addEventListener("click", (ev) => {
    ev.stopPropagation();
    if (state.selected.has(r.id)) state.selected.delete(r.id);
    else state.selected.add(r.id);
    syncCards();
  });
  thumb.appendChild(pick);
  thumb.addEventListener("click", () => goTo(r));
  card.appendChild(thumb);

  const info = el("div", "info");
  const meta = el("div", "meta");
  meta.appendChild(el("span", "tc", timecode(zeroPoint + r.seqTime, timebase)));
  const clip = el("span", "clip", r.clipName || "");
  clip.title = r.mediaPath;
  meta.appendChild(clip);
  info.appendChild(meta);
  info.appendChild(metricRow(metric("Smile", r.scores.smile, "#f5b83d"), metric("Sharp", r.scores.sharpness, "#4ea4ff")));
  info.appendChild(metricRow(metric("Still", r.scores.stillness, "#4fd18f"), metric("Eyes", r.scores.eyes, "#b694ff")));

  const actions = el("div", "actions");
  for (const [label, fn] of [["Go to", goTo], ["Marker", (x) => addMarkers([x])], ["Export", (x) => exportFrames([x])]]) {
    const b = el("div", "act", label);
    b.addEventListener("click", () => fn(r));
    actions.appendChild(b);
  }
  info.appendChild(actions);
  card.appendChild(info);
  return card;
}

function cardFor(r) {
  const key = `${state.view}:${r.id}`;
  let card = cards.get(key);
  if (!card) {
    card = buildCard(r);
    cards.set(key, card);
  }
  return card;
}

/** Selection and playhead highlight, without rebuilding anything. */
function syncCards() {
  for (const [key, card] of cards) {
    const id = key.slice(key.indexOf(":") + 1);
    card.classList.toggle("sel", state.selected.has(id));
    card.classList.toggle("current", state.currentId === id);
  }
  updateToolbar();
}

function rowCapacity() {
  return Math.max(1, Math.floor(($("results").offsetWidth - RESULTS_PADDING + CARD_GAP) / CARD_PITCH));
}

function renderResults() {
  if (!state.results.length) {
    if (state.polling) renderEmpty("Looking for smiles…", "Results appear here when the scan finishes.");
    else renderEmpty("Find the best smile", "Choose Whole sequence or Selected clips, then press Find smiles. Frames are ranked on smile, sharpness, stillness and open eyes.");
    return;
  }
  $("toolbar").classList.remove("hidden");
  $("summary").textContent = state.summary;
  const list = [...state.results].sort(
    state.sort === "time" ? (a, b) => a.seqTime - b.seqTime : (a, b) => a.rank - b.rank
  );
  const perRow = rowCapacity();
  state.perRow = perRow;
  const key = `${state.view}|${perRow}|${list.map((r) => r.id).join(",")}`;
  if (key !== layoutKey) {
    layoutKey = key;
    const main = $("results");
    main.innerHTML = "";
    let row = null;
    list.forEach((r, i) => {
      if (i % perRow === 0) {
        row = el("div", "cardrow");
        main.appendChild(row);
      }
      row.appendChild(cardFor(r));
    });
  }
  updateToolbar();
}

function updateToolbar() {
  const n = state.selected.size;
  $("markSelected").textContent = n ? `Mark ${n}` : "Mark all";
  $("exportSelected").textContent = n ? `Export ${n}` : "Export all";
  $("selectNone").classList.toggle("hidden", n === 0);
  $("selectAll").classList.toggle("hidden", n === state.results.length);
}

function chosenResults() {
  const list = state.selected.size
    ? state.results.filter((r) => state.selected.has(r.id))
    : state.results;
  return [...list].sort((a, b) => a.rank - b.rank);
}

async function renderFooter() {
  const t = await exportTarget();
  const key = `${t.path}|${t.custom}`;
  if (key === state.footerKey) return;
  state.footerKey = key;
  const sep = t.path && t.path.includes("\\") ? "\\" : "/";
  $("exportFolder").textContent = !t.path ? "save the project first"
    : t.custom ? shortPath(t.path) : `Project folder${sep}Exported${sep}Prints`;
  $("exportFolder").title = t.path || "";
  $("resetFolder").classList.toggle("hidden", !t.custom);
}

function note(text) {
  $("note").textContent = text;
}

function setStatus(text) {
  $("status").textContent = text;
  if (text) $("progressWrap").classList.remove("hidden");
  else if (!state.polling) $("progressWrap").classList.add("hidden");
}

function setWarnings(list) {
  const unique = [...new Set(list)];
  const box = $("warnings");
  box.innerHTML = "";
  unique.forEach((w) => box.appendChild(el("div", "", w)));
  box.classList.toggle("hidden", unique.length === 0);
}

function showProgress(on, value) {
  $("progressWrap").classList.toggle("hidden", !on);
  if (on) $("progress").style.width = `${Math.round((value || 0) * 100)}%`;
}

function setDisabled(id, on) {
  $(id).classList.toggle("disabled", on);
}

function onClick(id, fn) {
  $(id).addEventListener("click", () => {
    if (!$(id).classList.contains("disabled")) fn();
  });
}

/** Segmented control built from .opt divs carrying data-value. */
function bindChoice(id, value, onPick) {
  const opts = $(id).querySelectorAll(".opt");
  const mark = (v) => opts.forEach((o) => o.classList.toggle("on", o.getAttribute("data-value") === v));
  mark(String(value));
  opts.forEach((o) => o.addEventListener("click", () => {
    const v = o.getAttribute("data-value");
    mark(v);
    onPick(v);
  }));
}

function bindToggle(id, key) {
  const t = $(id);
  t.classList.toggle("on", Boolean(settings[key]));
  t.addEventListener("click", () => {
    if (t.classList.contains("disabled")) return;
    settings[key] = !settings[key];
    t.classList.toggle("on", settings[key]);
    saveSettings();
  });
}

// ---------------------------------------------------------------------------
// Update check, same scheme as Syncitol and ProMatch: once a day ask GitHub for
// the latest release and show a banner when it's newer than this panel. The
// repo must stay public: GitHub answers 404 to an anonymous request for a
// private repo's releases. Everything fails quietly and tries again tomorrow;
// clicking the version in the footer forces a check and reports either way.
// ---------------------------------------------------------------------------

const UPDATE_REPO = "thinkvp/SayCheese";
const UPDATE_API_URL = `https://api.github.com/repos/${UPDATE_REPO}/releases/latest`;
const UPDATE_PAGE_URL = `https://github.com/${UPDATE_REPO}/releases/latest`;
const UPDATE_LAST_CHECK_KEY = "saycheese-update-last-check";
const UPDATE_DISMISSED_KEY = "saycheese-update-dismissed";
const UPDATE_INTERVAL_MS = 24 * 60 * 60 * 1000;
const UPDATE_TIMEOUT_MS = 10000;

const update = { url: UPDATE_PAGE_URL, latest: null };

function lsGet(key) {
  try { return localStorage.getItem(key); } catch (e) { return null; }
}

function lsSet(key, value) {
  try { localStorage.setItem(key, value); } catch (e) { /* non-fatal */ }
}

function openExternal(url) {
  try {
    uxp.shell.openExternal(url, "Opens the SayCheese release page.");
  } catch (e) {
    setStatus(`Couldn't open ${url}: ${e.message || e}`);
  }
}

/** The running version, from the plugin's own manifest so it matches what is installed. */
async function pluginVersion() {
  try {
    const entry = await (await fs.getPluginFolder()).getEntry("manifest.json");
    return String(JSON.parse(await entry.read()).version || "");
  } catch (e) {
    return "";
  }
}

/** -1 / 0 / 1 like a comparator, or null when either isn't x.y.z. */
function compareVersions(a, b) {
  const parts = (v) => {
    const m = /^\s*v?(\d+)\.(\d+)\.(\d+)/.exec(v || "");
    return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
  };
  const pa = parts(a);
  const pb = parts(b);
  if (!pa || !pb) return null;
  for (let i = 0; i < 3; i++) if (pa[i] !== pb[i]) return pa[i] < pb[i] ? -1 : 1;
  return 0;
}

/** Briefly replaces the footer version with the result of a manual check. */
function versionNote(text) {
  $("version").textContent = `v${state.version} · ${text}`;
  setTimeout(() => { $("version").textContent = `v${state.version}`; }, 5000);
}

// `manual` = the user asked: report either way, ignore the throttle and a dismissal.
async function checkForUpdates(manual) {
  const last = Number(lsGet(UPDATE_LAST_CHECK_KEY) || 0);
  if (!manual && last && Date.now() - last < UPDATE_INTERVAL_MS) return;
  // Stamp before the request: a network that is down shouldn't mean a retry on every open.
  lsSet(UPDATE_LAST_CHECK_KEY, String(Date.now()));
  if (manual) $("version").textContent = `v${state.version} · checking…`;

  let tag = null;
  try {
    const res = await Promise.race([
      fetch(UPDATE_API_URL, { headers: { Accept: "application/vnd.github+json" } }),
      new Promise((_, reject) => setTimeout(() => reject(new Error("timed out")), UPDATE_TIMEOUT_MS)),
    ]);
    if (!res.ok) throw new Error(`GitHub replied ${res.status}`);
    const data = await res.json();
    tag = data && data.tag_name;
    if (data && data.html_url) update.url = data.html_url;
  } catch (e) {
    if (manual) {
      versionNote("couldn't check");
      openExternal(UPDATE_PAGE_URL);
    }
    return;
  }

  const diff = compareVersions(tag, state.version);
  if (diff === null) {
    if (manual) versionNote("couldn't read the latest release");
    return;
  }
  if (diff <= 0) {
    if (manual) versionNote("up to date");
    return;
  }
  update.latest = String(tag).replace(/^v/, "");
  if (manual) $("version").textContent = `v${state.version}`;
  if (!manual && lsGet(UPDATE_DISMISSED_KEY) === update.latest) return;
  $("updateText").textContent = `SayCheese v${update.latest} is available (you have v${state.version}).`;
  $("update").classList.remove("hidden");
}

async function initUpdates() {
  state.version = await pluginVersion();
  $("version").textContent = state.version ? `v${state.version}` : "";
  onClick("version", () => checkForUpdates(true));
  onClick("updateOpen", () => openExternal(update.url));
  onClick("updateClose", () => {
    if (update.latest) lsSet(UPDATE_DISMISSED_KEY, update.latest);
    $("update").classList.add("hidden");
  });
  if (state.version) checkForUpdates(false).catch(() => {});
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

function init() {
  for (const key of ["sampleFps", "maxResults", "priority", "minGap", "exportFormat"]) {
    bindChoice(key, settings[key], (v) => {
      settings[key] = v;
      saveSettings();
    });
  }
  bindToggle("skipCovered", "skipCovered");
  bindToggle("useInOut", "useInOut");

  bindChoice("mode", state.mode, (m) => {
    state.mode = m;
    $("skipCovered").classList.toggle("disabled", m === "selection");
  });
  bindChoice("sort", state.sort, (s) => { state.sort = s; renderResults(); });
  bindChoice("view", state.view, (v) => { state.view = v; renderResults(); });

  onClick("analyze", analyze);
  onClick("cancel", cancel);
  onClick("startEngine", startEngine);
  onClick("chooseFolder", chooseFolder);
  onClick("resetFolder", resetFolder);
  onClick("selectAll", () => {
    state.results.forEach((r) => state.selected.add(r.id));
    syncCards();
  });
  onClick("selectNone", () => {
    state.selected.clear();
    syncCards();
  });
  onClick("markSelected", () => addMarkers(chosenResults()));
  onClick("exportSelected", () => exportFrames(chosenResults()));

  // Rows are built for the current width, so re-row the cards when the panel resizes.
  setInterval(() => {
    if (state.results.length && rowCapacity() !== state.perRow) renderResults();
  }, 500);

  renderFooter();
  renderResults();
  initUpdates().then(checkEngine);
  setInterval(() => {
    if (!state.polling) checkEngine();
    renderFooter();
  }, 4000);
}

init();
