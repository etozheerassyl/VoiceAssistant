#!/usr/bin/env python3
"""
Test Client & DSP Verification for MacMediaService
1. Verifies PCMSpectrumAnalyzer (16-band logarithmic FFT, stereo-to-mono, clipping protection, silence decay).
2. Emulates ESP32-S3 over local TCP connection to test media_state, equalizer stream, and commands.
"""

import socket
import json
import time
import math
import sys

from audio_processor import PCMSpectrumAnalyzer, sanitize_equalizer_bands

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 8765
AUTH_TOKEN = "esp32_mac_secret_key"


def test_dsp_module():
    print("1. Testing Python PCMSpectrumAnalyzer DSP module...")
    analyzer = PCMSpectrumAnalyzer(band_count=16, fft_size=1024, sample_rate=48000)

    # Generate stereo test signal: 80 Hz (bass) on Left, 3000 Hz (presence) on Right
    sr = 48000.0
    frames = 1024
    stereo = []
    for i in range(frames):
        t = i / sr
        left = 0.6 * math.sin(2.0 * math.pi * 80.0 * t)
        right = 0.5 * math.sin(2.0 * math.pi * 3000.0 * t)
        stereo.append(left)
        stereo.append(right)

    now = 100.0
    for step in range(8):
        analyzer.push_pcm(stereo, channels=2, sample_rate=48000, now=now + step * 0.02)
        bands = analyzer.compute_bands(now=now + step * 0.02)

    print(f"   [DSP] 16 bands for 80Hz + 3000Hz stereo signal: {bands}")
    assert len(bands) == 16, "Expected 16 bands"
    assert all(0 <= b <= 100 for b in bands), "Bands must be in 0..100"
    assert max(bands[2:5]) > 40, "Expected strong bass response around 80 Hz"
    assert max(bands[10:13]) > 40, "Expected strong presence response around 3000 Hz"

    # Test clipping & NaN protection
    corrupted = [float("nan"), float("inf"), -float("inf"), 99.0, -99.0] * 400
    analyzer.push_pcm(corrupted, channels=2, sample_rate=48000, now=now + 0.5)
    clip_bands = analyzer.compute_bands(now=now + 0.5)
    assert len(clip_bands) == 16 and all(0 <= b <= 100 for b in clip_bands)
    print(f"   [DSP] Clipping/NaN protection verified: {clip_bands}")

    # Test smooth decay on pause/silence
    for step in range(1, 30):
        decay_bands = analyzer.compute_bands(now=now + 1.0 + step * 0.15)
    assert all(b == 0 for b in decay_bands), f"Expected decay to 0, got {decay_bands}"
    print(f"   [DSP] Silence decay verified: {decay_bands}")


def test_tcp_server():
    print(f"\n2. Connecting to MacMediaService at {SERVER_HOST}:{SERVER_PORT}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((SERVER_HOST, SERVER_PORT))
        print("   Connected! Sending auth token...")

        auth_msg = {"type": "auth", "token": AUTH_TOKEN}
        s.sendall((json.dumps(auth_msg) + "\n").encode("utf-8"))
        s.settimeout(3.0)

        buffer = ""
        got_media = False
        got_eq = False
        got_auth = False

        print("   Reading stream messages (media_state, equalizer, auth response)...")
        deadline = time.time() + 3.0
        while time.time() < deadline and not (got_media and got_eq and got_auth):
            chunk = s.recv(2048)
            if not chunk:
                break
            buffer += chunk.decode("utf-8")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                if not line.strip():
                    continue
                msg = json.loads(line)
                mtype = msg.get("type")
                print(f"   [RECV {mtype}]: {line}")
                if mtype == "media_state":
                    got_media = True
                elif mtype == "equalizer":
                    assert len(msg.get("bands", [])) == 16, "Expected 16 bands in equalizer msg"
                    got_eq = True
                elif mtype == "response" and msg.get("command") == "auth":
                    assert msg.get("success") is True
                    got_auth = True

        print("\n3. Sending GET_STATUS command...")
        cmd = {"type": "command", "command": "GET_STATUS"}
        s.sendall((json.dumps(cmd) + "\n").encode("utf-8"))

        deadline = time.time() + 2.0
        while time.time() < deadline:
            chunk = s.recv(2048)
            if not chunk:
                break
            buffer += chunk.decode("utf-8")
            if "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                print(f"   [RECV GET_STATUS]: {line}")
                break

        s.close()
        assert got_media and got_eq and got_auth, "Did not receive all expected message types"
        print("\nAll DSP and TCP integration tests completed successfully!")
    except Exception as e:
        print(f"Test client error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    test_dsp_module()
    test_tcp_server()
