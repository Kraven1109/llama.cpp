#!/bin/bash

echo "============================================"
echo " llama.cpp CUDA Build - Video Support"
echo "============================================"

# ── Enter build dir ──────────────────────────
BUILD_DIR="/DATA1/quang_dev/llama.cpp_kraven/build-linux-cuda"
if [ ! -d "$BUILD_DIR" ]; then
    echo "[ERROR] Build directory not found. Run llama_configure.sh first."
    exit 1
fi
cd "$BUILD_DIR"

# ── Build ────────────────────────────────────
echo "[INFO] Building with 16 parallel jobs..."
cmake --build . --config Release -j 16

if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed."
    exit 1
fi

echo ""
echo "[DONE] Build complete!"
echo "Binaries are in: $BUILD_DIR/bin/"
