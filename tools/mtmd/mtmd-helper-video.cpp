#include "mtmd-helper-video.h"
#include "mtmd.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#endif

// stb_image is already implemented in mtmd-helper.cpp, just include the header
#include "stb/stb_image.h"

// ============================================================================
// Internal helpers — Windows UTF-8 subprocess support
// ============================================================================

namespace fs = std::filesystem;

#if defined(_WIN32)
static std::wstring utf8_to_wide(const std::string & utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wstr[0], len);
    return wstr;
}

static FILE * utf8_popen(const std::string & cmd) {
    std::wstring wcmd = utf8_to_wide(cmd);
    return _wpopen(wcmd.c_str(), L"r");
}

static int utf8_pclose(FILE * fp) {
    return _pclose(fp);
}

static int utf8_system(const std::string & cmd) {
    std::wstring wcmd = utf8_to_wide(cmd);
    return _wsystem(wcmd.c_str());
}

static fs::path utf8_path(const std::string & p) {
    return fs::path(utf8_to_wide(p));
}
#else
static FILE * utf8_popen(const std::string & cmd) {
    return popen(cmd.c_str(), "r");
}
static int utf8_pclose(FILE * fp) {
    return pclose(fp);
}
static int utf8_system(const std::string & cmd) {
    return system(cmd.c_str());
}
static fs::path utf8_path(const std::string & p) {
    return fs::path(p);
}
#endif

// Use system temp directory with a hash to avoid Unicode issues in path
static std::string create_temp_dir(const char * video_path) {
    std::hash<std::string> hasher;
    size_t h = hasher(std::string(video_path));
    fs::path tmp = fs::temp_directory_path() / ("mtmd_frames_" + std::to_string(h));
    std::error_code ec;
    fs::create_directories(tmp, ec);
    return tmp.string();
}

static void cleanup_temp_dir(const std::string & dir) {
    std::error_code ec;
    fs::remove_all(utf8_path(dir), ec);
}

// ============================================================================
// Video probing — lightweight metadata extraction
// ============================================================================

struct video_probe_info {
    float  duration      = -1.0f; // seconds
    float  fps           = -1.0f; // source fps
    int    width         = 0;
    int    height        = 0;
    int    n_keyframes   = -1;    // -1 = unknown
    int64_t file_size_mb = 0;
};

// Probe video metadata in a single ffprobe call (duration, resolution, fps)
static video_probe_info probe_video(const char * video_path) {
    video_probe_info info;

    // Get file size
    {
        std::error_code ec;
        auto fsize = fs::file_size(utf8_path(video_path), ec);
        if (!ec) {
            info.file_size_mb = (int64_t)(fsize / (1024 * 1024));
        }
    }

    // Single ffprobe call for duration + resolution + fps
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "ffprobe -v error -select_streams v:0 "
            "-show_entries format=duration "
            "-show_entries stream=width,height,r_frame_rate,duration "
            "-of csv=p=0:nk=1 \"%s\"",
            video_path);

        FILE * fp = utf8_popen(cmd);
        if (fp) {
            char buf[512] = {};
            // Read all lines — format outputs duration on one line, stream on another
            std::string all_output;
            while (fgets(buf, sizeof(buf), fp)) {
                all_output += buf;
            }
            utf8_pclose(fp);

            // Parse output: typically 2 lines
            //   Line 1 (stream): width,height,fps_num/fps_den,stream_duration
            //   Line 2 (format): format_duration
            std::istringstream iss(all_output);
            std::string line;
            while (std::getline(iss, line)) {
                // Try parsing as stream line: w,h,num/den,dur
                int w = 0, h = 0, fn = 0, fd = 1;
                float sdur = -1.0f;
                if (sscanf(line.c_str(), "%d,%d,%d/%d,%f", &w, &h, &fn, &fd, &sdur) >= 4 && w > 0) {
                    info.width  = w;
                    info.height = h;
                    if (fd > 0) info.fps = (float)fn / (float)fd;
                    if (sdur > 0.0f && info.duration <= 0.0f) info.duration = sdur;
                }
                // Try parsing as format duration line (single float)
                else {
                    float fdur = -1.0f;
                    if (sscanf(line.c_str(), "%f", &fdur) == 1 && fdur > 0.0f) {
                        info.duration = fdur; // prefer format-level duration
                    }
                }
            }
        }
    }

    // Fallback: try simpler duration extraction methods if above failed
    if (info.duration <= 0.0f) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "ffprobe -v error -show_entries format=duration "
            "-of default=noprint_wrappers=1:nokey=1 \"%s\"",
            video_path);
        FILE * fp = utf8_popen(cmd);
        if (fp) {
            float dur = -1.0f;
            if (fscanf(fp, "%f", &dur) == 1 && dur > 0.0f) {
                info.duration = dur;
            }
            utf8_pclose(fp);
        }
    }

    return info;
}

// Collect sorted file paths with given prefix
static std::vector<std::string> collect_files_sorted(const std::string & dir, const std::string & prefix) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            std::string fname = entry.path().filename().string();
            if (fname.find(prefix) == 0) {
                files.push_back(entry.path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Validate video path for shell safety
// Paths are always double-quoted in ffmpeg/ffprobe commands, so '&' is safe.
// Block characters that are dangerous even inside double quotes.
static bool validate_video_path(const char * video_path) {
    if (!video_path) return false;
    std::string path(video_path);
    for (char c : path) {
        if (c == ';' || c == '|' || c == '`' || c == '$' || c == '\n' || c == '\r') {
            fprintf(stderr, "[mtmd-video] ERROR: video path contains unsafe characters\n");
            return false;
        }
    }
    return true;
}

// ============================================================================
// Frame count calculation (matching Qwen2.5-VL official implementation)
// ============================================================================

static constexpr float  QWEN_DEFAULT_FPS = 2.0f;   // FPS = 2.0
static constexpr int    FRAME_FACTOR     = 2;       // nframes must be divisible by 2
static constexpr int    FPS_MIN_FRAMES   = 4;       // minimum frames
static constexpr int    FPS_MAX_FRAMES   = 768;     // maximum frames (official)
static constexpr int    GGUF_MAX_FRAMES  = 120;     // practical cap for GGUF models (context window)

// Maximum output resolution — frames are scaled down if source exceeds this.
// VLMs internally resize to their patch size (e.g. 384x384 per tile), so
// saving frames at >1280px is pure waste of disk I/O and memory.
static constexpr int    MAX_FRAME_DIM    = 1280;

static int floor_to_factor(int n, int factor) {
    return (n / factor) * factor;
}

// Calculate optimal frame count for video
static int calc_nframes(float fps, int max_frames, float duration) {
    if (fps <= 0.0f) fps = QWEN_DEFAULT_FPS;
    if (max_frames <= 0) max_frames = GGUF_MAX_FRAMES;
    if (max_frames > GGUF_MAX_FRAMES) max_frames = GGUF_MAX_FRAMES;

    int nframes = (int)(duration * fps);
    int effective_max = std::min(max_frames, FPS_MAX_FRAMES);
    nframes = std::max(nframes, FPS_MIN_FRAMES);
    nframes = std::min(nframes, effective_max);
    nframes = floor_to_factor(nframes, FRAME_FACTOR);
    if (nframes < FRAME_FACTOR) nframes = FRAME_FACTOR;
    return nframes;
}

// ============================================================================
// Extraction strategy selection — adaptive based on video properties
// ============================================================================

enum extraction_strategy {
    EXTRACT_UNIFORM,    // Single ffmpeg pass with fps filter (default)
    EXTRACT_FAST_SEEK,  // Per-frame fast seek — better for long/large videos
    EXTRACT_KEYFRAME,   // I-frames only — fastest for very large files
};

static extraction_strategy choose_strategy(const video_probe_info & info, int nframes) {
    // For very large files or very long videos, use keyframe extraction
    if (info.file_size_mb > 2000 || info.duration > 1800.0f) {
        return EXTRACT_KEYFRAME;
    }
    // For large files or long videos, use fast seek (per-frame seeking)
    // This avoids decoding the entire stream — only seeks to nearest keyframe
    if (info.file_size_mb > 200 || info.duration > 300.0f) {
        return EXTRACT_FAST_SEEK;
    }
    return EXTRACT_UNIFORM;
}

// Build the video filter string: resolution scaling + fps/frame selection
// Output JPEG (q:v 2) instead of PNG — 10-15× smaller files, ample quality for VLMs
static std::string build_vf_scale(const video_probe_info & info) {
    if (info.width > MAX_FRAME_DIM || info.height > MAX_FRAME_DIM) {
        // Scale down preserving aspect ratio: constrain the larger dimension
        return "scale='min(" + std::to_string(MAX_FRAME_DIM) + ",iw)':'min("
            + std::to_string(MAX_FRAME_DIM) + ",ih)':force_original_aspect_ratio=decrease";
    }
    return "";
}

// ============================================================================
// Frame extraction — three strategies
// ============================================================================

// Strategy 1: Single-pass uniform extraction (best for short/small videos)
static bool extract_uniform(
    const char * video_path,
    const std::string & tmp_dir,
    int nframes,
    float duration,
    const video_probe_info & info,
    std::vector<std::string> & out_files)
{
    float target_fps = (float)nframes / duration;
    if (target_fps > 30.0f) target_fps = 30.0f;
    if (target_fps < 0.1f)  target_fps = 0.1f;

    std::string scale = build_vf_scale(info);
    std::string vf = "fps=" + std::to_string(target_fps);
    if (!scale.empty()) vf += "," + scale;

    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -i \"%s\" -y "
        "-vf \"%s\" -frames:v %d -q:v 2 "
        "\"%s\\frame_%%04d.jpg\" 2>NUL",
        video_path, vf.c_str(), nframes, tmp_dir.c_str());
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -i \"%s\" -y "
        "-vf \"%s\" -frames:v %d -q:v 2 "
        "\"%s/frame_%%04d.jpg\" 2>/dev/null",
        video_path, vf.c_str(), nframes, tmp_dir.c_str());
#endif

    int ret = utf8_system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[mtmd-video] WARN: ffmpeg uniform extraction returned %d\n", ret);
    }

    out_files = collect_files_sorted(tmp_dir, "frame_");
    return !out_files.empty();
}

// Strategy 2: Per-frame fast seek (best for long/large videos with sparse sampling)
// Uses -ss BEFORE -i for fast seek to nearest keyframe, then decodes one frame
static bool extract_fast_seek(
    const char * video_path,
    const std::string & tmp_dir,
    int nframes,
    float duration,
    const video_probe_info & info,
    std::vector<std::string> & out_files)
{
    std::string scale = build_vf_scale(info);
    std::string vf = scale.empty() ? "" : ("-vf \"" + scale + "\" ");

    // Calculate uniform timestamps
    float step = duration / (float)nframes;

    for (int i = 0; i < nframes; i++) {
        float ts = step * (i + 0.5f); // midpoint of interval

        char cmd[4096];
#if defined(_WIN32)
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -ss %.4f -i \"%s\" -y "
            "%s-frames:v 1 -q:v 2 "
            "\"%s\\frame_%04d.jpg\" 2>NUL",
            ts, video_path, vf.c_str(), tmp_dir.c_str(), i + 1);
#else
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -ss %.4f -i \"%s\" -y "
            "%s-frames:v 1 -q:v 2 "
            "\"%s/frame_%04d.jpg\" 2>/dev/null",
            ts, video_path, vf.c_str(), tmp_dir.c_str(), i + 1);
#endif

        int ret = utf8_system(cmd);
        if (ret != 0) {
            fprintf(stderr, "[mtmd-video] WARN: fast seek frame %d (ts=%.2f) returned %d\n", i, ts, ret);
        }
    }

    out_files = collect_files_sorted(tmp_dir, "frame_");
    return !out_files.empty();
}

// Strategy 3: Keyframe extraction (fastest for very large files)
// Only decodes I-frames, no B/P frame decoding needed
static bool extract_keyframes(
    const char * video_path,
    const std::string & tmp_dir,
    int nframes,
    const video_probe_info & info,
    std::vector<std::string> & out_files)
{
    std::string scale = build_vf_scale(info);
    // Extract all keyframes first, then we'll subsample
    // -skip_frame nokey tells the demuxer to only deliver keyframes
    std::string vf_parts;
    if (!scale.empty()) vf_parts = scale;

    char cmd[4096];
    // Extract up to 4× nframes keyframes, then subsample below
    int key_limit = nframes * 4;
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -skip_frame nokey -i \"%s\" -y "
        "-vsync vfr %s%s-frames:v %d -q:v 2 "
        "\"%s\\key_%%04d.jpg\" 2>NUL",
        video_path,
        vf_parts.empty() ? "" : "-vf \"",
        vf_parts.empty() ? "" : (vf_parts + "\" ").c_str(),
        key_limit, tmp_dir.c_str());
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -skip_frame nokey -i \"%s\" -y "
        "-vsync vfr %s%s-frames:v %d -q:v 2 "
        "\"%s/key_%%04d.jpg\" 2>/dev/null",
        video_path,
        vf_parts.empty() ? "" : "-vf \"",
        vf_parts.empty() ? "" : (vf_parts + "\" ").c_str(),
        key_limit, tmp_dir.c_str());
#endif

    int ret = utf8_system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[mtmd-video] WARN: keyframe extraction returned %d\n", ret);
    }

    auto all_keys = collect_files_sorted(tmp_dir, "key_");
    if (all_keys.empty()) return false;

    // Uniformly subsample keyframes down to nframes
    if ((int)all_keys.size() <= nframes) {
        out_files = all_keys;
    } else {
        float step = (float)all_keys.size() / (float)nframes;
        for (int i = 0; i < nframes; i++) {
            int idx = (int)(step * i);
            if (idx >= (int)all_keys.size()) idx = (int)all_keys.size() - 1;
            out_files.push_back(all_keys[idx]);
        }
    }
    return !out_files.empty();
}

// ============================================================================
// Public API
// ============================================================================

struct mtmd_video * mtmd_video_load(
    const char * video_path,
    int          max_frames,
    float        fps,
    float        scene_threshold)
{
    (void)scene_threshold; // reserved for future scene-based extraction

    if (!validate_video_path(video_path)) {
        return nullptr;
    }

    if (!fs::exists(utf8_path(video_path))) {
        fprintf(stderr, "[mtmd-video] ERROR: video file not found: %s\n", video_path);
        return nullptr;
    }

    // Single probe call for all metadata (duration, resolution, fps, file size)
    video_probe_info info = probe_video(video_path);
    if (info.duration <= 0.0f) {
        fprintf(stderr, "[mtmd-video] ERROR: could not determine video duration. Is ffprobe working?\n");
        return nullptr;
    }

    int nframes = calc_nframes(fps, max_frames, info.duration);
    extraction_strategy strategy = choose_strategy(info, nframes);
    float actual_fps = (float)nframes / info.duration;

    const char * strategy_names[] = { "uniform", "fast-seek", "keyframe" };
    fprintf(stderr, "[mtmd-video] loading: %s\n"
                    "  duration=%.1fs  resolution=%dx%d  size=%lldMB\n"
                    "  nframes=%d  fps=%.2f  strategy=%s  output=JPEG\n",
            video_path,
            info.duration, info.width, info.height, (long long)info.file_size_mb,
            nframes, actual_fps, strategy_names[strategy]);

    std::string tmp_dir = create_temp_dir(video_path);

    std::vector<std::string> frame_files;
    bool ok = false;

    switch (strategy) {
        case EXTRACT_UNIFORM:
            ok = extract_uniform(video_path, tmp_dir, nframes, info.duration, info, frame_files);
            break;
        case EXTRACT_FAST_SEEK:
            ok = extract_fast_seek(video_path, tmp_dir, nframes, info.duration, info, frame_files);
            break;
        case EXTRACT_KEYFRAME:
            ok = extract_keyframes(video_path, tmp_dir, nframes, info, frame_files);
            // Fallback to fast seek if keyframe extraction got too few frames
            if (ok && (int)frame_files.size() < nframes / 2) {
                fprintf(stderr, "[mtmd-video] keyframe extraction got only %d frames, falling back to fast-seek\n",
                        (int)frame_files.size());
                frame_files.clear();
                ok = extract_fast_seek(video_path, tmp_dir, nframes, info.duration, info, frame_files);
            }
            break;
    }

    if (!ok || frame_files.empty()) {
        fprintf(stderr, "[mtmd-video] ERROR: failed to extract any frames\n");
        cleanup_temp_dir(tmp_dir);
        return nullptr;
    }

    int n_frames = (int)frame_files.size();
    float time_step = info.duration / (float)n_frames;

    fprintf(stderr, "[mtmd-video] extracted %d frames (time_step=%.2fs)\n", n_frames, time_step);

    // Build output
    auto * video = new mtmd_video();
    video->n_frames     = n_frames;
    video->duration_sec = info.duration;
    video->sample_fps   = (float)n_frames / info.duration;
    video->tmp_dir      = strdup(tmp_dir.c_str());
    video->frames       = (mtmd_video_frame *)calloc(n_frames, sizeof(mtmd_video_frame));

    for (int i = 0; i < n_frames; i++) {
        auto & f = video->frames[i];
        f.frame_index    = i;
        f.timestamp_sec  = time_step * (i + 0.5f);
        f.png_path       = strdup(frame_files[i].c_str());
        f.data           = nullptr;

        int nx = 0, ny = 0, nc = 0;
        if (stbi_info(frame_files[i].c_str(), &nx, &ny, &nc)) {
            f.nx = (uint32_t)nx;
            f.ny = (uint32_t)ny;
        }
    }

    return video;
}

void mtmd_video_free(struct mtmd_video * video) {
    if (!video) return;

    for (int i = 0; i < video->n_frames; i++) {
        free(video->frames[i].data);
        free(video->frames[i].png_path);
    }
    free(video->frames);

    // Cleanup temp directory
    if (video->tmp_dir) {
        cleanup_temp_dir(video->tmp_dir);
        free(video->tmp_dir);
    }

    delete video;
}

size_t mtmd_video_frame_read_png(const struct mtmd_video_frame * frame,
                                 unsigned char ** out_buf)
{
    if (!frame || !frame->png_path || !out_buf) {
        return 0;
    }

    // Note: despite the function name, this reads any image format (JPEG or PNG)
    // The png_path field stores the path regardless of format
    std::ifstream file(frame->png_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        fprintf(stderr, "[mtmd-video] ERROR: cannot open frame: %s\n", frame->png_path);
        return 0;
    }

    size_t size = (size_t)file.tellg();
    file.seekg(0, std::ios::beg);

    unsigned char * buf = (unsigned char *)malloc(size);
    if (!buf) {
        return 0;
    }

    file.read(reinterpret_cast<char *>(buf), size);
    if (!file) {
        free(buf);
        return 0;
    }

    *out_buf = buf;
    return size;
}
