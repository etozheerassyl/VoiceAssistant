#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_err.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Конфигурация подключения усилителя MAX98357A к ESP32-S3
// -----------------------------------------------------------------------------
// Используются строго назначенные выводы:
//   VIN     -> 5V (или 3.3V, если на 5V нет питания от USB)
//   GND     -> GND
//   BCLK    -> GPIO15 (I2S Bit Clock / SCK)
//   LRC     -> GPIO16 (I2S Word Select / WS / LRCLK)
//   DIN     -> GPIO17 (I2S Data Out от ESP32 к DIN усилителя)
//   SD      -> 3.3V (или оставить неподключённым, если на плате есть подтяжка)
//   + / -   -> Динамик
//
// TFT-дисплей временно отключён.
// Микрофон INMP441 подключён к GPIO 4, 5, 6 (в данном тесте не задействован).
// -----------------------------------------------------------------------------
const int8_t PIN_I2S_BCLK = 15;
const int8_t PIN_I2S_WS   = 16;
const int8_t PIN_I2S_DOUT = 17; // Подключается к DIN на плате MAX98357A

// -----------------------------------------------------------------------------
// Параметры аудио
// -----------------------------------------------------------------------------
const uint32_t SAMPLE_RATE        = 16000; // Частота дискретизации 16 кГц
const uint32_t TONE_FREQ_HZ       = 440;   // Частота тона: 440 Гц (нота Ля первой октавы / A4)
const uint32_t TONE_DURATION_MS   = 2000;  // Длительность звучания: 2 секунды
const uint32_t PAUSE_DURATION_MS  = 1000;  // Длительность паузы: 1 секунда
const int16_t  AMPLITUDE          = 10000; // Чёткая слышимая амплитуда (~30.5% от 32767, безопасный уровень без перегруза)
const size_t   CHUNK_SAMPLES      = 256;   // Порция на канал (256 сэмплов * 2 канала = 512 значений = 1024 байта)
const uint32_t FADE_MS            = 25;    // Плавное нарастание и затухание (25 мс) для исключения щелчков

// Вычисление количества сэмплов по времени
const uint32_t TONE_TOTAL_SAMPLES  = (SAMPLE_RATE * TONE_DURATION_MS) / 1000;
const uint32_t PAUSE_TOTAL_SAMPLES = (SAMPLE_RATE * PAUSE_DURATION_MS) / 1000;
const uint32_t FADE_SAMPLES        = (SAMPLE_RATE * FADE_MS) / 1000;

// Экземпляр I2S шины (ESP32 Arduino Core 3.x)
I2SClass i2s;

// Буфер стерео фреймов (L + R) для гарантированной работы с любым режимом MAX98357A (Left / Right / Mix)
int16_t stereoBuffer[CHUNK_SAMPLES * 2];

// Фаза синусоиды для непрерывной генерации
float phase = 0.0f;
const float phaseInc = 2.0f * (float)M_PI * (float)TONE_FREQ_HZ / (float)SAMPLE_RATE;

// Счётчик циклов воспроизведения
uint32_t cycleCount = 0;

void setup() {
  Serial.begin(115200);
  delay(1500); // Ожидание подключения Serial Monitor

  Serial.println();
  Serial.println("==================================================");
  Serial.println("    ESP32-S3 Voice Assistant - Speaker Test");
  Serial.println("    Core: ESP32 Arduino Core 3.x (ESP_I2S TX Driver)");
  Serial.println("==================================================");
  Serial.println("Усилитель: MAX98357A (монофонический I2S ЦАП/усилитель класса D)");
  Serial.println("Подключение:");
  Serial.println("  VIN         -> 5V (или 3.3V)");
  Serial.println("  GND         -> GND");
  Serial.printf("  BCLK        -> GPIO %d\n", PIN_I2S_BCLK);
  Serial.printf("  LRC (WS)    -> GPIO %d\n", PIN_I2S_WS);
  Serial.printf("  DIN (DOUT)  -> GPIO %d\n", PIN_I2S_DOUT);
  Serial.println("  SD          -> 3.3V (активация усилителя, если не распаян pull-up)");
  Serial.println("  Выходы +/-  -> Динамик");
  Serial.println("Параметры теста:");
  Serial.printf("  Частота дискретизации: %u Гц\n", SAMPLE_RATE);
  Serial.printf("  Частота тона:          %u Гц (нота A4)\n", TONE_FREQ_HZ);
  Serial.printf("  Длительность тона:     %u мс (~2 сек)\n", TONE_DURATION_MS);
  Serial.printf("  Длительность паузы:    %u мс (~1 сек)\n", PAUSE_DURATION_MS);
  Serial.printf("  Амплитуда сигнала:     %d / 32767 (~30%%, безопасный уровень)\n", AMPLITUDE);
  Serial.println("  Режим I2S:             STEREO (L=R дублирование для 100% совместимости с SD_MODE)");
  Serial.println("--------------------------------------------------");

  // Настройка выводов: bclk, ws, dout, din, mclk
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);

  // Инициализация I2S TX в режиме STEREO с дублированием L=R.
  // Это гарантирует передачу звука и в левый, и в правый слоты фрейма I2S,
  // поэтому усилитель воспроизведет сигнал при любой аппаратной конфигурации пина SD_MODE.
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    esp_err_t err = (esp_err_t)i2s.lastError();
    Serial.printf("[FATAL ERROR] Не удалось инициализировать I2S TX! Ошибка: %s (код 0x%X)\n", esp_err_to_name(err), err);
    Serial.println("[HINT] Проверьте корректность распиновки GPIO 15, 16, 17.");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("[OK] I2S TX шина успешно инициализирована.");
  Serial.println("[INFO] Запуск периодического воспроизведения тестового тона...");
  Serial.println("--------------------------------------------------");
}

void loop() {
  cycleCount++;

  // --- Фаза 1: Воспроизведение тестового тона 440 Гц (~2 секунды) ---
  Serial.printf("[Цикл #%u] [ЗВУК]  Воспроизведение 440 Гц (2 сек, ампл. %d)...", cycleCount, AMPLITUDE);
  uint32_t toneStartMs = millis();

  uint32_t toneSamplesRemaining = TONE_TOTAL_SAMPLES;
  while (toneSamplesRemaining > 0) {
    size_t samplesToGen = (toneSamplesRemaining > CHUNK_SAMPLES) ? CHUNK_SAMPLES : toneSamplesRemaining;

    for (size_t i = 0; i < samplesToGen; ++i) {
      uint32_t currentSampleIdx = TONE_TOTAL_SAMPLES - toneSamplesRemaining + i;

      // Плавное нарастание (fade-in) в начале и затухание (fade-out) в конце для исключения щелчков
      float envelope = 1.0f;
      if (currentSampleIdx < FADE_SAMPLES) {
        envelope = (float)currentSampleIdx / (float)FADE_SAMPLES;
      } else if (currentSampleIdx >= TONE_TOTAL_SAMPLES - FADE_SAMPLES) {
        envelope = (float)(TONE_TOTAL_SAMPLES - currentSampleIdx) / (float)FADE_SAMPLES;
      }

      int16_t sampleVal = (int16_t)(AMPLITUDE * envelope * sinf(phase));
      phase += phaseInc;
      if (phase >= 2.0f * (float)M_PI) {
        phase -= 2.0f * (float)M_PI;
      }

      // Дублируем сэмпл в оба слота: левый и правый
      stereoBuffer[i * 2]     = sampleVal;
      stereoBuffer[i * 2 + 1] = sampleVal;
    }

    size_t bytesToWrite = samplesToGen * 2 * sizeof(int16_t);
    size_t written = i2s.write((const uint8_t *)stereoBuffer, bytesToWrite);

    if (written != bytesToWrite) {
      esp_err_t err = (esp_err_t)i2s.lastError();
      Serial.printf("\n[ERROR] Ошибка записи I2S TX: отправлено %u из %u байт! Ошибка: %s (0x%X)\n",
                    (unsigned)written, (unsigned)bytesToWrite, esp_err_to_name(err), err);
    }

    toneSamplesRemaining -= samplesToGen;
  }

  uint32_t toneActualMs = millis() - toneStartMs;
  Serial.printf(" готово (%u мс)\n", toneActualMs);

  // --- Фаза 2: Пауза (~1 секунда тишины) ---
  Serial.printf("[Цикл #%u] [ПАУЗА] Тишина (1 сек)...", cycleCount);
  uint32_t pauseStartMs = millis();

  memset(stereoBuffer, 0, sizeof(stereoBuffer));
  uint32_t pauseSamplesRemaining = PAUSE_TOTAL_SAMPLES;
  while (pauseSamplesRemaining > 0) {
    size_t samplesToSend = (pauseSamplesRemaining > CHUNK_SAMPLES) ? CHUNK_SAMPLES : pauseSamplesRemaining;
    size_t bytesToWrite = samplesToSend * 2 * sizeof(int16_t);

    i2s.write((const uint8_t *)stereoBuffer, bytesToWrite);
    pauseSamplesRemaining -= samplesToSend;
  }

  uint32_t pauseActualMs = millis() - pauseStartMs;
  Serial.printf(" готово (%u мс)\n", pauseActualMs);
}
