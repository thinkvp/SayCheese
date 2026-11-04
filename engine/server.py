"""Local HTTP service for the SayCheese Premiere Pro panel.

Runs on 127.0.0.1 only. Jobs are processed one at a time on a worker thread
so the face model is loaded once and never shared between threads.

  GET  /health                     engine status
  POST /jobs                       {segments: [...], options: {...}} -> {id}
  GET  /jobs/<id>                  status, progress, results
  POST /jobs/<id>/cancel
  GET  /jobs/<id>/files/<name>     thumbnail JPEGs
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import shutil
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import analyzer

VERSION = "0.1.0"
DEFAULT_PORT = 47820
WORK_DIR = os.path.join(tempfile.gettempdir(), "saycheese")
KEEP_JOBS = 5

jobs: dict[str, analyzer.Job] = {}
job_order: list[str] = []
job_queue: "queue.Queue[analyzer.Job]" = queue.Queue()
state = {"model": "loading", "error": None}


def worker():
    try:
        face = analyzer.FaceAnalyzer()
        state["model"] = "ready"
    except Exception as e:
        state["model"] = "error"
        state["error"] = str(e)
        return
    while True:
        job = job_queue.get()
        if job.cancel_requested:
            job.status, job.message = "cancelled", "Cancelled"
            continue
        job.run(face)


def prune_jobs():
    while len(job_order) > KEEP_JOBS:
        old = jobs.get(job_order[0])
        if old and old.status in ("queued", "running"):
            break
        job_order.pop(0)
        if old:
            jobs.pop(old.id, None)
            shutil.rmtree(old.out_dir, ignore_errors=True)


def parse_segments(raw: list) -> list[analyzer.Segment]:
    segs = []
    for i, s in enumerate(raw):
        path = s.get("mediaPath") or ""
        if not path:
            continue
        speed = float(s.get("speed") or 1.0)
        segs.append(analyzer.Segment(
            id=str(s.get("id", i)),
            media_path=path,
            src_in=float(s["srcIn"]),
            src_out=float(s["srcOut"]),
            seq_start=float(s.get("seqStart", s["srcIn"])),
            speed=speed if speed > 0 else 1.0,
            clip_name=str(s.get("clipName", "")),
            track=int(s.get("track", 0)),
        ))
    return segs


class Handler(BaseHTTPRequestHandler):
    server_version = f"SayCheese/{VERSION}"

    def log_message(self, fmt, *args):
        if os.environ.get("SAYCHEESE_VERBOSE"):
            super().log_message(fmt, *args)

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def _json(self, code: int, body: dict):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self._cors()
        self.end_headers()
        self.wfile.write(data)

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}") if n else {}

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/health":
            return self._json(200, {
                "ok": state["model"] == "ready", "version": VERSION,
                "model": state["model"], "error": state["error"],
                "busy": any(j.status == "running" for j in jobs.values()),
            })
        m = re.fullmatch(r"/jobs/([\w-]+)", path)
        if m:
            job = jobs.get(m.group(1))
            return self._json(200, job.snapshot()) if job else self._json(404, {"error": "unknown job"})
        m = re.fullmatch(r"/jobs/([\w-]+)/files/([\w-]+\.jpg)", path)
        if m:
            job = jobs.get(m.group(1))
            file = os.path.join(job.out_dir, m.group(2)) if job else None
            if not file or not os.path.isfile(file):
                return self._json(404, {"error": "not found"})
            with open(file, "rb") as fh:
                data = fh.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "max-age=3600")
            self._cors()
            self.end_headers()
            self.wfile.write(data)
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        try:
            body = self._body()
        except Exception:
            return self._json(400, {"error": "invalid JSON"})
        if path == "/jobs":
            segs = parse_segments(body.get("segments") or [])
            if not segs:
                return self._json(400, {"error": "no segments with media to analyse"})
            job_id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
            job = analyzer.Job(job_id, segs, analyzer.Options.from_dict(body.get("options") or {}),
                               os.path.join(WORK_DIR, job_id))
            jobs[job_id] = job
            job_order.append(job_id)
            prune_jobs()
            job_queue.put(job)
            return self._json(200, {"id": job_id})
        m = re.fullmatch(r"/jobs/([\w-]+)/cancel", path)
        if m:
            job = jobs.get(m.group(1))
            if not job:
                return self._json(404, {"error": "unknown job"})
            job.cancel_requested = True
            return self._json(200, {"ok": True})
        self._json(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description="SayCheese analysis engine")
    ap.add_argument("--port", type=int, default=int(os.environ.get("SAYCHEESE_PORT", DEFAULT_PORT)))
    a = ap.parse_args()
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    os.makedirs(WORK_DIR, exist_ok=True)
    threading.Thread(target=worker, daemon=True).start()
    httpd = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"SayCheese engine {VERSION} listening on http://127.0.0.1:{a.port}  (Ctrl+C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
