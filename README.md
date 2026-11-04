# SayCheese

A Premiere Pro panel that sweeps a sequence (or the selected clips) for smiles and ranks them as thumbnail candidates. It favours frames with no motion blur, open eyes and a natural mouth shape.

```
plugin/   UXP panel (Premiere Pro 25.6+)
engine/   Local analysis service (Python: MediaPipe Face Landmarker + OpenCV + ffmpeg)
```

## Why two parts

UXP panels can't run ML models or decode video frames themselves. The panel reads the timeline and sends the visible pieces of source media to a small engine running on `127.0.0.1:47820`. The engine decodes the original files with ffmpeg, which is far faster than rendering frames through Premiere. Thumbnails and the final frame export still come from Premiere, so exported frames include your grade, effects and reframing.

## Install

From the [latest release](https://github.com/thinkvp/SayCheese/releases/latest):

1. **Panel**: download `SayCheese-x.y.z.ccx` and double-click it; Creative Cloud installs it. In Premiere, open it from *Window → UXP Plugins → SayCheese*.
2. **Engine**: download `SayCheese-Engine-x.y.z.zip` and unzip it somewhere permanent. It needs Python 3.10+ ([python.org](https://www.python.org/downloads/)).
3. In the panel, press **Start engine** and pick `start.bat` (`start.command` on macOS) in the unzipped folder. The panel remembers it. The first start creates a virtual environment, installs dependencies and downloads the face model (~3.7 MB) and eye model (46 KB), so it takes a few minutes. You can also run `start.bat` yourself.

The panel checks GitHub for a newer release once a day and shows a banner when there is one. Click the version number in the footer to check now. Update the engine zip together with the panel; the engine status turns amber when their versions differ.

`ffmpeg` is taken from PATH if it's there; otherwise the engine uses the copy bundled with `imageio-ffmpeg`.

## Development

1. Run `engine\setup.bat` once, then `engine\start.bat` (or `engine/start.command` on macOS).
2. Load the panel in Adobe UXP Developer Tools: *Add Plugin* → `plugin/manifest.json` → *Load & Watch*. **Start engine** finds `engine/` next to `plugin/` in a checkout. Manifest changes need *Unload* and *Load*; other files reload on save.
3. `npm run lint` checks the panel script and that the version numbers agree. `npm run build` writes the `.ccx` and engine zip to `dist/`. Both need Node 22.2+ and no npm install.

### Releasing

```bash
npm run version:set -- 0.2.0
git commit -am "Release 0.2.0"
git tag v0.2.0
git push --follow-tags
```

`version:set` updates `plugin/manifest.json`, `package.json` and the engine's `VERSION` together. Pushing the tag runs the Release workflow, which checks the tag matches the manifest, builds both files and publishes them as a GitHub Release with generated notes. If you create the release in the GitHub UI instead, the workflow attaches the files to it. The repository must stay public, because the panel's update check asks GitHub anonymously.

## Using it

- **Whole sequence** analyses every enabled video clip and steps into nested sequences. With *Skip footage hidden by upper tracks* on, interview footage under B-roll is ignored. Graphics and stills on upper tracks (lower thirds) don't count as covering.
- **Selected clips** analyses only the clips selected in the timeline.
- *Only between In/Out* limits either mode to the sequence In/Out range.
- Click a thumbnail (or **Go to**) to move the playhead to that frame.
- **Marker** adds a comment marker with the scores. **Export** writes the sequence frame at full frame size through Premiere (PNG/JPEG/TIFF).
- Exports go to `Exported\Prints` next to the project file, created if needed. **Change** in the footer picks a different folder for the current project; **Use project folder** switches back.
- Tick cards (the circle at the bottom left of a thumbnail) to act on a subset; with nothing ticked, the toolbar buttons act on all results.
- The **Face** view swaps the frame thumbnails for face close-ups, which make blur easier to judge.

Options:

| Option | Effect |
|---|---|
| Sample | Coarse scan rate. 4 fps is a good default; use 8 fps for short, fast-changing clips and 2 fps for long interviews. |
| Show | Number of results. |
| Favour | Weighting preset: *Balanced*, *Sharpness* (stricter on blur) or *Smile*. |
| Gap | Minimum time between results, so you get different moments rather than ten frames of the same laugh. |

## How frames are scored

1. **Coarse scan**: each segment is decoded at the sample rate, downscaled to 960 px. MediaPipe's face blendshapes give a smile strength per sample. The face is tracked from sample to sample, and square tiles are tried when no face is found, so small, off-centre faces in wide interview shots are still detected.
2. **Moments**: smile peaks are picked with the minimum gap between them.
3. **Refine**: every frame within ±0.75 s of each peak is decoded at native resolution, cropped to the face by ffmpeg. Each frame gets:
   - **Sharpness**: variance of the Laplacian over the face, resampled to a fixed face height so close-ups and wides are comparable. It is normalised across the whole job.
   - **Stillness**: how far the face landmarks moved since the previous frame, relative to face size. Movement between frames is what produces motion blur.
   - **Smile**: MediaPipe's `mouthSmile` blendshape. There is deliberately no bonus for cheek or eye squint, which would favour the narrow-eyed peak of a laugh over the open-eyed smile just before it.
   - **Eyes**: each eye is checked by an open/closed-eye classifier, Intel Open Model Zoo's `open-closed-eye-0001` (Apache-2.0, 46 KB, checksum-verified on download). MediaPipe's blink blendshape is used as a backup for clear blinks and winks. MediaPipe alone reads eyes squeezed shut by laughter as open. Eyes act as a multiplier, because a blink ruins a thumbnail however good the rest is.
   - **Mouth**: penalises mid-word shapes (open jaw without a smile, "oo" and "o" shapes).
   - **Frontal**: a mild penalty for strong head turns.
4. The best frame of each moment is ranked by the combined score (0–100).

Times are frame-accurate within the source file. The panel snaps them to the nearest sequence frame.

## Testing the engine without Premiere

```bash
engine\.venv\Scripts\python engine\analyzer.py "D:\footage\interview.mp4" --start 60 --end 300 --results 12
```

This prints the ranked frames and writes thumbnails to `%TEMP%\saycheese\cli`.

## Known limits

- The eye classifier works on 32×32 crops, so it is less reliable on small faces in wide shots, where eyes are about 10 px tall. It may also read a strongly squinting smile as one eye closed. In practice that only nudges big squinty laughs below smiles with open eyes.
- Multicam clips are skipped with a warning. Flatten them first.
- Camera RAW formats ffmpeg can't decode (BRAW, R3D) are reported as errors. Use proxies or transcodes.
- Time remapping (variable speed) is approximated as constant speed. Reversed clips are skipped.
- The manifest requests full file-system access so exports can go next to the project without a folder prompt, and `https` launch access to open the release page.
- The manifest requests network access to `"all"` domains. Premiere's UXP rejected specific `http://127.0.0.1`/`localhost` entries ("Manifest entry not found"), with or without a port. The panel itself only ever talks to the local engine.
- Premiere opens a new floating panel at its minimum size (620×700) and ignores the preferred size. Dock it or drag it bigger for more columns.
