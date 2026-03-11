OK, tao sẽ lay out toàn bộ build plan cụ thể. Đây là roadmap thực tế.

---

## Tổng quan architecture sau khi build xong

```
Python gửi:
POST /v1/chat/completions
{
  "messages": [{
    "role": "user",
    "content": [
      {"type": "video_url", "video_url": {"url": "http://...video.mp4"}},
      {"type": "text", "text": "Grade this video"}
    ]
  }]
}

llama.cpp xử lý:
video.mp4 → mtmd-helper-video.cpp (decode + adaptive sample)
          → clip.cpp (encode frames + temporal mRoPE)
          → model nhận <|video_pad|> tokens với 3D position IDs
```

Python side: xóa toàn bộ ffmpeg extraction, image server, frame selection logic.

---

## 4 PRs theo thứ tự

---

### PR 1 — `mtmd/mtmd-helper-video.cpp` (file mới)

Đây là core, làm trước hết.

```cpp
// mtmd/mtmd-helper-video.cpp

#include "mtmd-helper-video.h"
#include <cmath>
#include <vector>
#include <string>

// Video frame với timestamp
struct mtmd_video_frame {
    std::vector<uint8_t> rgb_data; // raw RGB24
    int width;
    int height;
    float timestamp_sec;
    int frame_index;
};

struct mtmd_video_t {
    std::vector<mtmd_video_frame> frames;
    float duration_sec;
    int source_fps;
};

// =============================================
// Internal: ffmpeg subprocess decode
// =============================================
static bool decode_frames_ffmpeg(
    const char* video_path,
    std::vector<mtmd_video_frame>& out_frames,
    float& out_duration,
    int max_frames,
    float scene_threshold
) {
    // Step 1: get duration via ffprobe
    char probe_cmd[1024];
    snprintf(probe_cmd, sizeof(probe_cmd),
        "ffprobe -v error -show_entries format=duration "
        "-of default=noprint_wrappers=1:nokey=1 \"%s\"",
        video_path);
    
    FILE* fp = _popen(probe_cmd, "r"); // Windows: _popen
    if (!fp) return false;
    fscanf(fp, "%f", &out_duration);
    _pclose(fp);
    
    if (out_duration <= 0) out_duration = 15.0f; // fallback
    
    // Step 2: scene change detection
    // ffmpeg -i video.mp4 -vf "select=gt(scene\,0.3),showinfo" 
    //        -vsync vfr -frames:v MAX scene_%03d.png
    std::string tmp_dir = std::string(video_path) + "_frames_tmp";
    // ... mkdir tmp_dir ...
    
    char scene_cmd[2048];
    snprintf(scene_cmd, sizeof(scene_cmd),
        "ffmpeg -i \"%s\" -y "
        "-vf \"select='gt(scene,%.2f)',setpts=N/FRAME_RATE/TB\" "
        "-frames:v %d -vsync vfr "
        "\"%s\\scene_%%03d.png\" 2>NUL",
        video_path, scene_threshold, max_frames, tmp_dir.c_str());
    system(scene_cmd);
    
    // Step 3: collect scene frames, count
    // ... glob tmp_dir/scene_*.png ...
    int scene_count = /* count scene frames */0;
    
    // Step 4: uniform fill nếu scene_count < max_frames
    int remaining = max_frames - scene_count;
    if (remaining > 0) {
        float target_fps = remaining / out_duration;
        char uniform_cmd[2048];
        snprintf(uniform_cmd, sizeof(uniform_cmd),
            "ffmpeg -i \"%s\" -y "
            "-vf \"fps=%.4f\" -frames:v %d "
            "\"%s\\uniform_%%03d.png\" 2>NUL",
            video_path, target_fps, remaining, tmp_dir.c_str());
        system(uniform_cmd);
    }
    
    // Step 5: load all frames, assign timestamps, sort
    // timestamp = frame_number / source_fps OR
    // timestamp = index * (duration / total_frames) untuk uniform
    // ...
    
    return !out_frames.empty();
}

// =============================================
// Public API
// =============================================
mtmd_video_t* mtmd_video_load(
    const char* video_path,
    int max_frames,        // default 15
    float scene_threshold  // default 0.3
) {
    auto* v = new mtmd_video_t();
    
    bool ok = decode_frames_ffmpeg(
        video_path, v->frames, v->duration_sec,
        max_frames, scene_threshold
    );
    
    if (!ok || v->frames.empty()) {
        delete v;
        return nullptr;
    }
    
    // Assign frame indices
    for (int i = 0; i < (int)v->frames.size(); i++) {
        v->frames[i].frame_index = i;
    }
    
    return v;
}

void mtmd_video_free(mtmd_video_t* v) {
    delete v;
}
```

---

### PR 2 — `clip.cpp` — temporal mRoPE

Tìm `struct clip_image_u8`, thêm 2 fields:

```cpp
// clip.cpp - struct clip_image_u8
struct clip_image_u8 {
    int nx, ny;
    
    // ADD THESE:
    int    nz            = 0;    // frame index (0 = static image)
    float  timestamp_sec = 0.0f; // video timestamp
    
    std::vector<uint8_t> buf;
};
```

Tìm hàm build position IDs cho Qwen2.5-VL. Tên thường là `clip_image_build_graph` hoặc trong `llm_build_qwen2vl`. Sửa chỗ tính `mrope_pos`:

```cpp
// Trước: temporal luôn = 0
int temporal_id = 0;

// Sau:
static constexpr float QWEN_TOKENS_PER_SECOND = 2.0f;

int temporal_id = (img->nz > 0)
    ? (int)roundf(img->timestamp_sec * QWEN_TOKENS_PER_SECOND)
    : 0;

// Wire vào position IDs:
// mrope_pos[0] = temporal_id  (thay vì 0)
// mrope_pos[1] = height_pos
// mrope_pos[2] = width_pos
```

Chỗ feed frames từ video vào clip encoder:

```cpp
// Trong mtmd hoặc llava.cpp, khi encode video frames:
for (int i = 0; i < n_frames; i++) {
    clip_image_u8* img = load_frame(video->frames[i]);
    img->nz            = i + 1;
    img->timestamp_sec = video->frames[i].timestamp_sec;
    clip_image_encode(ctx_clip, n_threads, img, vec);
}
```

---

### PR 3 — `server.cpp` — nhận `video_url`

Tìm chỗ parse `image_url` trong content array, thêm `video_url` handler ngay cạnh:

```cpp
// server.cpp - parse_message_content() hoặc tương đương
if (content_type == "image_url") {
    // existing image handling
    auto url = item["image_url"]["url"].get<std::string>();
    slot.images.push_back(load_image_from_url(url));
}
else if (content_type == "video_url") {  // ADD THIS BLOCK
    auto url = item["video_url"]["url"].get<std::string>();
    
    // Download nếu HTTP, hoặc dùng local path
    std::string local_path = resolve_url_to_local(url);
    
    // Load video với adaptive sampling
    int max_frames = item.value("max_frames", 15);
    float scene_threshold = item.value("scene_threshold", 0.3f);
    
    auto* video = mtmd_video_load(
        local_path.c_str(), max_frames, scene_threshold
    );
    
    if (video) {
        for (auto& frame : video->frames) {
            auto img = frame_to_clip_image(frame);
            slot.images.push_back(img);
        }
        mtmd_video_free(video);
    }
}
```

---

### PR 4 — Jinja template `<|video_pad|>`

Đây là PR dễ nhất, không build C++. Sửa file `.jinja` mày đang dùng:

```jinja
{%- for content in message['content'] %}
  {%- if content['type'] == 'image_url' %}
    <|vision_start|><|image_pad|><|vision_end|>
  {%- elif content['type'] == 'video_url' %}
    <|vision_start|><|video_pad|><|vision_end|>
  {%- endif %}
{%- endfor %}
```

---

### Python side sau khi xong — `llm.py` thay đổi tối thiểu

```python
# llm.py - _send_chat_completion()
# Thay toàn bộ image handling:

# TRƯỚC (xóa):
if p_imgs:
    for img in p_imgs:
        user_content.append({
            "type": "image_url", 
            "image_url": {"url": str(img)}
        })

# SAU:
if video_path:  # truyền video path trực tiếp
    user_content.append({
        "type": "video_url",
        "video_url": {"url": video_url},
        "max_frames": 15,
        "scene_threshold": 0.3
    })
```

`pipeline_extraction.py` — xóa toàn bộ block video frame extraction (lines 207–257). `pipeline_grading.py` — xóa image server, frame selection logic.

---

## Build sequence thực tế

```
Week 1: PR 1 skeleton + ffmpeg subprocess (test độc lập, không cần model)
        → Verify frames extracted đúng với timestamps

Week 2: PR 2 clip.cpp temporal mRoPE
        → Build MSVC+CUDA, test với static images trước (nz=0 path)
        → Sau đó test video path

Week 3: PR 3 server.cpp video_url
        → Test end-to-end với Python

Week 4: PR 4 Jinja + cleanup Python pipeline
        → Xóa workaround code
```

---

## Một điều cần verify trước khi bắt đầu

```bash
grep -rn "nz\|timestamp_sec\|video_pad\|mtmd_video" \
  examples/llava/clip.cpp \
  examples/llava/mtmd* \
  examples/server/server.cpp
```

Nếu upstream đã có partial implementation từ tháng 2-3/2026, mày chỉ cần wire lên thay vì viết từ đầu. Check trước để không làm trùng.

Build from Source with CUDA (Recommended for NVIDIA GPUs)

Building from source gives you the latest model support and optimal GPU-specific code.

```powershell
# Prerequisites:
#   - Visual Studio 2022 with "Desktop development with C++" workload
#   - CUDA Toolkit (https://developer.nvidia.com/cuda-downloads)
#   - CMake 3.21+ (https://cmake.org/download/)
#   - Git

# 1. Clone llama.cpp
git clone https://github.com/ggml-org/llama.cpp
cd llama.cpp

# 2. Create build directory
mkdir build-cuda
cd build-cuda

# 3. Configure with CUDA
cmake .. -DGGML_CUDA=ON -DGGML_NATIVE=ON -DGGML_CUDA_FA=ON -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON

# 4. Build (use your core count, e.g., 16)
cmake --build . --config Release -j 16

# 5. Binaries are in: build-cuda/bin/Release/ (Windows)
```

**Key CMake options:**
| Option | Description |
|--------|-------------|
| `-DGGML_CUDA=ON` | Enable CUDA GPU acceleration |
| `-DGGML_NATIVE=ON` | Optimize for your specific CPU (AVX2, etc.) |
| `-DGGML_CUDA_FA=ON` | Enable Flash Attention for CUDA (faster inference) |
| `-DBUILD_SHARED_LIBS=ON` | Build as DLLs (required for server) |
| `-DCMAKE_CUDA_ARCHITECTURES=89` | Target specific GPU (89=RTX 4090, 86=RTX 3090) |

### Fix: "Unsupported CUDA compiler version" Error

If your CUDA toolkit version is newer than what your Visual Studio version officially supports,
you'll get an error like:

```
nvcc fatal : unsupported GPU architecture 'compute_XX'
# or
Host compiler XX is not supported by CUDA toolkit
```

**Workaround:** Add `-allow-unsupported-compiler` to the CUDA flags:

```powershell
cmake .. -DGGML_CUDA=ON -DGGML_NATIVE=ON -DGGML_CUDA_FA=ON ^
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON ^
  -DCMAKE_CUDA_FLAGS="-allow-unsupported-compiler"
```

This flag tells nvcc to proceed even if the host compiler (MSVC) version isn't in CUDA's
official compatibility list. This is safe for llama.cpp builds and is commonly needed when:
- CUDA 13.x is installed with VS 2022 v17.12+
- CUDA 12.x is installed with a newer MSVC toolchain
- You compiled CUDA toolkit from a newer channel than your VS version

### Must read: 
- thư mục công cụ: 
    d:\Microsoft Visual Studio\
    d:\msys64\
- proj đã apply patch này từ repo gốc để support step3VL: step3vl_llama_cpp.patch
- thư mục chứa gguf models: e:\llm\, gợi ý test: qwen3.5-VL-9b-opus46d-abl.Q6_K.gguf với mmproj tương ứng: qwen3.5-VL-9b-mmproj.BF16.gguf
- video để test: d:\quang_dev\automarking\courses\genai_2025\outputs\012008010426\KiềuNgọcAnh_4289055_assignsubmission_file\20260311-100632\work\AI&App_KieuNgocAnh\presAI_KieuNgocAnh.mp4

### BUild proj:
- config template:
```bash
@echo off
echo ============================================
echo  llama.cpp CUDA Configure - RTX 4090 Laptop
echo ============================================

:: ── Visual Studio toolchain ──────────────────
call "D:\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44
if errorlevel 1 (
    echo [WARN] vcvars64 with version flag failed, retrying without...
    call "D:\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 (
        echo [ERROR] Could not initialise MSVC environment. Aborting.
        pause
        exit /b 1
    )
)

:: ── CUDA 13.0 paths ───────────────────────────
set CudaToolkitDir=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;%PATH%

:: ── Ninja (from msys64) ───────────────────────
set PATH=D:\msys64\usr\bin;%PATH%

:: ── Verify tools ─────────────────────────────
where ninja >nul 2>&1
if errorlevel 1 (
    echo [ERROR] ninja.exe not found. Install via: winget install Ninja-build.Ninja
    echo         or check D:\msys64\usr\bin\ninja.exe exists
    pause
    exit /b 1
)
where nvcc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] nvcc not found. Check CUDA install path.
    pause
    exit /b 1
)
echo [OK] MSVC, CUDA, Ninja all found.

:: ── Create and enter build dir ────────────────
if not exist "D:\Apps\git_cloned_repos\llama.cpp\build-cuda" (
    mkdir "D:\Apps\git_cloned_repos\llama.cpp\build-cuda"
)
cd /d "D:\Apps\git_cloned_repos\llama.cpp\build-cuda"

:: ── CMake configure ───────────────────────────
:: sm_89 = RTX 4090 (Ada Lovelace) - correct for this machine
"D:\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" .. ^
    -G "Ninja" ^
    -DGGML_CUDA=ON ^
    -DGGML_NATIVE=ON ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DBUILD_SHARED_LIBS=ON ^
    -DCMAKE_CUDA_ARCHITECTURES=89 ^
    "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler" ^
    -DGGML_CUDA_FA_ALL_QUANTS=ON ^
    -DGGML_CUDA_FORCE_MMQ=OFF

if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    pause
    exit /b 1
)

echo.
echo [DONE] Configure successful. Run llama_build.bat to compile.
pause
```
- build template:
```bash
@echo off
echo ============================================
echo  llama.cpp CUDA Build - RTX 4090 Laptop
echo ============================================

:: ── Visual Studio toolchain ──────────────────
call "D:\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44
if errorlevel 1 (
    echo [WARN] vcvars64 with version flag failed, retrying without...
    call "D:\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 (
        echo [ERROR] Could not initialise MSVC environment. Aborting.
        pause
        exit /b 1
    )
)

:: ── CUDA 13.0 paths ───────────────────────────
set CudaToolkitDir=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;%PATH%

:: ── Ninja (from msys64) ───────────────────────
set PATH=D:\msys64\usr\bin;%PATH%

:: ── Enter build dir ───────────────────────────
if not exist "D:\Apps\git_cloned_repos\llama.cpp\build-cuda" (
    echo [ERROR] Build directory not found. Run llama_configure.bat first.
    pause
    exit /b 1
)
cd /d "D:\Apps\git_cloned_repos\llama.cpp\build-cuda"

:: ── Build ─────────────────────────────────────
:: -j 16 works correctly with Ninja (uses all cores)
echo [INFO] Building with 16 parallel jobs...
"D:\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
    --build . --config Release -j 16

if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo [DONE] Build complete!
echo Binaries are in: D:\Apps\git_cloned_repos\llama.cpp\build-cuda\bin\
pause
```