#!/usr/bin/env python3
"""
test_stt_pipeline.py — Тестирование полного цикла голосового взаимодействия «Дос»:
ESP32 PCM16 Audio -> Local Mixed STT -> Qwen2.5:3B LLM / Commands -> Kazakh TTS (OmniVoice).
"""

import os
import sys
import time
import soundfile as sf
import torchaudio
import torch
import numpy as np

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(BASE_DIR)
sys.path.insert(0, BASE_DIR)

from voice_assistant import recognize_pcm16, process_voice_command, query_local_llm

def file_to_pcm16_8k(file_path: str) -> bytes:
    """Загружает файл и конвертирует в 8000 Гц 16-бит моно PCM (как отправляет ESP32-S3)."""
    wav, sr = sf.read(file_path, dtype="float32")
    if wav.ndim > 1:
        wav = wav.mean(axis=1)
    if sr != 8000:
        resampler = torchaudio.transforms.Resample(orig_freq=sr, new_freq=8000)
        wav = resampler(torch.from_numpy(wav)).numpy()
    pcm = (np.clip(wav, -1.0, 1.0) * 32767.0).astype(np.int16).tobytes()
    return pcm

def main():
    print("=" * 70)
    print("  Полный тест сквозного пайплайна STT -> LLM -> TTS («Дос»)")
    print("=" * 70)

    test_audios = [
        ("numbers_dates.wav", "Бүгін екі мың жиырма алтыншы жылдың үшінші қарашасы, сағат он төрт отыз."),
        ("greeting.wav", "Сәлем! Мен Доспын, сенің қазақша дауыстық көмекшіңмін."),
        ("/tmp/cmd_weather.aiff", "Какая сейчас погода в Алматы")
    ]

    mock_state = {
        "title": "Дос өлеңі",
        "artist": "Қазақ әуені",
        "source": "macOS",
        "volume": 70,
        "state": "playing",
        "weather": {
            "temp": 20,
            "desc_ru": "ашық аспан",
            "usd_kzt": 495,
            "mac_battery": 95
        },
        "date": "бүгін"
    }

    samples_dir = os.path.join(PROJECT_ROOT, "audio_samples")

    for fpath_rel, expected_meaning in test_audios:
        fpath = os.path.join(samples_dir, fpath_rel) if not fpath_rel.startswith("/") else fpath_rel
        if not os.path.exists(fpath):
            continue

        print(f"\n>>> Тестирование аудио: {os.path.basename(fpath)}")
        print(f"    Эталонный смысл: «{expected_meaning}»")

        # 1. Симуляция микрофонного потока ESP32 (8 кГц PCM16)
        pcm_bytes = file_to_pcm16_8k(fpath)
        audio_dur = len(pcm_bytes) / (8000 * 2)
        print(f"    Сэмулирован поток ESP32: {len(pcm_bytes)} байт ({audio_dur:.2f} сек, 8000 Гц PCM16)")

        # 2. Распознавание речи через локальный STT
        t0 = time.time()
        recognized_text = recognize_pcm16(pcm_bytes, sample_rate=8000)
        dt_stt = time.time() - t0
        print(f"    [STT] Распознано за {dt_stt*1000:.1f} мс (RTF: {dt_stt/audio_dur:.3f}):")
        print(f"          «{recognized_text}»")

        # 3. Обработка команды / LLM
        t1 = time.time()
        res = process_voice_command(
            recognized_text,
            mock_state,
            set_volume_fn=lambda v: print(f"        [MOCK ACTION] Громкость: {v}%"),
            media_cmd_fn=lambda c: print(f"        [MOCK ACTION] Медиа: {c}")
        )
        dt_llm = time.time() - t1
        print(f"    [LOGIC/LLM] Время: {dt_llm:.2f} сек.")
        print(f"          Экран: «{res.get('reply', '')}»")
        print(f"          Голос: «{res.get('spoken', '')}»")
        print(f"          Эмоция/Экран: {res.get('emotion')}/{res.get('screen')}")

    print("\n" + "=" * 70)
    print("  Тестирование успешно завершено!")
    print("=" * 70)

if __name__ == "__main__":
    main()
