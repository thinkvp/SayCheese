# Handover: macOS build of SayCheese

This is the brief for building SayCheese for macOS. It was written at v0.2.0 (Windows only) by the session that built the Windows version. Everything needed is in this repo, plus Syncitol (`thinkvp/Syncitol`) as a working example of a Mac UXP addon.

## Goal

Ship one `.ccx` that installs on Windows x64 **and Apple Silicon Macs**, with the Mac addon built in CI like Syncitol's.

Done means:
- the Release workflow builds the Mac addon on `macos-latest` and the `.ccx` carries both platforms;
- on a Mac with Premiere Pro 26, the panel shows **Ready**, *Find smiles* finds and ranks smiles, thumbnails switch to graded versions ("colour matched in N s"), and Go to, Marker and Export work;
- results match the Windows build on the reference clips (see *Checking the results*);
- the Windows build is unchanged and CI stays green.

Release it as **v0.3.0** (`npm run version:set -- 0.3.0`; see *Releasing* in the README).

## Scope decisions already made

- **Apple Silicon only.** Google's `mediapipe` wheels have been arm64-only on macOS since 0.10.21, and 0.10.21's Intel wheel predates the C library (`libmediapipe`) SayCheese loads (checked: it isn't in that wheel). Intel would mean building MediaPipe from source with Bazel. Not doing that.
- **One `.ccx` for both platforms**, like Syncitol, so nobody downloads the wrong file. Expect about 70 MB, because the Mac `libmediapipe.dylib` is 112 MB uncompressed.
- On an Intel Mac the addon won't load; the panel already shows "Engine didn't load" with the error as a tooltip. Making that message say "Apple Silicon Macs only" would be a nice touch.

## How SayCheese is built (Windows today)

Read `README.md` first; *How it works* and *Development* cover this. In short:

```
plugin/                     the UXP panel; this folder is the .ccx
  main.js                   timeline reading, UI, calls the addon (require("saycheese.uxpaddon"))
  manifest.json             v6, "addon": {"name": "saycheese.uxpaddon"}, enableAddon
  win/x64/saycheese.uxpaddon   committed build of native/ (Windows)
  win/x64/libmediapipe.dll     fetched by npm run deps (gitignored)
  models/face_landmarker.task  fetched by npm run deps (gitignored)
native/
  src/*.cpp, *.h            the engine: frame_reader (FFmpeg), face (MediaPipe + sharpness + eye crops),
                            eye_model (CNN, weights in eye_model_weights.h), jpeg_writer, job (pipeline),
                            module_saycheese (UXP glue), cli (test harness)
  uxp/src/                  Adobe's UXP addon C glue, copied from Syncitol
  build-ffmpeg-win.sh/.bat  static FFmpeg 8.1.2, MSVC
  build-addon-win.bat       cl.exe build of the addon + saycheese-cli.exe
scripts/fetch-deps.js       downloads libmediapipe + model, pinned SHA-256
scripts/build.js            zips plugin/ into dist/SayCheese-<v>.ccx (requires the files it lists in REQUIRED)
.github/workflows/          ci.yml (push to main), release.yml (tag v*)
engine/                     the original Python engine, the reference for result checks
```

The C++ was written to be portable: `face.cpp` and `job.cpp` already have `dlopen`/`dladdr` branches, look for `libmediapipe.dylib`, and use `/` paths and `std::filesystem::u8path`. It has only ever been compiled with MSVC, so expect some clang fixes.

## Steps

1. **FFmpeg for Mac.** Start from Syncitol's `uxp/scripts/build-ffmpeg-mac.sh`. It handles download, clang flags and the `--disable-inline-asm` workaround. Swap its audio configure flags for SayCheese's from `native/build-ffmpeg-win.sh`:
   ```
   --disable-shared --enable-static --disable-debug --disable-programs --disable-doc
   --disable-avdevice --disable-avfilter --disable-swresample --disable-network --disable-everything
   --enable-protocol=file
   --enable-demuxer=mov,mxf,matroska,mpegts,avi,image2
   --enable-decoder=h264,hevc,prores,mpeg2video,mpeg4,dnxhd,mjpeg
   --enable-parser=h264,hevc,mpegvideo,mpeg4video,dnxhd,mjpeg
   --enable-encoder=mjpeg
   ```
   Keep swscale; the addon uses it. Don't add `--enable-small`: video decoding needs the speed. arm64 only, so no `lipo`. Put it in `native/build-ffmpeg-mac.sh`, building into `native/build/ffmpeg-out`.
2. **Addon build.** Write `native/CMakeLists.txt` from Syncitol's `uxp/native/CMakeLists.txt`.
   - Sources: everything `build-addon-win.bat` compiles (`frame_reader face eye_model jpeg_writer job` + `module_saycheese` + the three `uxp/src/utilities/*.cpp`).
   - Outputs: `saycheese.uxpaddon` (a dylib) and `saycheese-cli`.
   - Link: avformat, avcodec, swscale, avutil, plus the system frameworks and libs Syncitol links. VideoToolbox etc. may not be needed with no hwaccel; check what the linker asks for.
   - Default symbol visibility, as Syncitol does.
   - Ad-hoc sign the addon (`codesign -s - …`); Syncitol's release workflow verifies with `codesign --verify`.
3. **MediaPipe for Mac.** Add to `DEPS` in `scripts/fetch-deps.js`:
   - url `https://files.pythonhosted.org/packages/a3/5c/ce8750cfe2de42ab2433b2d6445f4a16225a63412d991b2970204b047d73/mediapipe-1.2.2-py3-none-macosx_11_0_arm64.whl`
   - sha256 `d3bb50923652182344ad31cfd2a09697e52952bd4bca2384ecabc90e21db9a5f`
   - extract `mediapipe/tasks/c/libmediapipe.dylib` to `plugin/mac/arm64/libmediapipe.dylib` (gitignore it like the DLL)

   Check `otool -L` on the dylib for dependencies beyond the system's, and whether it needs ad-hoc re-signing once it's out of the wheel.
4. **Where the addon goes.** Syncitol ships `mac/x64/` and `mac/arm64/` (see its `build-ccx.js`); UXP picks the folder for the running architecture. SayCheese needs `plugin/mac/arm64/saycheese.uxpaddon` next to the dylib, since `FaceAnalyzer::init` loads `libmediapipe` from the addon's own folder (`moduleDir()`). Add `mac` to `PLUGIN_FILES`, and the Mac files to `REQUIRED`, in `scripts/build.js`.
5. **CI.**
   - **release.yml:** add a `build-addon-mac` job on `macos-latest`: cached FFmpeg build, CMake build, sign, `upload-artifact`. The release job downloads it into `plugin/mac/arm64/` before `npm run build`. Copy the structure from Syncitol's `.github/workflows/uxp-release.yml`, including its "both platform addons are present" check against the finished `.ccx`.
   - **ci.yml:** at least build the Mac addon on pushes that touch `native/`.
   - **Smoke test (worth having):** on the Mac runner, run `saycheese-cli --thumbs <some face JPEG> <dir> t`. That proves libmediapipe loads, init works, a JPEG decodes and a face is found. Any JPEG with a clear face will do, such as a frame cut from `docs/screenshots/SayCheese UI.png`.

   The Windows addon stays committed and built locally (`native\build-addon-win.bat`), as now. Unlike Syncitol, there's no reason to commit a Mac addon if CI builds it.
6. **Build and test on this Mac.** Build locally first: run the FFmpeg script, CMake, `npm run deps`, then `npm run build`. Load `plugin/manifest.json` in UXP Developer Tools, then work through *Checking the results* below.

## Checking the results

Reference footage is on the NAS (`\\10.10.1.3\thinkvp`, `Z:` on the Windows machine; on the Mac probably `/Volumes/thinkvp`). Paths below are from the share root:

- A7S3: `Videography/2022/2022-12-09 Australia Owner Manager - OMP22 QLD/Video Files/2022-09-12 A7S3/20221209_C6469.MP4` (1080p H.264 long-GOP, 11.5 min)
- FX6: `…/Video Files/2022-09-12 FX6/111_2124.MXF` (1080p 10-bit H.264 long-GOP)

What the Windows build gives (`saycheese-cli <file> <start> <end> <results> 4 balanced`):

| Run | Expected |
|---|---|
| A7S3 300–360 s, 8 results | #1 at **331.640 s, score 93.7**; then about 342.68 (87.0), 321.48 (85.4), 355.92 (84.1), 349.40 (82.0). The Python engine gives the same #1. |
| A7S3 full clip, 24 results | top 3 at 455.8, 674.56, **331.64 s** (#3, the editor's reference smile); about 51 s on Windows |
| FX6 full clip, 24 results | top 3 at 454.48, **331.08 s** (#2), 677.84; about 59 s on Windows |

Small differences, like a frame either way or a few tenths of a point, are expected from scaling differences. Different moments in the top 5 are not.

To check the eye network after any change to `eye_model.cpp`, use the `--eye` mode of the CLI (see README). On Windows it matched OpenCV's run of the ONNX model to 3.5e-6.

In Premiere, the editor's test project is *AOM - OMP22 QLD_1_1_1-ProMatch test.prproj* (same shoot folder), sequence **"Jackson Martens - Conrad Martens Plumbing & Hot Water"**. With the editor's In/Out set, Windows found 12 smiles in about 10 s, plus about 5 s of colour matching. The editor's taste: open-eyed smiles beat the squinty peak of a laugh, and a blink rules a frame out.

## Gotchas already found (Premiere 26 UXP, Windows)

- Manifest changes need *Unload* then *Load* in UXP Developer Tools; Watch only reloads HTML/JS/CSS.
- A loaded addon can't be replaced while Premiere runs; a rebuilt addon needs a Premiere restart. On Windows the build renames the loaded copy aside. On macOS, overwriting a loaded dylib in place can crash Premiere, so write the new file next to it and `mv` it over the old one.
- `ppro.Exporter.exportSequenceFrame` resolves **before** the file is on disk. `main.js` polls the file size (`waitForFile`) before using it.
- Thumbnails are written under `fs.getTemporaryFolder()` and shown with `plugin-temp:/saycheese/job-N/<file>` URLs. Check that those display on macOS too.
- UXP CSS: `opacity` didn't apply to styled divs, `flex-wrap` collapses rows, and native `<button>`s have a fixed size. That's why the layout is built from divs with explicit rows. It should look the same on Mac, but check.
- MediaPipe runs each landmarker on one core; the addon creates `cores / 3` of them (at most 6) and splits the work. Apple Silicon core counts differ, so look at `timings` in the CLI output if it's slow.
- FFmpeg logging is set to errors only in `init()`. MediaPipe's own glog lines on stderr are harmless.

## Releasing

Once it works on this Mac and CI is green:

1. Bump the version (`npm run version:set -- 0.3.0`), update the README (*Install*, *Known limits*, the Mac build steps in *Development*), commit, tag `v0.3.0`, push.
2. Download the published `.ccx`, check it holds `win/x64/*` and `mac/arm64/*`, and install it on both machines.
3. Edit the release notes, in the same style as v0.2.0.
