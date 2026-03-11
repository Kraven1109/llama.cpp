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
#define popen  _popen
#define pclose _pclose
#endif

// stb_image is already implemented in mtmd-helper.cpp, just include the header
#include "stb/stb_image.h"

// ============================================================================
// Internal helpers
// ============================================================================

namespace fs = std::filesystem;

static std::string create_temp_dir(const char * video_path) {
    fs::path vpath(video_path);
    fs::path tmp = vpath.parent_path() / (vpath.stem().string() + "_mtmd_frames");
    std::error_code ec;
    fs::create_directories(tmp, ec);
    return tmp.string();
}

static void cleanup_temp_dir(const std::string & dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Get video duration using ffprobe - tries multiple methods
static float get_video_duration(const char * video_path) {
    // Method 1: format duration (container-level)
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "ffprobe -v error -show_entries format=duration "
            "-of default=noprint_wrappers=1:nokey=1 \"%s\"",
            video_path);

        FILE * fp = popen(cmd, "r");
        if (fp) {
            float duration = -1.0f;
            if (fscanf(fp, "%f", &duration) == 1 && duration > 0.0f) {
                pclose(fp);
                return duration;
            }
            pclose(fp);
        }
    }

    // Method 2: stream duration (more reliable for some formats)
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "ffprobe -v error -select_streams v:0 "
            "-show_entries stream=duration "
            "-of default=noprint_wrappers=1:nokey=1 \"%s\"",
            video_path);

        FILE * fp = popen(cmd, "r");
        if (fp) {
            float duration = -1.0f;
            if (fscanf(fp, "%f", &duration) == 1 && duration > 0.0f) {
                pclose(fp);
                return duration;
            }
            pclose(fp);
        }
    }

    // Method 3: count frames and use frame rate
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "ffprobe -v error -select_streams v:0 "
            "-count_frames -show_entries stream=nb_read_frames,r_frame_rate "
            "-of csv=p=0 \"%s\"",
            video_path);

        FILE * fp = popen(cmd, "r");
        if (fp) {
            char buf[256] = {};
            if (fgets(buf, sizeof(buf), fp)) {
                int num = 0, den = 1;
                long n_frames = 0;
                // Parse "fps_num/fps_den,n_frames"
                if (sscanf(buf, "%d/%d,%ld", &num, &den, &n_frames) == 3 && den > 0 && n_frames > 0) {
                    pclose(fp);
                    return (float)n_frames * (float)den / (float)num;
                }
            }
            pclose(fp);
        }
    }

    fprintf(stderr, "[mtmd-video] ERROR: all duration detection methods failed\n");
    return -1.0f;  // caller should handle this
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
static bool validate_video_path(const char * video_path) {
    if (!video_path) return false;
    std::string path(video_path);
    for (char c : path) {
        if (c == ';' || c == '|' || c == '&' || c == '`' || c == '$' || c == '\n' || c == '\r') {
            fprintf(stderr, "[mtmd-video] ERROR: video path contains unsafe characters\n");
            return false;
        }
    }
    return true;
}

// ============================================================================
// Frame extraction
// ============================================================================

static bool extract_frames_uniform(
    const char * video_path,
    const std::string & tmp_dir,
    int max_frames,
    float duration,
    std::vector<std::string> & out_files)
{
    if (duration <= 0.0f) {
        fprintf(stderr, "[mtmd-video] ERROR: invalid duration for frame extraction\n");
        return false;
    }

    float target_fps = (float)max_frames / duration;
    if (target_fps > 30.0f) target_fps = 30.0f;
    if (target_fps < 0.1f)  target_fps = 0.1f;

    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -i \"%s\" -y "
        "-vf \"fps=%.4f\" -frames:v %d "
        "\"%s\\frame_%%04d.png\" 2>NUL",
        video_path, target_fps, max_frames, tmp_dir.c_str());
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -i \"%s\" -y "
        "-vf \"fps=%.4f\" -frames:v %d "
        "\"%s/frame_%%04d.png\" 2>/dev/null",
        video_path, target_fps, max_frames, tmp_dir.c_str());
#endif

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[mtmd-video] WARN: ffmpeg returned %d\n", ret);
    }

    out_files = collect_files_sorted(tmp_dir, "frame_");
    return !out_files.empty();
}

// ============================================================================
// Public API
// ============================================================================

struct mtmd_video * mtmd_video_load(
    const char * video_path,
    int          max_frames,
    float        scene_threshold)
{
    (void)scene_threshold; // reserved for future scene-based extraction

    if (!validate_video_path(video_path) || max_frames <= 0) {
        return nullptr;
    }

    if (!fs::exists(video_path)) {
        fprintf(stderr, "[mtmd-video] ERROR: video file not found: %s\n", video_path);
        return nullptr;
    }

    if (max_frames > 120) max_frames = 120;

    float duration = get_video_duration(video_path);
    if (duration <= 0.0f) {
        fprintf(stderr, "[mtmd-video] ERROR: could not determine video duration. Is ffprobe working?\n");
        return nullptr;
    }

    fprintf(stderr, "[mtmd-video] loading video: %s (duration=%.1fs, max_frames=%d)\n",
            video_path, duration, max_frames);

    std::string tmp_dir = create_temp_dir(video_path);

    std::vector<std::string> frame_files;
    bool ok = extract_frames_uniform(video_path, tmp_dir, max_frames, duration, frame_files);

    if (!ok || frame_files.empty()) {
        fprintf(stderr, "[mtmd-video] ERROR: failed to extract any frames\n");
        cleanup_temp_dir(tmp_dir);
        return nullptr;
    }

    int n_frames = (int)frame_files.size();
    float time_step = duration / (float)n_frames;

    fprintf(stderr, "[mtmd-video] extracted %d frames (time_step=%.2fs)\n", n_frames, time_step);

    // Build output
    auto * video = new mtmd_video();
    video->n_frames     = n_frames;
    video->duration_sec = duration;
    video->tmp_dir      = strdup(tmp_dir.c_str());
    video->frames       = (mtmd_video_frame *)calloc(n_frames, sizeof(mtmd_video_frame));

    for (int i = 0; i < n_frames; i++) {
        auto & f = video->frames[i];
        f.frame_index    = i;
        f.timestamp_sec  = time_step * (i + 0.5f); // midpoint of interval
        f.png_path       = strdup(frame_files[i].c_str());
        f.data           = nullptr; // loaded on demand via mtmd_video_frame_read_png

        // Get image dimensions
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

    std::ifstream file(frame->png_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        fprintf(stderr, "[mtmd-video] ERROR: cannot open frame PNG: %s\n", frame->png_path);
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
