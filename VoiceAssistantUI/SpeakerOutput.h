#ifndef SPEAKER_OUTPUT_H
#define SPEAKER_OUTPUT_H

#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_err.h>
#include <math.h>

// ============================================================================
// Модуль аудиовыхода MAX98357A (I2S TX, независимый контроллер I2S_NUM_1)
// Работает параллельно с микрофоном INMP441 (I2S_NUM_0) на отдельных GPIO:
//   VIN  -> 5V
//   GND  -> GND
//   BCLK -> GPIO 17
//   LRC  -> GPIO 18
//   DIN  -> GPIO 7
// ============================================================================

static const int8_t PIN_AMP_BCLK = 17;
static const int8_t PIN_AMP_WS   = 18;
static const int8_t PIN_AMP_DOUT = 7;

class SpeakerOutput {
public:
  static const uint32_t SAMPLE_RATE   = 16000;
  static const size_t   CHUNK_SAMPLES = 256;

private:
  I2SClass i2sAmp;
  bool     initialized;
  int16_t  stereoBuffer[CHUNK_SAMPLES * 2];

public:
  SpeakerOutput()
    : i2sAmp(I2S_NUM_1),
      initialized(false) {
    memset(stereoBuffer, 0, sizeof(stereoBuffer));
  }

  bool begin() {
    Serial.println("--------------------------------------------------");
    Serial.println("[AMP] Инициализация усилителя MAX98357A (I2S_NUM_1)...");
    Serial.printf("[AMP] Пины: BCLK=GPIO%d, LRC(WS)=GPIO%d, DIN(DOUT)=GPIO%d\n",
                  PIN_AMP_BCLK, PIN_AMP_WS, PIN_AMP_DOUT);

    i2sAmp.setPins(PIN_AMP_BCLK, PIN_AMP_WS, PIN_AMP_DOUT);

    if (!i2sAmp.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
      esp_err_t err = (esp_err_t)i2sAmp.lastError();
      Serial.printf("[AMP ERROR] Не удалось инициализировать I2S TX (I2S_NUM_1)! Ошибка: %s (0x%X)\n",
                    esp_err_to_name(err), err);
      initialized = false;
      return false;
    }

    // Отправляем короткий буфер тишины после старта
    memset(stereoBuffer, 0, sizeof(stereoBuffer));
    i2sAmp.write((const uint8_t *)stereoBuffer, sizeof(stereoBuffer));

    initialized = true;
    Serial.println("[AMP OK] Шина I2S TX (I2S_NUM_1) успешно запущена (16000 Гц, 16-bit STEREO L=R).");
    Serial.println("--------------------------------------------------");
    return true;
  }

  bool isInitialized() const { return initialized; }

  // Воспроизведение плавного тона заданной частоты и длительности (с защитой от щелчков fade-in/out)
  void playTone(uint32_t freqHz = 440, uint32_t durationMs = 1000, int16_t amplitude = 9000) {
    if (!initialized) {
      Serial.println("[AMP WARN] Усилитель I2S_NUM_1 не инициализирован!");
      return;
    }

    Serial.printf("[AMP] Воспроизведение тона %u Гц (%u мс, ампл. %d)...\n",
                  freqHz, durationMs, (int)amplitude);

    const uint32_t totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    const uint32_t fadeSamples  = (SAMPLE_RATE * 25) / 1000; // 25 мс плавный фронт/спад
    const float    phaseInc     = 2.0f * (float)M_PI * (float)freqHz / (float)SAMPLE_RATE;
    float          phase        = 0.0f;

    uint32_t remaining = totalSamples;
    while (remaining > 0) {
      size_t count = (remaining > CHUNK_SAMPLES) ? CHUNK_SAMPLES : remaining;

      for (size_t i = 0; i < count; ++i) {
        uint32_t idx = totalSamples - remaining + i;
        float env = 1.0f;
        if (idx < fadeSamples) {
          env = (float)idx / (float)fadeSamples;
        } else if (idx >= totalSamples - fadeSamples) {
          env = (float)(totalSamples - idx) / (float)fadeSamples;
        }

        int16_t sampleVal = (int16_t)((float)amplitude * env * sinf(phase));
        phase += phaseInc;
        if (phase >= 2.0f * (float)M_PI) {
          phase -= 2.0f * (float)M_PI;
        }

        stereoBuffer[i * 2]     = sampleVal;
        stereoBuffer[i * 2 + 1] = sampleVal;
      }

      i2sAmp.write((const uint8_t *)stereoBuffer, count * 2 * sizeof(int16_t));
      remaining -= count;
    }

    // Завершаем тишиной, чтобы не оставалось смещения на выходе ЦАП
    memset(stereoBuffer, 0, sizeof(stereoBuffer));
    i2sAmp.write((const uint8_t *)stereoBuffer, sizeof(stereoBuffer));
    Serial.println("[AMP] Воспроизведение тона завершено.");
  }

  // Короткий приятный двухтональный сигнал готовности системы (C5 -> E5)
  void playStartupChime() {
    if (!initialized) return;
    playTone(523, 140, 7000); // До второй октавы (C5)
    delay(30);
    playTone(659, 200, 7500); // Ми второй октавы (E5)
  }

  // Плавный частотно-модулированный чирп (глиссандо в стиле R2-D2 / Cozmo)
  void playSweep(uint32_t startFreqHz, uint32_t endFreqHz, uint32_t durationMs = 90, int16_t amplitude = 6500) {
    if (!initialized || durationMs == 0) return;

    const uint32_t totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    if (totalSamples == 0) return;
    const uint32_t fadeSamples = (SAMPLE_RATE * 12) / 1000; // 12 мс мягкий фронт/спад
    float phase = 0.0f;

    uint32_t remaining = totalSamples;
    while (remaining > 0) {
      size_t count = (remaining > CHUNK_SAMPLES) ? CHUNK_SAMPLES : remaining;
      for (size_t i = 0; i < count; ++i) {
        uint32_t idx = totalSamples - remaining + i;
        float tNorm = (float)idx / (float)totalSamples;
        float curFreq = (float)startFreqHz + ((float)endFreqHz - (float)startFreqHz) * tNorm;

        float env = 1.0f;
        if (idx < fadeSamples && fadeSamples > 0) {
          env = (float)idx / (float)fadeSamples;
        } else if (idx >= totalSamples - fadeSamples && fadeSamples > 0) {
          env = (float)(totalSamples - idx) / (float)fadeSamples;
        }

        int16_t sampleVal = (int16_t)((float)amplitude * env * sinf(phase));
        phase += 2.0f * (float)M_PI * curFreq / (float)SAMPLE_RATE;
        if (phase >= 2.0f * (float)M_PI) {
          phase -= 2.0f * (float)M_PI;
        }

        stereoBuffer[i * 2]     = sampleVal;
        stereoBuffer[i * 2 + 1] = sampleVal;
      }
      i2sAmp.write((const uint8_t *)stereoBuffer, count * 2 * sizeof(int16_t));
      remaining -= count;
    }

    memset(stereoBuffer, 0, sizeof(stereoBuffer));
    i2sAmp.write((const uint8_t *)stereoBuffer, sizeof(stereoBuffer));
  }

  // Эмоция THINKING: короткий мягкий «бип-буп» подтверждения приёма голосовой команды
  void playThinkingChirp() {
    if (!initialized) return;
    playSweep(540, 780, 55, 5500);
    delay(15);
    playTone(880, 50, 5500);
  }

  // Эмоция HAPPY: радостный восходящий перелив R2-D2 / Cozmo
  void playHappyChime() {
    if (!initialized) return;
    playTone(587, 55, 6200);       // D5
    delay(12);
    playTone(784, 55, 6500);       // G5
    delay(12);
    playSweep(880, 1320, 85, 6800); // A5 -> E6 чирп
  }

  // Эмоция CONFUSED: вопросительный изогнутый чирп («хмм?»)
  void playConfusedChirp() {
    if (!initialized) return;
    playSweep(480, 690, 75, 5800);
    delay(15);
    playSweep(620, 390, 90, 5500);
  }

  // Эмоция SLEEPY: сонный нисходящий вздох при засыпании
  void playSleepChirp() {
    if (!initialized) return;
    playSweep(660, 330, 140, 4800);
  }

  // Эмоция WAKE: бодрый чирп пробуждения
  void playWakeChirp() {
    if (!initialized) return;
    playSweep(380, 860, 95, 6200);
  }

  // Звуковой сигнал завершения голосового таймера (громкий 3-тональный перезвон x3)
  void playTimerAlarm() {
    if (!initialized) return;
    for (int k = 0; k < 3; k++) {
      playTone(784, 120, 11000);  // G5
      delay(30);
      playTone(1046, 120, 11500); // C6
      delay(30);
      playTone(1318, 180, 12000); // E6
      delay(120);
    }
  }
};

#endif // SPEAKER_OUTPUT_H

