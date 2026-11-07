#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PID_FILE="$DIR/.service.pid"
LOG_FILE="$DIR/service.log"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if ps -p "$PID" > /dev/null 2>&1; then
        echo "[INFO] MacMediaService уже запущен (PID $PID)."
        exit 0
    else
        rm "$PID_FILE"
    fi
fi

# 1. Проверяем наличие скрипта-обертки media_helper
if [ ! -f "$DIR/media_helper" ]; then
    printf '#!/bin/zsh\nexec /usr/bin/swift "%s/media_helper.swift" "$@"\n' "$DIR" > "$DIR/media_helper"
    chmod +x "$DIR/media_helper"
fi

# 2. Компилируем нативный анализатор системного аудио audio_analyzer (если нужно)
if [ ! -f "$DIR/audio_analyzer" ] || [ "$DIR/audio_analyzer.swift" -nt "$DIR/audio_analyzer" ]; then
    echo "[BUILD] Компиляция модуля захвата системного аудио (audio_analyzer.swift)..."
    /usr/bin/swiftc -O "$DIR/audio_analyzer.swift" -o "$DIR/audio_analyzer"
fi

# 2.5. Запуск локального нейросетевого TTS API Доса (OmniVoice 612M)
TTS_PID_FILE="$DIR/.tts_service.pid"
TTS_LOG_FILE="$DIR/tts_service.log"
TTS_PY="/Users/yerassyl/DiplomaVoiceAssistant/tts_env/bin/python"

if [ -f "$TTS_PID_FILE" ] && ps -p "$(cat "$TTS_PID_FILE")" > /dev/null 2>&1; then
    echo "[INFO] Kazakh TTS API (OmniVoice) уже запущен."
else
    if [ -f "$TTS_PY" ] && [ -f "$DIR/kazakh_tts_api.py" ]; then
        echo "[START] Запуск Kazakh TTS API (OmniVoice)..."
        nohup "$TTS_PY" "$DIR/kazakh_tts_api.py" > "$TTS_LOG_FILE" 2>&1 &
        echo $! > "$TTS_PID_FILE"
    fi
fi

# 3. Запуск сервиса в фоне (используем tts_env с поддержкой TorchScript STT и TTS)
echo "[START] Запуск MacMediaService..."
nohup "$TTS_PY" "$DIR/server.py" > "$LOG_FILE" 2>&1 &
NEW_PID=$!
echo $NEW_PID > "$PID_FILE"

sleep 1.5
if ps -p "$NEW_PID" > /dev/null 2>&1; then
    echo "=================================================="
    echo "  MacMediaService успешно запущен! (PID $NEW_PID)"
    echo "  Лог работы: $LOG_FILE"
    echo "  Остановить: ./stop_service.sh"
    echo "=================================================="
    IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo "127.0.0.1")
    echo "  -> Укажите в NetworkConfig.h на ESP32:"
    echo "     #define SERVER_HOST \"$IP\""
    echo "     #define SERVER_PORT 8765"
    echo "=================================================="
    PERM=$("$DIR/audio_analyzer" --check-permission 2>/dev/null)
    if [[ "$PERM" == *"false"* ]]; then
        echo "  [ВНИМАНИЕ] Для работы эквалайзера от системного звука"
        echo "  требуется разрешение macOS ScreenCaptureKit:"
        echo "  Запустите: ./request_audio_permission.sh"
        echo "=================================================="
    fi
else
    echo "[ERROR] Сервис завершился с ошибкой. Проверьте лог: $LOG_FILE"
    cat "$LOG_FILE"
    exit 1
fi
