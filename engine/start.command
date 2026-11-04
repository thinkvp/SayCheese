#!/bin/bash
# macOS launcher (double-click, or opened by the panel's Start button).
cd "$(dirname "$0")" || exit 1
if [ ! -x .venv/bin/python ]; then
  echo "Setting up SayCheese engine..."
  python3 -m venv .venv || exit 1
  .venv/bin/python -m pip install --upgrade pip
  .venv/bin/python -m pip install -r requirements.txt || exit 1
  .venv/bin/python -c "import analyzer; analyzer.ensure_model(); analyzer.ensure_eye_model()" || exit 1
fi
exec .venv/bin/python server.py "$@"
