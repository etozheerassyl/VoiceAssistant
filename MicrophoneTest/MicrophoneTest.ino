#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_err.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Конфигурация подключения микрофона INMP441 к ESP32-S3
// -----------------------------------------------------------------------------
// Дисплей временно полностью отключён.
// Wi-Fi, усилитель и светодиоды не используются.
// Задействованы строго указанные GPIO:
//   VDD -> 3V3
//   GND -> GND
//   SCK -> GPIO4  (I2S BCLK / Bit Clock)
//   WS  -> GPIO5  (I2S LRCLK / Word Select)
//   SD  -> GPIO6  (I2S DIN / Serial Data)
//   L/R -> GND    (Левый канал / Left Channel)
// -----------------------------------------------------------------------------
const int8_t PIN_I2S_BCLK = 4;
const int8_t PIN_I2S_WS   = 5;
const int8_t PIN_I2S_DIN  = 6;

// -----------------------------------------------------------------------------
// Параметры аудио
// -----------------------------------------------------------------------------
const uint32_t SAMPLE_RATE       = 16000; // 16000 Гц
const size_t   CHUNK_SAMPLES     = 512;   // 512 сэмплов за вызов (~32 мс)
const uint32_t PRINT_INTERVAL_MS = 250;   // Вывод в Serial 4 раза в секунду (каждые 250 мс)

// Экземпляр I2S шины (ESP32 Arduino Core 3.x)
I2SClass i2s;

// Буфер для 32-битных сэмплов из DMA
int32_t audioBuffer[CHUNK_SAMPLES];

// Переменные для накопления статистики за интервал ~250 мс
double   accumSum   = 0.0;
double   accumSumSq = 0.0;
int32_t  accumPeak  = 0;
uint32_t accumCount = 0;
uint32_t zeroCount  = 0;
uint32_t lastPrintTime = 0;
uint32_t consecutiveZeroWindows = 0;

// Отрисовка псевдографического VU-метра
void printVuMeter(float dbfs, int barWidth = 20) {
  const float minDb = -60.0f; // Уровень условной тишины в помещении
  const float maxDb = 0.0f;   // Предел шкалы (0 dBFS)

  int filled = 0;
  if (dbfs > minDb) {
    float norm = (dbfs - minDb) / (maxDb - minDb);
    if (norm > 1.0f) norm = 1.0f;
    filled = (int)(norm * barWidth);
  }

  Serial.print("[");
  for (int i = 0; i < barWidth; ++i) {
    if (i < filled) {
      Serial.print("#");
    } else {
      Serial.print("-");
    }
  }
  Serial.print("]");
}

void setup() {
  Serial.begin(115200);
  delay(1500); // Ожидание подключения Serial Monitor

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   ESP32-S3 Voice Assistant - Microphone Test");
  Serial.println("   Core: ESP32 Arduino Core 3.x (ESP_I2S Driver)");
  Serial.println("==================================================");
  Serial.println("Микрофон: INMP441 (цифровой I2S MEMS)");
  Serial.println("Подключение:");
  Serial.printf("  SCK (BCLK)  -> GPIO %d\n", PIN_I2S_BCLK);
  Serial.printf("  WS  (LRCLK) -> GPIO %d\n", PIN_I2S_WS);
  Serial.printf("  SD  (DIN)   -> GPIO %d\n", PIN_I2S_DIN);
  Serial.println("  L/R         -> GND (Левый канал)");
  Serial.println("Параметры:");
  Serial.printf("  Частота:     %u Гц\n", SAMPLE_RATE);
  Serial.println("  Разрядность: 32 бита (данные INMP441 в старших 24 битах)");
  Serial.println("  Режим:       MONO (Левый слот / Left Slot)");
  Serial.println("--------------------------------------------------");

  // Настройка выводов: bclk, ws, dout, din, mclk
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1, PIN_I2S_DIN);

  // Инициализация стандартного режима I2S
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    esp_err_t err = (esp_err_t)i2s.lastError();
    Serial.printf("[FATAL ERROR] Не удалось инициализировать I2S! Ошибка: %s (код 0x%X)\n", esp_err_to_name(err), err);
    Serial.println("[HINT] Проверьте корректность распиновки и отсутствие конфликта периферии.");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("[OK] I2S шина успешно инициализирована.");
  Serial.println("[INFO] Начинаем измерение уровня звукового сигнала...");
  Serial.println("--------------------------------------------------");
  Serial.println("Формат: RMS_16 | Peak_16 | dBFS | Шкала уровня | Статус");
  Serial.println("--------------------------------------------------");

  lastPrintTime = millis();
}

void loop() {
  // Непрерывное чтение порции аудиосэмплов из DMA
  size_t bytesToRead = sizeof(audioBuffer);
  size_t bytesRead = i2s.readBytes((char *)audioBuffer, bytesToRead);

  // Проверка результата чтения I2S
  if (bytesRead != bytesToRead) {
    esp_err_t err = (esp_err_t)i2s.lastError();
    Serial.printf("[ERROR] Сбой чтения I2S: получено %u из %u байт! Ошибка: %s (0x%X)\n",
                  (unsigned)bytesRead, (unsigned)bytesToRead, esp_err_to_name(err), err);
    delay(50);
    return;
  }

  size_t samplesCount = bytesRead / sizeof(int32_t);

  // Обработка прочитанных 32-битных сэмплов
  for (size_t i = 0; i < samplesCount; ++i) {
    int32_t raw = audioBuffer[i];

    if (raw == 0) {
      zeroCount++;
    }

    // Данные INMP441 выровнены по старшим 24 битам 32-битного слова.
    // Арифметический сдвиг вправо на 8 бит сохраняет знак и приводит к 24-битному числу.
    int32_t val24 = raw >> 8;

    accumSum += (double)val24;
    accumSumSq += ((double)val24 * (double)val24);

    int32_t absVal = (val24 < 0) ? -val24 : val24;
    if (absVal > accumPeak) {
      accumPeak = absVal;
    }

    accumCount++;
  }

  // Периодический вывод агрегированных результатов (4 раза в секунду)
  uint32_t now = millis();
  if (now - lastPrintTime >= PRINT_INTERVAL_MS) {
    if (accumCount > 0) {
      // 1. Постоянная составляющая (DC Offset)
      double meanDC = accumSum / accumCount;

      // 2. Истинное среднеквадратичное значение (AC RMS без влияния DC смещения)
      double variance = (accumSumSq / accumCount) - (meanDC * meanDC);
      if (variance < 0.0) {
        variance = 0.0;
      }
      double rms = sqrt(variance);

      // 3. Пиковое значение относительно смещения
      double peakAc = (double)accumPeak;

      // 4. Приведение к 16-битной шкале (0 .. 32767) для наглядности
      int32_t rms16 = (int32_t)(rms / 256.0);
      int32_t peak16 = (int32_t)(peakAc / 256.0);
      if (rms16 > 32767) rms16 = 32767;
      if (peak16 > 32767) peak16 = 32767;

      // 5. Расчет уровня сигнала в dBFS (относительно максимальной амплитуды 24 бит)
      float dbfs = -96.0f;
      if (rms > 1.0) {
        dbfs = 20.0f * log10f((float)(rms / 8388607.0));
      }

      // 6. Форматированный вывод строки
      char statBuf[64];
      snprintf(statBuf, sizeof(statBuf), "RMS: %5d | Peak: %5d | %5.1f dBFS ", (int)rms16, (int)peak16, dbfs);
      Serial.print(statBuf);

      // 7. Графический VU-метр
      printVuMeter(dbfs, 20);

      // 8. Анализ состояния и подсказки
      if (zeroCount == accumCount) {
        consecutiveZeroWindows++;
        if (consecutiveZeroWindows >= 4) { // 1 секунда сплошных нулей
          Serial.print(" [ПРЕДУПРЕЖДЕНИЕ: линия SD выдаёт только нули! Проверьте контакты SD/SCK/WS]");
        }
      } else {
        consecutiveZeroWindows = 0;
      }

      if (peak16 >= 32000) {
        Serial.print(" [ПЕРЕГРУЗКА]");
      } else if (rms16 > 1500) {
        Serial.print(" [ЗВУК]");
      }

      Serial.println();
    }

    // Сброс счетчиков на новый интервал
    accumSum = 0.0;
    accumSumSq = 0.0;
    accumPeak = 0;
    accumCount = 0;
    zeroCount = 0;
    lastPrintTime = now;
  }
}

