# Правила разработки и прошивки ESP32-S3 (DiplomaVoiceAssistant)

## Автоматическая компиляция и загрузка прошивки по завершении полного этапа
Не нужно загружать прошивку на ESP32-S3 после каждого микрошага — сначала полностью завершай все необходимые изменения в коде, и только когда всё готово целиком, компилируй и загружай прошивку в подключенную плату ESP32-S3 без дополнительного подтверждения.

### Алгоритм безошибочной загрузки в плату:
1. Определить подключенный USB-порт через `/opt/homebrew/bin/arduino-cli board list` (обычно `/dev/cu.usbserial-110` или аналогичный `/dev/cu.usbserial-*`).
2. Освободить порт от фонового монитора порта (чтобы избежать ошибки `Resource busy` и автоперезапуска `serial-monitor`):
   ```bash
   pkill -f "arduino-cli monitor" 2>/dev/null || true
   pkill -f "serial-monitor" 2>/dev/null || true
   ```
3. Скомпилировать и прошить скетч с обязательным параметром `UploadSpeed=115200` (на скорости `921600` USB-UART мост обрывает соединение):
   ```bash
   /opt/homebrew/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3 /Users/yerassyl/DiplomaVoiceAssistant/VoiceAssistantUI && \
   /opt/homebrew/bin/arduino-cli upload -p /dev/cu.usbserial-110 --fqbn esp32:esp32:esp32s3:UploadSpeed=115200 /Users/yerassyl/DiplomaVoiceAssistant/VoiceAssistantUI
   ```

