"""SayCheese analysis engine: finds sharp, well-timed smiles in video.

Pipeline per job:
  1. Coarse pass   - decode each segment at a low sample rate (default 4 fps,
                     downscaled) and score smile strength per sample.
  2. Moments       - pick smile peaks, suppressing peaks closer than `min_gap`.
  3. Refine pass   - decode every frame in a short window around each peak at
                     native resolution (cropped to the face) and measure
                     sharpness (Laplacian variance) and motion (landmark
                     displacement between consecutive frames).
  4. Ranking       - normalise sharpness across the job, combine with smile,
                     eyes-open, mouth shape and head pose into one score, and
                     keep the best frame of each moment.
  5. Thumbnails    - pull the chosen frames at full resolution.
"""
from __future__ import annotations

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

ENGINE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(ENGINE_DIR, "models", "face_landmarker.task")
MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/face_landmarker/"
    "face_landmarker/float16/1/face_landmarker.task"
)
# Intel Open Model Zoo open-closed-eye-0001 (Apache-2.0). Verified against the
# zoo's published SHA-384; the GitHub copy is a byte-identical fallback.
EYE_MODEL_PATH = os.path.join(ENGINE_DIR, "models", "open_closed_eye.onnx")
EYE_MODEL_URLS = (
    "https://storage.openvinotoolkit.org/repositories/open_model_zoo/public/2022.1/"
    "open-closed-eye-0001/open_closed_eye.onnx",
    "https://github.com/georg-jung/FaceAiSharp.Models/raw/HEAD/src/"
    "FaceAiSharp.Models.OpenVino.open-closed-eye-0001/open_closed_eye.onnx",
)
EYE_MODEL_SHA384 = (
    "2615bce53b55620c629db21b043057600ccc53466f053c0a8277c43577c2db21"
    "e48f330cf9b15213016d17cddb8cba27"
)
# Eye crop side as a fraction of the distance between the eye centres. Sizing
# from eye width instead over-zooms the far eye in a three-quarter view.
EYE_CROP_SCALE = 0.75

COARSE_MAX_WIDTH = 960
REFINE_MAX_SIDE = 1280
SHARPNESS_FACE_HEIGHT = 256

# Weight presets for the "prioritise" option. Eyes-open and mouth shape are
# applied as multipliers on top, since a blink ruins a thumbnail regardless.
WEIGHTS = {
    "balanced": {"expression": 0.45, "sharpness": 0.30, "stillness": 0.15, "frontal": 0.10},
    "sharpness": {"expression": 0.30, "sharpness": 0.42, "stillness": 0.20, "frontal": 0.08},
    "smile": {"expression": 0.60, "sharpness": 0.20, "stillness": 0.10, "frontal": 0.10},
}

_NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0


class Cancelled(Exception):
    pass


def ensure_model() -> str:
    if not os.path.exists(MODEL_PATH):
        import urllib.request

        os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
        tmp = MODEL_PATH + ".part"
        urllib.request.urlretrieve(MODEL_URL, tmp)
        os.replace(tmp, MODEL_PATH)
    return MODEL_PATH


def ensure_eye_model() -> str:
    if os.path.exists(EYE_MODEL_PATH):
        return EYE_MODEL_PATH
    import hashlib
    import urllib.request

    os.makedirs(os.path.dirname(EYE_MODEL_PATH), exist_ok=True)
    errors = []
    for url in EYE_MODEL_URLS:
        try:
            with urllib.request.urlopen(url, timeout=30) as resp:
                data = resp.read()
        except Exception as e:
            errors.append(f"{url}: {e}")
            continue
        if hashlib.sha384(data).hexdigest() != EYE_MODEL_SHA384:
            errors.append(f"{url}: checksum mismatch")
            continue
        with open(EYE_MODEL_PATH, "wb") as fh:
            fh.write(data)
        return EYE_MODEL_PATH
    raise RuntimeError("Could not download the eye model:\n" + "\n".join(errors))


def ffmpeg_exe() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    import imageio_ffmpeg

    return imageio_ffmpeg.get_ffmpeg_exe()


def clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return lo if v < lo else hi if v > hi else v


# --------------------------------------------------------------------------
# Decoding
# --------------------------------------------------------------------------

def probe(path: str) -> dict:
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open media: {path}")
    fps = cap.get(cv2.CAP_PROP_FPS) or 0.0
    frames = cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0.0
    cap.release()
    if not (1.0 < fps < 400.0):
        fps = 25.0
    return {"fps": fps, "duration": frames / fps if frames > 0 else None}


def _iter_bmp(stream):
    """Split an image2pipe BMP stream into decoded frames.

    BMP carries its own size in the header, so frame dimensions (including any
    rotation ffmpeg applied) never need to be known up front.
    """
    while True:
        head = stream.read(14)
        if len(head) < 14:
            return
        size = int.from_bytes(head[2:6], "little")
        body = stream.read(size - 14)
        if len(body) < size - 14:
            return
        img = cv2.imdecode(np.frombuffer(head + body, np.uint8), cv2.IMREAD_COLOR)
        if img is not None:
            yield img


def decode(path: str, start: float, duration: float | None, vf: str, max_frames: int | None = None):
    """Yield BGR frames from ffmpeg, starting at `start` seconds into the file."""
    cmd = [ffmpeg_exe(), "-v", "error", "-nostdin", "-ss", f"{max(0.0, start):.6f}", "-i", path]
    if duration is not None:
        cmd += ["-t", f"{max(0.001, duration):.6f}"]
    if max_frames is not None:
        cmd += ["-frames:v", str(max_frames)]
    cmd += ["-an", "-sn", "-dn", "-vf", vf, "-f", "image2pipe", "-vcodec", "bmp", "-"]
    err = tempfile.TemporaryFile()
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=err, bufsize=1 << 22, creationflags=_NO_WINDOW
    )
    count = 0
    try:
        for img in _iter_bmp(proc.stdout):
            count += 1
            yield img
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
        if count == 0:
            err.seek(0)
            msg = err.read().decode("utf-8", "replace").strip()
            err.close()
            if msg:
                raise RuntimeError(f"ffmpeg could not decode {os.path.basename(path)}: {msg[-400:]}")
        else:
            err.close()


# --------------------------------------------------------------------------
# Face measurement
# --------------------------------------------------------------------------

@dataclass
class Face:
    pts: np.ndarray            # (478, 2) pixel coordinates in the analysed image
    blend: dict
    bbox: tuple                # x0, y0, x1, y1 pixels

    @property
    def height(self) -> float:
        return max(1.0, self.bbox[3] - self.bbox[1])

    @property
    def area(self) -> float:
        return (self.bbox[2] - self.bbox[0]) * (self.bbox[3] - self.bbox[1])

    def shifted(self, dx: float, dy: float) -> "Face":
        p = self.pts + np.array([dx, dy])
        x0, y0, x1, y1 = self.bbox
        return Face(p, self.blend, (x0 + dx, y0 + dy, x1 + dx, y1 + dy))

    def expression(self) -> dict:
        b = self.blend
        smile = (b.get("mouthSmileLeft", 0) + b.get("mouthSmileRight", 0)) / 2
        squint = (
            b.get("cheekSquintLeft", 0) + b.get("cheekSquintRight", 0)
            + b.get("eyeSquintLeft", 0) + b.get("eyeSquintRight", 0)
        ) / 4
        blink = max(b.get("eyeBlinkLeft", 0), b.get("eyeBlinkRight", 0))
        jaw = b.get("jawOpen", 0)
        pucker = max(b.get("mouthPucker", 0), b.get("mouthFunnel", 0))
        # Yaw proxy: nose tip position between the two face-edge landmarks.
        lx, rx, nx = self.pts[234, 0], self.pts[454, 0], self.pts[1, 0]
        yaw = (nx - lx) / (rx - lx) if abs(rx - lx) > 1e-3 else 0.5
        return {"smile": smile, "squint": squint, "blink": blink, "jaw": jaw, "pucker": pucker, "yaw": yaw}


class FaceAnalyzer:
    def __init__(self, model_path: str | None = None):
        opts = vision.FaceLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=model_path or ensure_model()),
            running_mode=vision.RunningMode.IMAGE,
            num_faces=2,
            output_face_blendshapes=True,
            min_face_detection_confidence=0.4,
            min_face_presence_confidence=0.4,
        )
        self.landmarker = vision.FaceLandmarker.create_from_options(opts)
        self.eye_net = cv2.dnn.readNetFromONNX(ensure_eye_model())

    def eyes_open(self, img: np.ndarray, face: Face) -> tuple:
        """Probability each eye is open, from the open/closed-eye classifier.

        MediaPipe's blink blendshape and lid landmarks both read eyes squeezed
        shut by laughter as open, so a dedicated classifier makes this call.
        """
        p = face.pts
        centres = [(p[33] + p[133]) / 2, (p[362] + p[263]) / 2]
        side = float(np.linalg.norm(centres[0] - centres[1])) * EYE_CROP_SCALE
        if side < 4:
            return 1.0, 1.0
        probs = []
        for c in centres:
            m = cv2.getRotationMatrix2D((float(c[0]), float(c[1])), 0, 32 / side)
            m[0, 2] += 16 - c[0]
            m[1, 2] += 16 - c[1]
            crop = cv2.warpAffine(img, m, (32, 32), flags=cv2.INTER_AREA)
            # The model was trained on infrared (grey) eye crops.
            crop = cv2.cvtColor(cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY), cv2.COLOR_GRAY2BGR)
            self.eye_net.setInput(cv2.dnn.blobFromImage(crop, 1 / 255.0, (32, 32), (127, 127, 127)))
            out = self.eye_net.forward().reshape(-1)
            # The zoo docs list [open, closed]; in practice index 1 is open.
            probs.append(float(out[1]))
        return tuple(probs)

    def detect(self, bgr: np.ndarray) -> list[Face]:
        h, w = bgr.shape[:2]
        rgb = np.ascontiguousarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
        res = self.landmarker.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb))
        faces = []
        for lms, shapes in zip(res.face_landmarks, res.face_blendshapes):
            pts = np.array([[p.x * w, p.y * h] for p in lms], dtype=np.float64)
            x0, y0 = pts.min(axis=0)
            x1, y1 = pts.max(axis=0)
            blend = {c.category_name: float(c.score) for c in shapes}
            faces.append(Face(pts, blend, (x0, y0, x1, y1)))
        return faces

    def find(self, img: np.ndarray, hint: tuple | None = None) -> Face | None:
        """Largest face in `img`. `hint` (a previous bbox) is tried first as a
        zoomed crop, which keeps small faces in wide shots detectable."""
        h, w = img.shape[:2]
        if hint is not None:
            crop, ox, oy = square_crop(img, hint, 2.4)
            if crop is not None:
                faces = self.detect(crop)
                if faces:
                    return max(faces, key=lambda f: f.area).shifted(ox, oy)
        faces = self.detect(img)
        if not faces:
            # Interview framing often puts the subject off-centre and small;
            # square tiles give the short-range detector a closer look.
            side = min(h, w)
            for frac in (0.0, 0.5, 1.0):
                ox = int((w - side) * frac) if w > h else 0
                oy = int((h - side) * frac) if h > w else 0
                found = self.detect(img[oy:oy + side, ox:ox + side])
                if found:
                    faces = [f.shifted(ox, oy) for f in found]
                    break
                if w == h:
                    break
        return max(faces, key=lambda f: f.area) if faces else None


def square_crop(img: np.ndarray, bbox: tuple, scale: float):
    h, w = img.shape[:2]
    x0, y0, x1, y1 = bbox
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    side = max(x1 - x0, y1 - y0) * scale
    if side < 16:
        return None, 0, 0
    ax0, ay0 = int(max(0, cx - side / 2)), int(max(0, cy - side / 2))
    ax1, ay1 = int(min(w, cx + side / 2)), int(min(h, cy + side / 2))
    if ax1 - ax0 < 16 or ay1 - ay0 < 16:
        return None, 0, 0
    return img[ay0:ay1, ax0:ax1], ax0, ay0


def sharpness(img: np.ndarray, face: Face) -> float:
    """Variance of the Laplacian over the face, at a fixed face height so the
    number is comparable between close-ups and wide shots."""
    x0, y0, x1, y1 = (int(round(v)) for v in face.bbox)
    h, w = img.shape[:2]
    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w, x1), min(h, y1)
    if x1 - x0 < 8 or y1 - y0 < 8:
        return 0.0
    gray = cv2.cvtColor(img[y0:y1, x0:x1], cv2.COLOR_BGR2GRAY)
    scale = SHARPNESS_FACE_HEIGHT / gray.shape[0]
    gray = cv2.resize(gray, None, fx=scale, fy=scale,
                      interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_CUBIC)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)  # keep sensor noise from reading as detail
    return float(cv2.Laplacian(gray, cv2.CV_64F).var())


# --------------------------------------------------------------------------
# Job
# --------------------------------------------------------------------------

@dataclass
class Segment:
    id: str
    media_path: str
    src_in: float
    src_out: float
    seq_start: float
    speed: float = 1.0
    clip_name: str = ""
    track: int = 0

    def seq_time(self, src_t: float) -> float:
        return self.seq_start + (src_t - self.src_in) / self.speed


@dataclass
class Options:
    sample_fps: float = 4.0
    max_results: int = 24
    min_gap: float = 3.0
    min_smile: float = 0.15
    window: float = 0.75
    priority: str = "balanced"

    @classmethod
    def from_dict(cls, d: dict) -> "Options":
        o = cls()
        for k in ("sample_fps", "min_gap", "min_smile", "window"):
            if k in d:
                setattr(o, k, float(d[k]))
        if "max_results" in d:
            o.max_results = max(1, int(d["max_results"]))
        if d.get("priority") in WEIGHTS:
            o.priority = d["priority"]
        return o


@dataclass
class Moment:
    segment: Segment
    t: float
    coarse_smile: float
    bbox_n: tuple                      # normalised face bbox from the coarse pass
    frames: list = field(default_factory=list)
    best: dict | None = None


class Job:
    def __init__(self, job_id: str, segments: list[Segment], options: Options, out_dir: str):
        self.id = job_id
        self.segments = segments
        self.options = options
        self.out_dir = out_dir
        self.status = "queued"
        self.progress = 0.0
        self.message = "Queued"
        self.warnings: list[str] = []
        self.results: list[dict] = []
        self.started = None
        self.finished = None
        self.cancel_requested = False

    def snapshot(self, include_results: bool = True) -> dict:
        d = {
            "id": self.id, "status": self.status, "progress": round(self.progress, 4),
            "message": self.message, "warnings": self.warnings,
            "elapsed": round((self.finished or time.time()) - self.started, 1) if self.started else 0,
        }
        if include_results:
            d["results"] = self.results
        return d

    def _check(self):
        if self.cancel_requested:
            raise Cancelled()

    def _set(self, progress: float, message: str):
        self.progress = clamp(progress)
        self.message = message

    # ---- run -------------------------------------------------------------

    def run(self, analyzer: FaceAnalyzer):
        self.started = time.time()
        self.status = "running"
        try:
            self._run(analyzer)
            self.status = "done"
            self._set(1.0, f"Found {len(self.results)} smiles")
        except Cancelled:
            self.status = "cancelled"
            self.message = "Cancelled"
        except Exception as e:  # surfaced to the panel
            self.status = "error"
            self.message = str(e)
        finally:
            self.finished = time.time()

    def _run(self, analyzer: FaceAnalyzer):
        os.makedirs(self.out_dir, exist_ok=True)
        info: dict[str, dict] = {}
        segments = []
        for seg in self.segments:
            if seg.media_path not in info:
                try:
                    info[seg.media_path] = probe(seg.media_path)
                except Exception as e:
                    info[seg.media_path] = {"error": str(e)}
            if "error" in info[seg.media_path]:
                self.warnings.append(f"{seg.clip_name or seg.media_path}: {info[seg.media_path]['error']}")
                continue
            if seg.src_out - seg.src_in > 0.05:
                segments.append(seg)

        total = sum(s.src_out - s.src_in for s in segments) or 1.0
        done = 0.0
        moments: list[Moment] = []
        for i, seg in enumerate(segments):
            label = f"Scanning {seg.clip_name or os.path.basename(seg.media_path)} ({i + 1}/{len(segments)})"
            self._set(0.75 * done / total, label)
            samples = self._coarse(analyzer, seg, lambda p: self._set(0.75 * (done + p) / total, label))
            moments += self._pick_moments(seg, samples)
            done += seg.src_out - seg.src_in

        # Keep the strongest peaks overall; refining is the expensive part.
        moments.sort(key=lambda m: m.coarse_smile, reverse=True)
        moments = moments[: max(self.options.max_results * 3, 30)]
        if not moments:
            self.warnings.append("No smiling faces were found.")
            return

        for i, m in enumerate(moments):
            self._check()
            self._set(0.75 + 0.20 * i / len(moments), f"Checking sharpness {i + 1}/{len(moments)}")
            fps = info[m.segment.media_path]["fps"]
            try:
                self._refine(analyzer, m, fps)
            except Cancelled:
                raise
            except Exception as e:
                self.warnings.append(f"Refine failed at {m.t:.2f}s in {m.segment.clip_name}: {e}")

        ranked = self._rank([m for m in moments if m.frames])
        self._set(0.95, "Grabbing thumbnails")
        results = []
        for i, m in enumerate(ranked):
            self._check()
            self._set(0.95 + 0.05 * i / max(1, len(ranked)), "Grabbing thumbnails")
            results.append(self._result(i + 1, m))
        self.results = results

    # ---- passes ----------------------------------------------------------

    def _coarse(self, analyzer: FaceAnalyzer, seg: Segment, report) -> list[tuple]:
        rate = self.options.sample_fps
        dur = seg.src_out - seg.src_in
        vf = f"fps={rate:g},scale='min({COARSE_MAX_WIDTH},iw)':-2"
        samples = []
        hint = None
        for k, img in enumerate(decode(seg.media_path, seg.src_in, dur, vf)):
            self._check()
            t = seg.src_in + k / rate
            if t >= seg.src_out:
                break
            face = analyzer.find(img, hint)
            hint = face.bbox if face else None
            if face:
                h, w = img.shape[:2]
                x0, y0, x1, y1 = face.bbox
                samples.append((t, face.expression()["smile"], (x0 / w, y0 / h, x1 / w, y1 / h)))
            if k % 8 == 0:
                report(min(dur, k / rate))
        return samples

    def _pick_moments(self, seg: Segment, samples: list[tuple]) -> list[Moment]:
        picked: list[Moment] = []
        for t, smile, bbox in sorted(samples, key=lambda s: s[1], reverse=True):
            if smile < self.options.min_smile:
                break
            if all(abs(t - m.t) >= self.options.min_gap for m in picked):
                picked.append(Moment(seg, t, smile, bbox))
        return picked

    def _refine(self, analyzer: FaceAnalyzer, m: Moment, fps: float):
        seg = m.segment
        half = self.options.window
        start = max(seg.src_in, m.t - half)
        end = min(seg.src_out, m.t + half)
        start = math.ceil(start * fps - 1e-6) / fps  # land on a frame boundary
        if end - start < 1 / fps:
            end = start + 1 / fps

        # Crop around the face (generously, it may move) before ffmpeg hands the
        # frames over: refining at native resolution is only cheap this way.
        x0, y0, x1, y1 = m.bbox_n
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        hw, hh = (x1 - x0) * 1.25, (y1 - y0) * 1.25
        cx0, cy0 = clamp(cx - hw), clamp(cy - hh)
        cx1, cy1 = clamp(cx + hw), clamp(cy + hh)
        vf = (
            f"crop=trunc(iw*{cx1 - cx0:.5f}/2)*2:trunc(ih*{cy1 - cy0:.5f}/2)*2:"
            f"iw*{cx0:.5f}:ih*{cy0:.5f},scale='min({REFINE_MAX_SIDE},iw)':-2"
        )
        prev = None
        hint = None
        for i, img in enumerate(decode(seg.media_path, start, end - start, vf)):
            self._check()
            t = start + i / fps
            if t >= end:
                break
            face = analyzer.find(img, hint)
            if face is None:
                prev = hint = None
                continue
            hint = face.bbox
            ex = face.expression()
            motion = None
            if prev is not None:
                motion = float(np.linalg.norm(face.pts - prev.pts, axis=1).mean() / face.height)
            h, w = img.shape[:2]
            fx0, fy0, fx1, fy1 = face.bbox
            eye_r, eye_l = analyzer.eyes_open(img, face)
            m.frames.append({
                "t": t,
                "lap": sharpness(img, face),
                "eye_open": (eye_r + eye_l) / 2,
                "motion": motion,
                "bbox": (
                    cx0 + fx0 / w * (cx1 - cx0), cy0 + fy0 / h * (cy1 - cy0),
                    cx0 + fx1 / w * (cx1 - cx0), cy0 + fy1 / h * (cy1 - cy0),
                ),
                "face_px": face.height,
                **ex,
            })
            prev = face
        # Motion is measured backwards; the first frame borrows its neighbour's.
        for a, b in zip(m.frames, m.frames[1:]):
            if a["motion"] is None:
                a["motion"] = b["motion"]

    def _rank(self, moments: list[Moment]) -> list[Moment]:
        laps = np.array([math.log1p(f["lap"]) for m in moments for f in m.frames])
        lo, hi = np.percentile(laps, 5), np.percentile(laps, 95)
        span = max(hi - lo, 1e-6)
        w = WEIGHTS[self.options.priority]
        for m in moments:
            for f in m.frames:
                sharp = clamp((math.log1p(f["lap"]) - lo) / span)
                motion = f["motion"] if f["motion"] is not None else 0.01
                still = 1.0 / (1.0 + (motion / 0.012) ** 2)
                # No bonus for cheek/eye squint: it favours the deepest point of
                # a laugh, where eyes are narrowest, over the open-eyed smile
                # just before it that editors tend to pick.
                expression = clamp(f["smile"])
                # The eye classifier catches eyes shut by laughter; the blink
                # blendshape backs it up on winks in wide shots, where eyes are too
                # small for the classifier (winks read ~0.6, open smiles <= ~0.4).
                eyes = min(clamp((f["eye_open"] - 0.2) / 0.7), clamp(1 - (f["blink"] - 0.45) / 0.3))
                mouth = 1 - 0.6 * clamp((f["pucker"] - 0.25) / 0.4)
                if f["jaw"] > 0.4 and f["smile"] < 0.45:
                    mouth *= 0.7  # mid-word rather than a smile
                frontal = clamp(1 - (abs(f["yaw"] - 0.5) - 0.15) / 0.3)
                quality = (
                    w["expression"] * expression + w["sharpness"] * sharp
                    + w["stillness"] * still + w["frontal"] * frontal
                )
                f["scores"] = {
                    "smile": expression, "sharpness": sharp, "stillness": still,
                    "eyes": eyes, "mouth": mouth, "frontal": frontal,
                }
                f["total"] = 100 * quality * (0.4 + 0.6 * eyes) * (0.75 + 0.25 * mouth)
            m.best = max(m.frames, key=lambda f: f["total"])

        ranked: list[Moment] = []
        for m in sorted(moments, key=lambda m: m.best["total"], reverse=True):
            st = m.segment.seq_time(m.best["t"])
            if all(abs(st - r.segment.seq_time(r.best["t"])) >= self.options.min_gap for r in ranked):
                ranked.append(m)
            if len(ranked) >= self.options.max_results:
                break
        return ranked

    def _result(self, rank: int, m: Moment) -> dict:
        seg, f = m.segment, m.best
        cid = f"c{rank:03d}"
        thumb, face = self._thumbnail(seg.media_path, f["t"], f["bbox"], cid)
        return {
            "id": cid,
            "rank": rank,
            "score": round(f["total"], 1),
            "scores": {k: round(v, 3) for k, v in f["scores"].items()},
            "raw": {k: round(f[k], 3) for k in ("smile", "squint", "blink", "eye_open", "jaw", "pucker", "yaw", "lap")},
            "srcTime": round(f["t"], 4),
            "seqTime": round(seg.seq_time(f["t"]), 4),
            "segmentId": seg.id,
            "clipName": seg.clip_name,
            "mediaPath": seg.media_path,
            "track": seg.track,
            "facePx": round(f["face_px"]),
            "thumb": thumb,
            "face": face,
        }

    def _thumbnail(self, path: str, t: float, bbox_n: tuple, cid: str):
        frame = next(iter(decode(path, t, None, "null", max_frames=1)), None)
        if frame is None:
            return None, None
        h, w = frame.shape[:2]
        thumb_name, face_name = f"{cid}.jpg", f"{cid}_face.jpg"
        tw = 384  # panel cards are 188 px wide; 2x covers HiDPI without UXP scaling down large images
        small = cv2.resize(frame, (tw, max(1, round(h * tw / w))), interpolation=cv2.INTER_AREA)
        cv2.imwrite(os.path.join(self.out_dir, thumb_name), small, [cv2.IMWRITE_JPEG_QUALITY, 88])
        x0, y0, x1, y1 = bbox_n[0] * w, bbox_n[1] * h, bbox_n[2] * w, bbox_n[3] * h
        crop, _, _ = square_crop(frame, (x0, y0, x1, y1), 1.5)
        if crop is None:
            return thumb_name, None
        crop = cv2.resize(crop, (320, 320), interpolation=cv2.INTER_AREA)
        cv2.imwrite(os.path.join(self.out_dir, face_name), crop, [cv2.IMWRITE_JPEG_QUALITY, 88])
        return thumb_name, face_name


# --------------------------------------------------------------------------
# CLI for testing without Premiere
# --------------------------------------------------------------------------

def main(argv=None):
    import argparse
    import json

    ap = argparse.ArgumentParser(description="Find the best smiles in a video file.")
    ap.add_argument("video")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--fps", type=float, default=4.0)
    ap.add_argument("--results", type=int, default=12)
    ap.add_argument("--priority", choices=list(WEIGHTS), default="balanced")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "saycheese", "cli"))
    a = ap.parse_args(argv)

    end = a.end
    if end is None:
        end = probe(a.video)["duration"] or 0.0
    seg = Segment("s0", a.video, a.start, end, a.start, clip_name=os.path.basename(a.video))
    opts = Options(sample_fps=a.fps, max_results=a.results, priority=a.priority)
    job = Job("cli", [seg], opts, a.out)
    t0 = time.time()
    job.run(FaceAnalyzer())
    with open(os.path.join(a.out, "results.json"), "w") as fh:
        json.dump(job.snapshot(), fh, indent=1)
    print(json.dumps({k: v for k, v in job.snapshot().items() if k != "results"}, indent=2))
    for r in job.results:
        s = r["scores"]
        print(f"#{r['rank']:>2} {r['score']:5.1f}  t={r['srcTime']:8.3f}s  smile={s['smile']:.2f} "
              f"sharp={s['sharpness']:.2f} still={s['stillness']:.2f} eyes={s['eyes']:.2f} "
              f"(open={r['raw']['eye_open']:.2f} blink={r['raw']['blink']:.2f})  {r['thumb']}")
    print(f"thumbnails in {a.out}  ({time.time() - t0:.1f}s)")


if __name__ == "__main__":
    main()
