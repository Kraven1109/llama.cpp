"""
Test script for llama.cpp video support with Qwen3.5-VL.

Tests:
1. Video frame extraction (mtmd_video_load via ffmpeg)
2. Server video_url API endpoint
3. End-to-end video inference with Qwen3.5-VL model

Requirements:
  - ffmpeg/ffprobe in PATH
  - llama-server built with video support
  - Qwen3.5-VL GGUF model + mmproj

Usage:
  uv run tests/test_video_support.py [--server-url URL] [--video-path PATH]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

# ============================================================================
# Configuration
# ============================================================================

DEFAULT_SERVER_URL = "http://127.0.0.1:8080"
DEFAULT_MODEL_PATH = r"E:\llm\qwen3.5-VL-9b-opus46d-abl.Q6_K.gguf"
DEFAULT_MMPROJ_PATH = r"E:\llm\qwen3.5-VL-9b-mmproj.BF16.gguf"
DEFAULT_VIDEO_PATH = r"D:\quang_dev\automarking\courses\genai_2025\outputs\012008010426\KiềuNgọcAnh_4289055_assignsubmission_file\20260311-100632\work\AI&App_KieuNgocAnh\presAI_KieuNgocAnh.mp4"
BUILD_DIR = r"D:\Apps\git_cloned_repos\llama.cpp\build-cuda\bin"


def log(msg: str, level: str = "INFO"):
    print(f"[{level}] {msg}", flush=True)


def log_pass(msg: str):
    print(f"  \u2705 PASS: {msg}", flush=True)


def log_fail(msg: str):
    print(f"  \u274c FAIL: {msg}", flush=True)


# ============================================================================
# Test 1: FFmpeg availability
# ============================================================================

def test_ffmpeg_available() -> bool:
    """Check that ffmpeg and ffprobe are available in PATH."""
    log("Test 1: Checking ffmpeg/ffprobe availability...")
    passed = True

    for tool in ["ffmpeg", "ffprobe"]:
        try:
            result = subprocess.run(
                [tool, "-version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                version_line = result.stdout.split("\n")[0]
                log_pass(f"{tool} found: {version_line}")
            else:
                log_fail(f"{tool} returned error code {result.returncode}")
                passed = False
        except FileNotFoundError:
            log_fail(f"{tool} not found in PATH")
            passed = False
        except Exception as e:
            log_fail(f"{tool} check failed: {e}")
            passed = False

    return passed


# ============================================================================
# Test 2: Video frame extraction (standalone)
# ============================================================================

def test_frame_extraction(video_path: str) -> bool:
    """Test that ffmpeg can extract frames from the video file."""
    log(f"Test 2: Testing frame extraction from: {video_path}")

    if not os.path.exists(video_path):
        log_fail(f"Video file not found: {video_path}")
        return False

    # Get video duration
    try:
        result = subprocess.run(
            [
                "ffprobe", "-v", "error",
                "-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1:nokey=1",
                video_path,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        duration = float(result.stdout.strip())
        log_pass(f"Video duration: {duration:.1f}s")
    except Exception as e:
        log_fail(f"Could not get video duration: {e}")
        return False

    # Get video resolution
    try:
        result = subprocess.run(
            [
                "ffprobe", "-v", "error",
                "-select_streams", "v:0",
                "-show_entries", "stream=width,height",
                "-of", "csv=p=0",
                video_path,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        width, height = result.stdout.strip().split(",")
        log_pass(f"Video resolution: {width}x{height}")
    except Exception as e:
        log_fail(f"Could not get video resolution: {e}")
        return False

    # Extract test frames to temp dir
    import tempfile
    with tempfile.TemporaryDirectory() as tmp_dir:
        max_frames = 5
        target_fps = max_frames / duration
        if target_fps > 30:
            target_fps = 30

        cmd = [
            "ffmpeg", "-i", video_path, "-y",
            "-vf", f"fps={target_fps:.4f}",
            "-frames:v", str(max_frames),
            os.path.join(tmp_dir, "frame_%04d.png"),
        ]

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=120,
            )
            frames = [f for f in os.listdir(tmp_dir) if f.startswith("frame_")]
            if len(frames) > 0:
                log_pass(f"Extracted {len(frames)} frames")
                for f in sorted(frames):
                    fpath = os.path.join(tmp_dir, f)
                    fsize = os.path.getsize(fpath)
                    log_pass(f"  {f}: {fsize} bytes")
            else:
                log_fail("No frames extracted")
                return False
        except Exception as e:
            log_fail(f"Frame extraction failed: {e}")
            return False

    return True


# ============================================================================
# Test 3: Server health check
# ============================================================================

def test_server_health(server_url: str) -> bool:
    """Check the server is running and supports vision."""
    log(f"Test 3: Server health check at {server_url}...")

    try:
        req = urllib.request.Request(f"{server_url}/health")
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
            status = data.get("status", "unknown")
            if status == "ok":
                log_pass(f"Server healthy: {data}")
                return True
            else:
                log_fail(f"Server status: {status}")
                return False
    except Exception as e:
        log_fail(f"Server not reachable: {e}")
        return False


# ============================================================================
# Test 4: Image API (baseline - existing functionality)
# ============================================================================

def test_image_api(server_url: str) -> bool:
    """Test that the existing image_url API works (baseline)."""
    log("Test 4: Testing existing image_url API (baseline)...")

    # Create a simple 2x2 red PNG (minimal valid PNG)
    import base64
    # Minimal 2x2 red PNG in base64
    red_png_b64 = (
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAADklEQVQI12P4z8BQDwAEgAF/"
        "QualIQAAAABJRU5ErkJggg=="
    )

    payload = {
        "model": "test",
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "image_url",
                        "image_url": {
                            "url": f"data:image/png;base64,{red_png_b64}",
                        },
                    },
                    {
                        "type": "text",
                        "text": "What do you see in this image? Reply in one sentence.",
                    },
                ],
            }
        ],
        "max_tokens": 100,
        "temperature": 0.1,
    }

    try:
        data = json.dumps(payload).encode()
        req = urllib.request.Request(
            f"{server_url}/v1/chat/completions",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read().decode())
            content = result["choices"][0]["message"]["content"]
            log_pass(f"Image API response: {content[:100]}")
            return True
    except Exception as e:
        log_fail(f"Image API failed: {e}")
        return False


# ============================================================================
# Test 5: Video URL API
# ============================================================================

def test_video_api(server_url: str, video_path: str) -> bool:
    """Test the new video_url API endpoint."""
    log(f"Test 5: Testing video_url API with: {video_path}")

    if not os.path.exists(video_path):
        log_fail(f"Video file not found: {video_path}")
        return False

    payload = {
        "model": "test",
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "video_url",
                        "video_url": {
                            "url": video_path,
                            "max_frames": 5,
                        },
                    },
                    {
                        "type": "text",
                        "text": "Describe what you see in this video. What is the main topic being presented?",
                    },
                ],
            }
        ],
        "max_tokens": 300,
        "temperature": 0.1,
    }

    try:
        data = json.dumps(payload).encode()
        req = urllib.request.Request(
            f"{server_url}/v1/chat/completions",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=300) as resp:
            result = json.loads(resp.read().decode())
            content = result["choices"][0]["message"]["content"]
            log_pass(f"Video API response ({len(content)} chars):")
            print(f"    {content[:300]}")

            # Basic sanity: response should be non-trivial
            if len(content) > 20:
                log_pass("Response is non-trivial (>20 chars)")
            else:
                log_fail("Response too short, model may not have understood the video")
                return False

            return True
    except urllib.error.HTTPError as e:
        error_body = e.read().decode() if e.fp else ""
        log_fail(f"Video API HTTP error {e.code}: {error_body[:500]}")
        return False
    except Exception as e:
        log_fail(f"Video API failed: {e}")
        return False


# ============================================================================
# Test 6: Video with multiple frames (stress test)
# ============================================================================

def test_video_multi_frame(server_url: str, video_path: str) -> bool:
    """Test video with more frames to check temporal mRoPE."""
    log(f"Test 6: Testing video with 10 frames (temporal mRoPE stress)...")

    if not os.path.exists(video_path):
        log_fail(f"Video file not found: {video_path}")
        return False

    payload = {
        "model": "test",
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "video_url",
                        "video_url": {
                            "url": video_path,
                            "max_frames": 10,
                        },
                    },
                    {
                        "type": "text",
                        "text": "This is a video of a presentation. How many different slides or sections do you see? What are the main topics covered?",
                    },
                ],
            }
        ],
        "max_tokens": 500,
        "temperature": 0.1,
    }

    try:
        data = json.dumps(payload).encode()
        req = urllib.request.Request(
            f"{server_url}/v1/chat/completions",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        t0 = time.time()
        with urllib.request.urlopen(req, timeout=600) as resp:
            elapsed = time.time() - t0
            result = json.loads(resp.read().decode())
            content = result["choices"][0]["message"]["content"]
            usage = result.get("usage", {})
            log_pass(f"Multi-frame response in {elapsed:.1f}s:")
            print(f"    {content[:400]}")
            if usage:
                log_pass(f"Token usage: prompt={usage.get('prompt_tokens')}, completion={usage.get('completion_tokens')}")
            return len(content) > 30
    except Exception as e:
        log_fail(f"Multi-frame test failed: {e}")
        return False


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Test llama.cpp video support")
    parser.add_argument("--server-url", default=DEFAULT_SERVER_URL, help="Server URL")
    parser.add_argument("--video-path", default=DEFAULT_VIDEO_PATH, help="Path to test video")
    parser.add_argument("--model", default=DEFAULT_MODEL_PATH, help="Model path")
    parser.add_argument("--mmproj", default=DEFAULT_MMPROJ_PATH, help="MMProj path")
    parser.add_argument("--skip-server", action="store_true", help="Skip server tests")
    parser.add_argument("--start-server", action="store_true", help="Auto-start server")
    args = parser.parse_args()

    log("=" * 60)
    log("llama.cpp Video Support Test Suite")
    log("=" * 60)

    results = {}
    server_proc = None

    # Test 1: FFmpeg
    results["ffmpeg"] = test_ffmpeg_available()
    print()

    # Test 2: Frame extraction
    results["frame_extraction"] = test_frame_extraction(args.video_path)
    print()

    if not args.skip_server:
        # Optionally start the server
        if args.start_server:
            log("Starting llama-server...")
            server_bin = os.path.join(BUILD_DIR, "llama-server")
            if os.name == "nt":
                server_bin += ".exe"

            server_cmd = [
                server_bin,
                "-m", args.model,
                "--mmproj", args.mmproj,
                "-c", "8192",
                "-ngl", "99",
                "--host", "0.0.0.0",
                "--port", "8080",
                "--jinja",
            ]
            log(f"Command: {' '.join(server_cmd)}")
            server_proc = subprocess.Popen(
                server_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            log("Waiting for server to start...")
            for i in range(60):
                time.sleep(2)
                try:
                    req = urllib.request.Request(f"{args.server_url}/health")
                    with urllib.request.urlopen(req, timeout=5) as resp:
                        data = json.loads(resp.read().decode())
                        if data.get("status") == "ok":
                            log_pass(f"Server started after {(i+1)*2}s")
                            break
                except Exception:
                    pass
            else:
                log_fail("Server did not start within 120s")
                if server_proc:
                    server_proc.terminate()
                return 1

        # Test 3: Server health
        results["server_health"] = test_server_health(args.server_url)
        print()

        if results["server_health"]:
            # Test 4: Image API baseline
            results["image_api"] = test_image_api(args.server_url)
            print()

            # Test 5: Video API
            results["video_api"] = test_video_api(args.server_url, args.video_path)
            print()

            # Test 6: Multi-frame
            if results["video_api"]:
                results["video_multi_frame"] = test_video_multi_frame(args.server_url, args.video_path)
                print()

    # Summary
    log("=" * 60)
    log("TEST SUMMARY")
    log("=" * 60)
    total = len(results)
    passed = sum(1 for v in results.values() if v)
    for name, result in results.items():
        status = "\u2705 PASS" if result else "\u274c FAIL"
        log(f"  {status}: {name}")

    log(f"\nResult: {passed}/{total} tests passed")

    if server_proc:
        log("Shutting down server...")
        server_proc.terminate()

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
