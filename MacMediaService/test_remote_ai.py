#!/usr/bin/env python3
"""
Скрипт диагностики и тестирования удалённого AI-сервера (ПК с GPU 16GB).
Проверяет сетевую доступность, Ollama, TTS и отказоустойчивый Fallback на MacBook.
"""

import os
import sys
import time
import json
import urllib.request
import requests

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from voice_assistant import (
    load_ai_config,
    get_ollama_endpoint,
    get_tts_endpoint,
    query_local_llm,
)


def main():
    print("=" * 70)
    print("  Диагностика распределённого AI-бэкенда («Дос»)")
    print("=" * 70)

    cfg = load_ai_config()
    remote_host = cfg.get("remote_ai_host", "").strip()

    print(f"1. Конфигурация (ai_config.json):")
    print(f"   • remote_ai_host:         '{remote_host or '<НЕ ЗАДАН (только локальный Mac)>'}'")
    print(f"   • remote_ollama_port:     {cfg.get('remote_ollama_port', 11434)}")
    print(f"   • remote_ollama_model:    {cfg.get('remote_ollama_model')}")
    print(f"   • remote_tts_port:        {cfg.get('remote_tts_port', 8001)}")
    print(f"   • fallback_local_ollama:  {cfg.get('fallback_local_ollama', True)}")
    print(f"   • local_ollama_model:     {cfg.get('local_ollama_model')}")

    # 2. Проверка активного Ollama эндпоинта
    print("\n2. Проверка подключения к Ollama:")
    url, model, is_remote = get_ollama_endpoint()
    if is_remote:
        print(f"   [OK] Подключен УДАЛЁННЫЙ ПК с GPU: {url} (модель: {model})")
    else:
        if remote_host:
            print(f"   [FALLBACK] Удалённый ПК ({remote_host}) оффлайн. Активен локальный MacBook: {url}")
        else:
            print(f"   [INFO] Удалённый ПК не настроен. Работает локальный MacBook: {url}")

    # 3. Тест инференса LLM
    print("\n3. Тестовый запрос к LLM (замер скорости):")
    mock_state = {
        "title": "", "artist": "", "volume": 50, "state": "paused",
        "weather": {"temp": 18, "desc_ru": "ясно", "usd_kzt": 495, "mac_battery": 95}
    }
    test_q = "сәлем дос! сен қайда жұмыс істеп тұрсың?"
    t0 = time.time()
    first_tok_t = None

    def on_sent(s):
        nonlocal first_tok_t
        if first_tok_t is None:
            first_tok_t = time.time() - t0
        print(f"      -> Первое предложение ({first_tok_t:.2f}с): «{s}»")

    try:
        ans = query_local_llm(test_q, mock_state, on_sentence=on_sent)
        dt = time.time() - t0
        print(f"   [OK] Полный ответ за {dt:.2f} сек.")
        print(f"        Экран: «{ans.get('reply')}»")
        print(f"        Голос: «{ans.get('spoken')}»")
    except Exception as e:
        print(f"   [ERROR] Ошибка запроса к LLM: {e}")

    # 4. Проверка TTS эндпоинта
    print("\n4. Проверка синтеза речи (TTS):")
    tts_url, health_url, tts_remote = get_tts_endpoint()
    if tts_remote:
        print(f"   [OK] Удалённый OmniVoice на ПК активен: {tts_url}")
    else:
        print(f"   [INFO] OmniVoice на ПК не найден -> используется Edge Neural TTS (Svetlana / Daulet)")

    print("\n" + "=" * 70)
    print("  Диагностика завершена успешно.")
    print("=" * 70)


if __name__ == "__main__":
    main()

