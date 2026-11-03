#!/usr/bin/env python3
"""
Тест сквозного контура голосового ассистента «Дос»:
Вопрос пользователя -> Ollama (qwen2.5:3b) -> Kazakh TTS API (OmniVoice) -> Аудио на MacBook.
"""

import sys
import os
import time

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from voice_assistant import query_local_llm, speak_on_mac, _synthesize_to_cache

def main():
    print("=" * 65)
    print("  Тестирование сквозного контура казахоязычного ассистента «Дос»")
    print("=" * 65)

    test_queries = [
        "Сәлем Дос! Өзіңді таныстырып өтші, сен кімсің және не істей аласың?",
        "Дос, қазір Алматыда сағат неше және ауа райы қандай?"
    ]

    mock_state = {
        "title": "Ойнатылмай тұр",
        "artist": "",
        "source": "macOS",
        "volume": 60,
        "state": "paused",
        "weather": {
            "temp": 18,
            "desc_ru": "ашық күн",
            "usd_kzt": 495,
            "mac_battery": 92
        },
        "date": "бүгін"
    }

    for idx, user_prompt in enumerate(test_queries, 1):
        print(f"\n--- [Тест {idx}/2] Сұрақ: «{user_prompt}» ---")
        
        t0 = time.time()
        print("[1/3] Запрос к локальной LLM Qwen2.5:3B...")
        llm_res = query_local_llm(user_prompt, mock_state)
        t_llm = time.time() - t0
        
        reply_screen = llm_res.get("reply", "")
        spoken_text = llm_res.get("spoken", "")
        action = llm_res.get("action", "")
        
        print(f"      Ответ LLM получен за {t_llm:.2f} сек.")
        print(f"      Экран (ESP32): «{reply_screen}»")
        print(f"      Голос (Дос):   «{spoken_text}»")
        print(f"      Действие:      «{action}»")

        print("[2/3] Синтез речи через Kazakh TTS API (OmniVoice)...")
        t_tts = time.time()
        audio_path, engine = _synthesize_to_cache(spoken_text)
        dur_tts = time.time() - t_tts
        print(f"      Аудио создано за {dur_tts:.2f} сек. Движок: {engine}")
        print(f"      Файл: {audio_path}")

        print("[3/3] Воспроизведение через динамики MacBook...")
        speak_on_mac(spoken_text)
        time.sleep(dur_tts + 2.0)

    print("\n" + "=" * 65)
    print("Сквозной тест успешно завершен!")
    print("=" * 65)

if __name__ == "__main__":
    main()

