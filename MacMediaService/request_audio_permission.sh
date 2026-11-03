#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if [ ! -f "$DIR/audio_analyzer" ] || [ "$DIR/audio_analyzer.swift" -nt "$DIR/audio_analyzer" ]; then
    echo "[BUILD] Компиляция audio_analyzer..."
    /usr/bin/swiftc -O "$DIR/audio_analyzer.swift" -o "$DIR/audio_analyzer"
fi

echo "=================================================="
echo "  Проверка разрешения macOS ScreenCaptureKit"
echo "  (Запись экрана и системного аудио)"
echo "=================================================="
RES=$("$DIR/audio_analyzer" --request-permission)
echo "Результат: $RES"
echo ""
echo "Если появилось системное окно macOS:"
echo "1. Нажмите «Открыть Системные настройки»."
echo "2. В разделе «Конфиденциальность и безопасность» ->"
echo "   «Запись экрана и системного аудио» включите тумблер"
echo "   для вашего терминала (Terminal / iTerm / IDE)."
echo "3. Перезапускать сервис не требуется — захват аудио"
echo "   начнется автоматически в течение 2 секунд."
echo "=================================================="

