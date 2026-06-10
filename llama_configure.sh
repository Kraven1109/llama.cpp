#!/bin/bash

echo "============================================"
echo " llama.cpp CUDA Configure - Video Support"
echo "============================================"

# ── Check for CUDA ───────────────────────────
if ! command -v nvcc &> /dev/null; then
    echo "[ERROR] nvcc not found. Please ensure CUDA is installed and in PATH."
    exit 1
fi

# ── Check for CMake ──────────────────────────
if ! command -v cmake &> /dev/null; then
    echo "[ERROR] cmake not found. Please install CMake."
    exit 1
fi

# ── Check for Ninja ──────────────────────────
if ! command -v ninja &> /dev/null; then
    echo "[ERROR] ninja not found. Please install Ninja."
    exit 1
fi

echo "[OK] CUDA, CMake, Ninja all found."

# ── Create and enter build dir ───────────────
BUILD_DIR="/DATA1/quang_dev/llama.cpp_kraven/build-linux-cuda"
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi
cd "$BUILD_DIR"

# ── CMake configure ──────────────────────────
cmake .. \
    -G "Ninja" \
    -DGGML_CUDA=ON \
    -DGGML_NATIVE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_CUDA_ARCHITECTURES=89 \
    "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler" \
    -DGGML_CUDA_FA_ALL_QUANTS=ON \
    -DGGML_CUDA_FORCE_MMQ=OFF

if [ $? -ne 0 ]; then
    echo "[ERROR] CMake configure failed."
    exit 1
fi

echo ""
echo "[DONE] Configure successful. Run llama_build.sh to compile."