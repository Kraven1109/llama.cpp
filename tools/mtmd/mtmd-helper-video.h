#ifndef MTMD_HELPER_VIDEO_H
#define MTMD_HELPER_VIDEO_H

#include "mtmd.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Video frame extracted from a video file
struct mtmd_video_frame {
    uint32_t nx;
    uint32_t ny;
    float    timestamp_sec;   // timestamp in seconds
    int      frame_index;     // 0-based frame index
    unsigned char * data;     // RGB data, size = nx * ny * 3
    char * png_path;          // path to PNG file on disk (for server loading)
};

// Video container holding extracted frames
struct mtmd_video {
    struct mtmd_video_frame * frames;
    int    n_frames;
    float  duration_sec;
    char * tmp_dir;           // temp directory with frame PNGs (cleaned up on free)
};

// Load video and extract frames using ffmpeg subprocess
// video_path: path to video file (local)
// max_frames: maximum number of frames to extract (default: 15)
// scene_threshold: scene change detection threshold 0.0-1.0 (default: 0.3)
// returns nullptr on failure
// NOTE: ffmpeg must be available in PATH
MTMD_API struct mtmd_video * mtmd_video_load(
    const char * video_path,
    int          max_frames,
    float        scene_threshold);

// Free video and all its frames, also cleans up temp directory
MTMD_API void mtmd_video_free(struct mtmd_video * video);

// Read a video frame's PNG file into a raw buffer suitable for mtmd_helper_bitmap_init_from_buf
// Returns the buffer size, or 0 on failure
// The caller must free the returned buffer with free()
MTMD_API size_t mtmd_video_frame_read_png(const struct mtmd_video_frame * frame,
                                          unsigned char ** out_buf);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MTMD_HELPER_VIDEO_H
