# SayCheese

A Premiere Pro panel that sweeps a sequence (or the selected clips) for smiles and ranks them as thumbnail candidates. It favours frames with no motion blur, open eyes and a natural mouth shape.

![SayCheese in Premiere Pro: twelve ranked smiles from an interview, in the Face view](docs/screenshots/SayCheese%20UI.png)

```
plugin/   UXP panel (Premiere Pro 26+), including the analysis addon
native/   Source of the addon: FFmpeg decoding + MediaPipe face analysis in C++
engine/   The original Python engine, kept as the reference the addon is checked against
```

## Install

Needs Premiere Pro 26 or later on 64-bit Windows or an Apple Silicon Mac (macOS 14 or later). One `.ccx` installs on both.

Download `SayCheese-x.y.z.ccx` from the [latest release](https://github.com/thinkvp/SayCheese/releases/latest) and double-click it; Creative Cloud installs it. In Premiere, open it from *Window → UXP Plugins → SayCheese*. That's all: the analysis engine is built into the panel, so there's nothing else to install or start.

The panel checks GitHub for a newer release once a day and shows a banner when there is one. Click the version number in the footer to check now. Installing a newer `.ccx` replaces the old one.

## How it works

UXP panels can't decode video or run ML models in JavaScript, so the panel carries a native addon (`plugin/win/x64/saycheese.uxpaddon` and `plugin/mac/arm64/saycheese.uxpaddon`, built from `native/`; UXP loads the one for the running platform). The addon decodes the original media with FFmpeg, built in statically, which is far faster than rendering frames through Premiere. It finds faces with Google's MediaPipe face landmarker, loaded from `libmediapipe.dll` (or `libmediapipe.dylib`) next to it. The work runs on background threads, one face detector per worker, so Premiere stays responsive and the panel polls for progress. The first thumbnails come straight from the media, as shot; Premiere then exports each result's frame and the addon remakes the card and face thumbnails from it, so they show your grade, effects and reframing, like the exported stills.

## Using it

- **Whole sequence** analyses every enabled video clip and steps into nested sequences. With *Skip footage hidden by upper tracks* on, interview footage under B-roll is ignored. Graphics and stills on upper tracks (lower thirds) don't count as covering.
- **Selected clips** analyses only the clips selected in the timeline.
- *Only between In/Out* limits either mode to the sequence In/Out range.
- Click a thumbnail (or **Go to**) to move the playhead to that frame.
- **Marker** adds a comment marker with the scores. **Export** writes the sequence frame at full frame size through Premiere (PNG/JPEG/TIFF).
- Exports go to `Exported/Prints` next to the project file, created if needed. **Change** in the footer picks a different folder for the current project; **Use project folder** switches back.
- Tick cards (the circle at the bottom left of a thumbnail) to act on a subset; with nothing ticked, the toolbar buttons act on all results.
- The **Face** view swaps the frame thumbnails for face close-ups, which make blur easier to judge.

Options:

| Option | Effect |
|---|---|
| Sample | Coarse scan rate. 4 fps is a good default; use 8 fps for short, fast-changing clips and 2 fps for long interviews. |
| Show | Number of results. |
| Favour | Weighting preset: *Balanced*, *Sharpness* (stricter on blur) or *Smile*. |
| Gap | Minimum time between results, so you get different moments rather than ten frames of the same laugh. |

An 11.5-minute 1080p interview clip on a network drive takes about a minute to scan on Windows; on an M1 Mac a 2-minute 4K highlight reel took about 30 s. The cards appear as soon as the scan finishes, showing the footage as shot, then switch one by one to frames rendered by Premiere with your grade, effects and reframing. That takes about half a second per result, and the header reports it ("colour matched in 5 s").

### Temporary files

Thumbnails live in the panel's temporary folder, which UXP provides: `%TEMP%\Adobe\UXP\PluginsStorage\PPRO\26\…\com.saycheese.smilefinder\PluginData\saycheese\job-N` on Windows, and the same path under `$TMPDIR/Adobe/UXP/PluginsStorage/PPRO/26/…` on macOS. Each scan takes well under 1 MB. Only the last five scans are kept, and the folder is emptied each time the panel starts in a new Premiere session. The full-size frames Premiere renders for colour matching are deleted as soon as their thumbnails are made.

## How frames are scored

1. **Coarse scan**: each segment is decoded at the sample rate, downscaled to 960 px, split into chunks scanned in parallel. MediaPipe's face blendshapes give a smile strength per sample. The face is tracked from sample to sample, and square tiles are tried when no face is found, so small, off-centre faces in wide interview shots are still detected.
2. **Moments**: smile peaks are picked with the minimum gap between them.
3. **Refine**: every frame within ±0.75 s of each peak is decoded at native resolution, cropped to the face. Each frame gets:
   - **Sharpness**: variance of the Laplacian over the face, resampled to a fixed face height so close-ups and wides are comparable. It is normalised across the whole job.
   - **Stillness**: how far the face landmarks moved since the previous frame, relative to face size. Movement between frames is what produces motion blur.
   - **Smile**: MediaPipe's `mouthSmile` blendshape. There is deliberately no bonus for cheek or eye squint, which would favour the narrow-eyed peak of a laugh over the open-eyed smile just before it.
   - **Eyes**: each eye is checked by an open/closed-eye classifier, Intel Open Model Zoo's `open-closed-eye-0001` (Apache-2.0), compiled into the addon. MediaPipe's blink blendshape is used as a backup for clear blinks and winks. MediaPipe alone reads eyes squeezed shut by laughter as open. Eyes act as a multiplier, because a blink ruins a thumbnail however good the rest is.
   - **Mouth**: penalises mid-word shapes (open jaw without a smile, "oo" and "o" shapes).
   - **Frontal**: a mild penalty for strong head turns.
4. The best frame of each moment is ranked by the combined score (0–100).

Times are frame-accurate within the source file. The panel snaps them to the nearest sequence frame.

## Development

Needs Node 22.2+ (no npm install). For the addon: on Windows, Visual Studio 2022 Build Tools and MSYS2 with `make` and `nasm`; on macOS, Xcode's command line tools and CMake 3.20+.

1. `npm run deps` fetches MediaPipe's library for both platforms (from Google's `mediapipe` wheels on PyPI) and the face model into `plugin/`, checked against pinned SHA-256 hashes. They're gitignored.
2. The Windows addon `plugin/win/x64/saycheese.uxpaddon` is committed, so you only need to build it when changing `native/`:
   - `native\build-ffmpeg-win.bat` builds the static FFmpeg (video decoders, swscale, and JPEG reading and writing; LGPL only) into `native/build/`. It downloads the FFmpeg 8.1.2 source first if needed; run once.
   - `native\build-addon-win.bat` builds the addon and `saycheese-cli.exe`, and copies the addon into `plugin/win/x64`. If Premiere has the old addon loaded, that copy is renamed to `*.old` (ignored by git and the build) rather than overwritten.
3. The macOS addon is built by CI and not committed. To build it on a Mac (Apple Silicon only):
   - `bash native/build-ffmpeg-mac.sh` builds the same FFmpeg for arm64 into `native/build/`; run once.
   - `bash native/build-addon-mac.sh` builds the addon and `saycheese-cli` with CMake, ad-hoc signs them, and moves the addon into `plugin/mac/arm64`. It writes the new file alongside and renames it over the old one, because overwriting a dylib Premiere has loaded can crash it.
4. Load the panel in Adobe UXP Developer Tools: *Add Plugin* → `plugin/manifest.json` → *Load & Watch*. Premiere must have *Settings → Plugins → Enable developer mode* turned on (it takes effect after a restart). Manifest changes need *Unload* and *Load*; other files reload on save. A rebuilt addon only loads after Premiere restarts.
5. `npm run lint` checks the panel script and that the versions agree. `npm run build` writes the `.ccx` to `dist/`. It needs both platforms' addons, so on Windows, download the `mac-addon` artifact from a CI run into `plugin/mac/arm64` first.

### Testing the engine without Premiere

```bash
native\build\out\saycheese-cli.exe "D:\footage\interview.mp4" 60 300 12
```

On a Mac it's `native/build/out/saycheese-cli`, with the same arguments. The build copies `libmediapipe.dll` (or `.dylib`) and the model next to it from `plugin/` (after `npm run deps`). It prints the job result JSON, with per-phase timings, and writes thumbnails to `native/build/out/cli-out`. Two more modes help when working on the addon:

- `saycheese-cli --thumbs <frame.jpg> <outDir> <name>` runs the colour-matching step on a frame exported from Premiere.
- `saycheese-cli --eye <file>` runs the eye classifier on raw 32×32 crops. That is how it was checked against OpenCV's run of the original ONNX model; `native/tools/export_eye_weights.py` regenerates the weights.

The Python engine in `engine/` still runs on its own (`engine\setup.bat`, then `engine\.venv\Scripts\python engine\analyzer.py <video> --start 60 --end 300`) for comparing results.

### Releasing

```bash
npm run version:set -- 0.3.0
git commit -am "Release 0.3.0"
git tag v0.3.0
git push --follow-tags
```

`version:set` updates `plugin/manifest.json` and `package.json` together. Pushing the tag runs the Release workflow, which builds the macOS addon on a Mac runner, checks the tag matches the manifest, fetches the dependencies, builds the `.ccx`, checks it holds both platforms' files, and publishes it as a GitHub Release with generated notes, which you can then edit. If you create the release in the GitHub UI instead, the workflow attaches the file to it. The repository must stay public, because the panel's update check asks GitHub anonymously.

## Known limits

- On macOS, Apple Silicon only, macOS 14 or later. Google ships MediaPipe's library for arm64 Macs only, built for macOS 14. On an Intel Mac, or with Premiere set to open using Rosetta, the panel says "Needs an Apple Silicon Mac".
- The eye classifier works on 32×32 crops, so it is less reliable on small faces in wide shots, where eyes are about 10 px tall. It may also read a strongly squinting smile as one eye closed. In practice that only nudges big squinty laughs below smiles with open eyes.
- Multicam clips are skipped with a warning. Flatten them first.
- Camera RAW formats the built-in FFmpeg can't decode (BRAW, R3D) are reported as errors. Use proxies or transcodes. The decoders included are H.264, HEVC, ProRes, DNxHD, MPEG-2, MPEG-4 and MJPEG, in MP4/MOV, MXF, MKV, MPEG-TS and AVI.
- Time remapping (variable speed) is approximated as constant speed. Reversed clips are skipped.
- The manifest requests full file-system access so exports can go next to the project without a folder prompt, network access to `api.github.com` for the update check, and `https` launch access to open the release page.
- Premiere opens a new floating panel at its minimum size (620×700) and ignores the preferred size. Dock it or drag it bigger for more columns.

## Licences

SayCheese is MIT-licensed. The addon statically links FFmpeg (LGPL-2.1; this repository holds the build script, so it can be relinked) and bundles MediaPipe's library and face landmarker model (Apache-2.0) and the weights of Intel's open-closed-eye-0001 model (Apache-2.0).
