OK, nói về llama.cpp general video pipeline.

500MB là normal với video production content. Vấn đề core cần giải quyết:

---

## 1. Memory — không load toàn bộ vào RAM

Design pattern phải là **lazy/streaming**:

```cpp
// SAI — load toàn bộ
std::vector<Frame> all_frames = decode_entire_video(path); // 500MB vào RAM

// ĐÚNG — decode on demand
for (int i = 0; i < n_frames; i++) {
    Frame f = decode_single_frame(path, target_timestamp[i]);
    encode_to_visual_tokens(f);
    free(f); // discard ngay sau khi encode
}
```

Đây chính xác là pattern `mtmd_tokenize_lazy` trong issue #18389 mà ngxson đang design. Mỗi frame decode → encode → free, không accumulate.

---

## 2. Frame selection strategy — phải tính trước timestamps

Trước khi decode bất kỳ frame nào, cần biết **sẽ lấy frame ở giây nào**. Tách thành 2 pass:

```cpp
// Pass 1: probe only — cực nhanh, không decode video
VideoInfo info = probe_video(path);
// → duration, fps, keyframe positions, file size

// Pass 2: tính target timestamps dựa trên info
std::vector<float> target_ts = select_timestamps(info, max_frames);

// Pass 3: decode chỉ những frames cần thiết
for (float ts : target_ts) {
    Frame f = seek_and_decode(path, ts);
    process(f);
    free(f);
}
```

---

## 3. Timestamp selection — adaptive theo content

Đây là phần thú vị. 3 strategies cần support:

```cpp
enum SamplingStrategy {
    UNIFORM,      // đều nhau, đơn giản
    KEYFRAME,     // I-frames only, nhanh với file lớn  
    SCENE_CHANGE, // content-aware, chậm nhưng tốt nhất
};

SamplingStrategy choose_strategy(VideoInfo info) {
    float size_mb = info.file_size / 1024.0f / 1024.0f;
    
    if (size_mb > 500)        return KEYFRAME;      // quá lớn
    if (info.duration > 300)  return KEYFRAME;      // >5 phút
    return SCENE_CHANGE;                             // normal case
}
```

**KEYFRAME** nhanh vì ffmpeg chỉ decode I-frames, skip B/P frames — seek trực tiếp, không cần decode toàn bộ stream.

---

## 4. Context window là hard constraint

Đây là ceiling thực sự, không phải file size:

```cpp
int calculate_max_frames(VideoInfo info, int ctx_size) {
    // Estimate tokens available cho visual
    int text_tokens_reserved = 4096;  // prompt + output
    int visual_budget = ctx_size - text_tokens_reserved;
    
    // Qwen3.5: ~256 tokens/frame (dynamic patching, average)
    int tokens_per_frame = 256;
    int max_by_ctx = visual_budget / tokens_per_frame;
    
    // Scale by duration nhưng không vượt ctx limit
    int wanted;
    if      (info.duration <= 30)  wanted = 15;
    else if (info.duration <= 120) wanted = 20;
    else if (info.duration <= 300) wanted = 24;
    else                           wanted = 30;
    
    return std::min(wanted, max_by_ctx);
}
```

---

## 5. Seek accuracy vs speed tradeoff

Với file 500MB, seek đến exact timestamp tốn kém vì phải decode từ nearest keyframe:

```cpp
// Exact seek — chính xác nhưng chậm
// ffmpeg -ss {ts} -i video.mp4 (sau -i = slow seek)

// Fast seek — đến nearest keyframe, có thể lệch vài giây
// ffmpeg -ss {ts} -i video.mp4 (trước -i = fast seek)
```

Với video dài, fast seek acceptable vì timestamp lệch 1-2s không quan trọng. Với video ngắn (15s), cần exact seek.

```cpp
SeekMode choose_seek_mode(VideoInfo info) {
    return (info.duration < 60) ? EXACT_SEEK : FAST_SEEK;
}
```

---

## Minimal viable API cho llama.cpp

```cpp
// Public API mà server.cpp gọi
struct mtmd_video_params {
    int   max_frames;        // -1 = auto từ ctx size
    float scene_threshold;   // 0.3 default
    SamplingStrategy strategy; // AUTO = tự chọn theo file
};

mtmd_video_t* mtmd_video_load(
    const char*              path,
    const mtmd_video_params* params  // NULL = all defaults
);

// Iterator pattern — không hold tất cả frames trong memory
bool mtmd_video_next_frame(mtmd_video_t* v, mtmd_video_frame* out_frame);
void mtmd_video_free(mtmd_video_t* v);
```

Iterator pattern quan trọng — caller encode từng frame rồi free, không bao giờ có nhiều hơn 1 frame decoded trong RAM cùng lúc.

---

## Dependency: ffmpeg

Hai options:

**libavcodec link tĩnh** — professional nhưng phức tạp với MSVC, license issues (LGPL)

**ffmpeg subprocess** — đơn giản hơn, không cần link, mày đã quen với approach này. Tradeoff: spawn process overhead, nhưng với video chỉ xảy ra 1 lần per request nên chấp nhận được.

Upstream #18389 recommend `dlopen(libavcodec)` với ffmpeg subprocess fallback — flexible nhất.