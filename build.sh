#!/bin/sh
# Builds the Papyrus engine as a single static Windows x64 exe.
# Needs zig (pip install ziglang). Works on Linux/macOS (cross-compile) and on Windows under Git Bash.
set -e
PY="${PYTHON:-python3}"
VER="$(tr -d ' \r\n' < VERSION)"
mkdir -p dist
"$PY" -m ziglang c++ -target x86_64-windows-gnu -std=c++20 -O2 -municode -static \
  -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
  -DPAPYRUS_VER="\"$VER\"" \
  engine/src/main.cpp -o dist/papyrus-engine.exe \
  -ld3d11 -ldxgi -lavrt -lpdh -lpsapi -lwinmm -luser32 -lole32 -lshell32 -s
ls -l dist/papyrus-engine.exe
