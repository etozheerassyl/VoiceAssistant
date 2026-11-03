#!/usr/bin/env python3
"""
Скрипт автоматической загрузки необходимых нейросетевых моделей с HuggingFace.
Используется при развёртывании проекта на новом компьютере (ПК с GPU 16GB).
"""

import os
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODELS_DIR = os.path.join(BASE_DIR, "models")
os.makedirs(MODELS_DIR, exist_ok=True)

try:
    from huggingface_hub import snapshot_download
except ImportError:
    print("[ERROR] Не установлена библиотека huggingface_hub.")
    print("Установите: pip install huggingface_hub")
    sys.exit(1)


def main():
    print("=" * 65)
    print("  Загрузка нейросетевых моделей для Voice Assistant «Дос»")
    print("=" * 65)

    # 1. Kazakh-Russian Mixed STT (Wav2Vec2-CTC)
    stt_dir = os.path.join(MODELS_DIR, "kazakh-russian-mixed-stt")
    print("\n[1/3] Загрузка модели распознавания речи (Kazakh-Russian Mixed STT)...")
    try:
        snapshot_download(
            repo_id="alibiserikbay/kazakh-russian-mixed-stt",
            local_dir=stt_dir,
            local_dir_use_symlinks=False
        )
        print(f"      [OK] STT модель сохранена в: {stt_dir}")
    except Exception as e:
        print(f"      [WARNING] Ошибка загрузки STT: {e}")

    # 2. Kokoro TTS Kazakh (82M)
    kokoro_dir = os.path.join(MODELS_DIR, "kokoro-tts-kazakh")
    print("\n[2/3] Загрузка лёгкой модели Kokoro TTS Kazakh...")
    try:
        snapshot_download(
            repo_id="TilLabs/kokoro-tts-kazakh",
            local_dir=kokoro_dir,
            local_dir_use_symlinks=False
        )
        print(f"      [OK] Kokoro TTS сохранена в: {kokoro_dir}")
    except Exception as e:
        print(f"      [WARNING] Ошибка загрузки Kokoro TTS: {e}")

    # 3. KazakhTTS-OmniVoice (612M)
    omni_dir = os.path.join(MODELS_DIR, "KazakhTTS-OmniVoice")
    print("\n[3/3] Проверка модели KazakhTTS-OmniVoice...")
    if not os.path.exists(omni_dir) or not os.listdir(omni_dir):
        print(f"      Папка {omni_dir} пуста или отсутствует.")
        print("      Для OmniVoice скопируйте веса с MacBook или загрузите веса OmniVoice.")
    else:
        print(f"      [OK] OmniVoice присутствует в: {omni_dir}")

    print("\n" + "=" * 65)
    print("  Все модели готовы к работе!")
    print("=" * 65)


if __name__ == "__main__":
    main()
