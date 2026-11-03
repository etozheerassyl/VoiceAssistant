#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PID_FILE="$DIR/.service.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if ps -p "$PID" > /dev/null 2>&1; then
        echo "[STOP] Остановка MacMediaService (PID $PID)..."
        kill "$PID"
        sleep 0.5
        if ps -p "$PID" > /dev/null 2>&1; then
            kill -9 "$PID"
        fi
        echo "[STOP] Сервис остановлен."
    else
        echo "[INFO] Процесс $PID не найден (сервис уже остановлен)."
    fi
    rm -f "$PID_FILE"
else
    PID=$(pgrep -f "server.py" | head -n 1)
    if [ -n "$PID" ]; then
        echo "[STOP] Найден процесс server.py (PID $PID), завершение..."
        kill "$PID"
        echo "[STOP] Сервис остановлен."
    else
        echo "[INFO] Сервис MacMediaService не запущен."
    fi
fi

# Остановка Kazakh TTS API
TTS_PID_FILE="$DIR/.tts_service.pid"
if [ -f "$TTS_PID_FILE" ]; then
    TTS_PID=$(cat "$TTS_PID_FILE")
    if ps -p "$TTS_PID" > /dev/null 2>&1; then
        echo "[STOP] Остановка Kazakh TTS API (PID $TTS_PID)..."
        kill "$TTS_PID" 2>/dev/null || true
    fi
    rm -f "$TTS_PID_FILE"
fi
pkill -f "kazakh_tts_api.py" 2>/dev/null || true

# Завершаем дочерние демоны media_helper.swift и audio_analyzer.swift, если они остались
pkill -f "media_helper.swift --daemon" 2>/dev/null || true
pkill -f "audio_analyzer" 2>/dev/null || true
