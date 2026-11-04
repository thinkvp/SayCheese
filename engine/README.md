# Python reference engine

This is SayCheese's original analysis engine (Python, MediaPipe, OpenCV and ffmpeg). It's no longer shipped: since 0.2.0 the panel runs the C++ port in `native/` as a UXP addon.

It's kept as the reference the addon is checked against. To compare results on a clip:

```bash
engine\setup.bat
engine\.venv\Scripts\python engine\analyzer.py "D:\footage\interview.mp4" --start 300 --end 360 --results 8
native\build\out\saycheese-cli.exe "D:\footage\interview.mp4" 300 360 8
```

`server.py` is the HTTP service the panel used before 0.2.0; nothing uses it now.
