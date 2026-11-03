#!/usr/bin/env python3
"""
Kokoro TTS Kazakh — Тестовый скрипт проверки естественности казахской речи.
Модель: TilLabs/kokoro-tts-kazakh (82M параметров, ISSAI KazakhTTS2, голос km_m1).
"""

import os
import sys
import time
import resource
import subprocess
import torch
import soundfile as sf
from kokoro import KModel
from misaki import espeak

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_DIR = os.path.join(BASE_DIR, "models", "kokoro-tts-kazakh")
CONFIG_PATH = os.path.join(MODEL_DIR, "config.json")
WEIGHTS_PATH = os.path.join(MODEL_DIR, "kokoro_kazakh.pth")
VOICE_PATH = os.path.join(MODEL_DIR, "km_m1.pt")
OUTPUT_DIR = os.path.join(BASE_DIR, "audio_samples")
os.makedirs(OUTPUT_DIR, exist_ok=True)

def get_ram_mb() -> float:
    # На macOS getrusage возвращает байты
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1024.0 * 1024.0)

def main():
    print("=" * 60)
    print("  Казахоязычная умная колонка «Дос» — Тестирование TTS")
    print("=" * 60)

    ram_start = get_ram_mb()
    print(f"[1/4] Исходное потребление RAM: {ram_start:.1f} МБ")

    print("[2/4] Загрузка нейросети Kokoro-82M Kazakh...")
    t0 = time.time()
    model = KModel(repo_id="hexgrad/Kokoro-82M", config=CONFIG_PATH, model=WEIGHTS_PATH).eval()
    voicepack = torch.load(VOICE_PATH, map_location="cpu", weights_only=True)
    g2p = espeak.EspeakG2P(language="kk")
    t_load = time.time() - t0
    ram_loaded = get_ram_mb()
    print(f"      Модель успешно загружена за {t_load:.2f} сек. RAM: {ram_loaded:.1f} МБ")

    # Тестовые фразы: специфические звуки, числительные, имена, смешанная речь
    test_cases = [
        ("greeting", "Сәлем! Мен Доспын, сенің қазақша дауыстық көмекшіңмін. Саған қалай көмектесе аламын?"),
        ("letters_spec", "Әдемі өмір, жылы ұя, үлкен қала, ғажайып жаңа ілім."),
        ("numbers_dates", "Бүгін екі мың жиырма алтыншы жылдың үшінші қарашасы, сағат он төрт отыз."),
        ("geo_names", "Қасым-Жомарт Тоқаев Астана мен Алматы қалаларының дамуын талқылады."),
        ("mixed_speech", "YouTube-тан қазақша музыка қосып берші, өтініш.")
    ]

    print("[3/4] Синтез речи...")
    results = []
    for tag, text in test_cases:
        phonemes, _ = g2p(text)
        ref_s = voicepack[min(len(phonemes) - 1, voicepack.shape[0] - 1)]

        t_synth_start = time.time()
        audio = model(phonemes, ref_s, speed=1.0)
        synth_dur = time.time() - t_synth_start

        if isinstance(audio, tuple):
            audio = audio[0]
        audio_np = audio.detach().cpu().numpy()
        audio_dur = len(audio_np) / 24000.0
        rtf = synth_dur / audio_dur if audio_dur > 0 else 0

        out_path = os.path.join(OUTPUT_DIR, f"{tag}.wav")
        sf.write(out_path, audio_np, 24000)

        results.append((tag, text, phonemes, audio_dur, synth_dur, rtf, out_path))
        print(f"  ✓ [{tag}] Длина: {audio_dur:.2f}с | Синтез: {synth_dur:.3f}с | RTF: {rtf:.3f} | Файл: {os.path.basename(out_path)}")

    ram_peak = get_ram_mb()
    print(f"[4/4] Пиковое потребление RAM: {ram_peak:.1f} МБ (прирост: +{ram_peak - ram_start:.1f} МБ)")
    print("=" * 60)
    print("Воспроизведение приветственной фразы через динамики MacBook...")
    greeting_file = os.path.join(OUTPUT_DIR, "greeting.wav")
    if os.path.exists(greeting_file):
        subprocess.run(["afplay", greeting_file])
    print("Тестирование успешно завершено!")

if __name__ == "__main__":
    main()

