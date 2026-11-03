#ifndef MIC_INPUT_H
#define MIC_INPUT_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESP_I2S.h>
#include <esp_err.h>
#include <math.h>
#include "NetworkConfig.h"

// ============================================================================
// Модуль микрофонного входа INMP441 (I2S RX, контроллер I2S_NUM_0)
// Работает в отдельной задаче FreeRTOS (Core 0), не блокируя главный цикл UI.
// Захватывает I2S на 16 кГц и формирует сглаженный поток 8 кГц (16 бит PCM)
// с кольцевой предзаписью 250 мс. Занимает всего 44 КБ RAM, оставляя
// максимум свободной памяти для стека Wi-Fi и буфера RobotEyes.
// ============================================================================

static const int8_t PIN_MIC_BCLK = 4;
static const int8_t PIN_MIC_WS   = 5;
static const int8_t PIN_MIC_DIN  = 6;

class MicInput {
public:
  static const uint32_t I2S_SAMPLE_RATE       = 16000; // Частота опроса I2S: 16 кГц
  static const uint32_t STT_SAMPLE_RATE       = 8000;  // Частота потока для STT: 8 кГц (экономия RAM и Wi-Fi в 2 раза)
  static const size_t   CHUNK_SAMPLES         = 512;   // 512 сэмплов I2S (~32 мс на один DMA-блок)
  static const size_t   RING_BUFFER_SIZE      = 2048;  // Кольцевой буфер предзаписи (256 мс при 8 кГц = 4 КБ)
  static const size_t   PREROLL_SAMPLES       = 1800;  // 225 мс предзаписи до срабатывания порога голоса
  static const size_t   MAX_UTTERANCE_SAMPLES = 34000; // Макс. длина фразы: 4.25 сек при 8 кГц (68 КБ)
  static const size_t   MIN_UTTERANCE_SAMPLES = 3600;  // Мин. длина фразы (включая предзапись): ~450 мс
  static const uint32_t PRINT_INTERVAL_MS     = 250;   // Интервал вывода телеметрии в Serial (4 раза/сек)

  static const int32_t  VOICE_RMS_THRESH      = 260;   // Порог старта записи речи (фон тишины ~10..80)
  static const int32_t  VOICE_SILENCE_THRESH  = 190;   // Порог тишины для завершения фразы
  static const int32_t  MIN_VALID_SPEECH_RMS  = 320;   // Мин. пиковый RMS внутри фразы для отправки в STT
  static const uint32_t SILENCE_HOLD_MS       = 850;   // Пауза после речи для отправки фразы (850 мс)
  static const uint32_t VOICE_UI_HOLD_MS      = 400;   // Удержание индикатора активности голоса в UI
  static const uint16_t VOICE_HTTP_PORT       = 8766;  // Порт HTTP-шлюза MacMediaService

private:
  I2SClass i2sMic;
  TaskHandle_t taskHandle;

  int32_t dmaRawBuffer[CHUNK_SAMPLES];
  int16_t pcmRingBuffer[RING_BUFFER_SIZE];
  volatile size_t ringWriteIdx;

  // Буфер записи голосовой команды (44 КБ)
  int16_t *utteranceBuffer;
  size_t   utteranceCount;

  volatile bool     initialized;
  volatile int32_t  latestRms16;
  volatile int32_t  latestPeak16;
  volatile float    latestDbfs;
  volatile float    smoothLevel01;
  volatile bool     zeroWarning;
  volatile bool     voiceActive;
  volatile bool     recordingUtterance;
  volatile bool     sendingToServer;
  volatile bool     forceRecordTrigger;
  volatile uint32_t lastVoiceDetectedMs;
  volatile uint32_t cooldownUntilMs;

  bool     serialLogEnabled;
  uint32_t lastSerialPrintMs;

  static void taskEntry(void *param) {
    MicInput *self = static_cast<MicInput *>(param);
    self->readerLoop();
  }

  // Отправка записанного PCM16-буфера (8000 Гц) на MacBook по HTTP POST (на Core 0)
  void sendUtteranceToMac(const int16_t *pcmData, size_t sampleCount) {
    if (!pcmData || sampleCount == 0) return;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[VOICE STT] Пропуск отправки: Wi-Fi не подключён.");
      return;
    }

    size_t totalBytes = sampleCount * sizeof(int16_t);
    Serial.printf("[VOICE STT] Отправка голоса на MacBook (%s:%u): %u сэмплов (%u байт, %.2f сек)...\n",
                  SERVER_HOST, VOICE_HTTP_PORT, (unsigned)sampleCount, (unsigned)totalBytes,
                  (float)sampleCount / (float)STT_SAMPLE_RATE);

    WiFiClient httpClient;
    httpClient.setTimeout(5);
    if (!httpClient.connect(SERVER_HOST, VOICE_HTTP_PORT)) {
      Serial.println("[VOICE STT] Ошибка подключения к HTTP-шлюзу MacBook (:8766)");
      return;
    }

    httpClient.setNoDelay(true);
    httpClient.printf("POST /api/voice HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n",
                      SERVER_HOST, VOICE_HTTP_PORT, (unsigned)totalBytes);

    const uint8_t *bytePtr = reinterpret_cast<const uint8_t *>(pcmData);
    size_t remaining = totalBytes;
    const size_t chunkSize = 4096;

    while (remaining > 0 && httpClient.connected()) {
      size_t toSend = (remaining > chunkSize) ? chunkSize : remaining;
      size_t written = httpClient.write(bytePtr, toSend);
      if (written == 0) break;
      bytePtr   += written;
      remaining -= written;
      vTaskDelay(1);
    }

    uint32_t waitStart = millis();
    while (httpClient.connected() && (millis() - waitStart < 4500)) {
      if (httpClient.available()) {
        String respLine = httpClient.readStringUntil('\n');
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    httpClient.stop();
  }

  void readerLoop() {
    double   accumSum   = 0.0;
    double   accumSumSq = 0.0;
    int32_t  accumPeak  = 0;
    uint32_t accumCount = 0;
    uint32_t zeroCount  = 0;
    uint32_t consecutiveZeroWindows = 0;
    uint32_t windowStartMs = millis();
    float    dcEstimate = 0.0f;

    bool     isRec = false;
    uint32_t lastSpeechWindowMs = 0;
    uint32_t recStartMs = 0;
    int32_t  maxUtteranceRms = 0;
    int32_t  prevSample16 = 0;

    while (true) {
      if (!initialized) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      size_t bytesToRead = sizeof(dmaRawBuffer);
      size_t bytesRead = i2sMic.readBytes((char *)dmaRawBuffer, bytesToRead);

      if (bytesRead != bytesToRead || bytesRead == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      size_t samplesCount = bytesRead / sizeof(int32_t);
      size_t writePos = ringWriteIdx;

      for (size_t i = 0; i < samplesCount; ++i) {
        int32_t raw = dmaRawBuffer[i];
        if (raw == 0) zeroCount++;

        int32_t val24 = raw >> 8;
        accumSum   += (double)val24;
        accumSumSq += ((double)val24 * (double)val24);

        int32_t absVal = (val24 < 0) ? -val24 : val24;
        if (absVal > accumPeak) accumPeak = absVal;
        accumCount++;

        // Фильтрация DC и цифровое усиление x3
        dcEstimate = 0.995f * dcEstimate + 0.005f * (float)val24;
        int32_t ac24 = val24 - (int32_t)dcEstimate;
        int32_t pcm16 = (ac24 * 3) >> 8;
        if (pcm16 > 32767)  pcm16 = 32767;
        if (pcm16 < -32768) pcm16 = -32768;

        // Децимация 2:1 (16 кГц -> 8 кГц) с антиалиасинговым усреднением соседних отсчётов
        if ((i & 1) == 1) {
          int16_t decimated = (int16_t)((prevSample16 + pcm16) >> 1);
          pcmRingBuffer[writePos] = decimated;
          writePos = (writePos + 1) % RING_BUFFER_SIZE;

          if (isRec && utteranceBuffer && utteranceCount < MAX_UTTERANCE_SAMPLES) {
            utteranceBuffer[utteranceCount++] = decimated;
          }
        } else {
          prevSample16 = pcm16;
        }
      }

      ringWriteIdx = writePos;

      uint32_t now = millis();
      if (now - windowStartMs >= 64 && accumCount > 0) {
        double meanDC = accumSum / accumCount;
        double variance = (accumSumSq / accumCount) - (meanDC * meanDC);
        if (variance < 0.0) variance = 0.0;
        double rms = sqrt(variance);

        int32_t rms16  = (int32_t)(rms / 256.0);
        int32_t peak16 = (int32_t)(accumPeak / 256.0);
        if (rms16 > 32767)  rms16 = 32767;
        if (peak16 > 32767) peak16 = 32767;

        float dbfs = -96.0f;
        if (rms > 1.0) {
          dbfs = 20.0f * log10f((float)(rms / 8388607.0));
        }

        float norm = 0.0f;
        const float minDb = -56.0f;
        const float maxDb = -10.0f;
        if (dbfs > minDb) {
          norm = (dbfs - minDb) / (maxDb - minDb);
          if (norm > 1.0f) norm = 1.0f;
        }

        float curSmooth = smoothLevel01;
        if (norm >= curSmooth) {
          curSmooth += (norm - curSmooth) * 0.65f;
        } else {
          curSmooth += (norm - curSmooth) * 0.25f;
        }
        if (curSmooth < 0.01f) curSmooth = 0.0f;

        if (zeroCount == accumCount) {
          consecutiveZeroWindows++;
          if (consecutiveZeroWindows >= 15) zeroWarning = true;
        } else {
          consecutiveZeroWindows = 0;
          zeroWarning = false;
        }

        if (rms16 >= VOICE_RMS_THRESH && !zeroWarning) {
          lastVoiceDetectedMs = now;
          voiceActive = true;
        } else if (now - lastVoiceDetectedMs >= VOICE_UI_HOLD_MS) {
          voiceActive = false;
        }

        // --------------------------------------------------------------------
        // Автомат записи голосовой команды (VAD + Pre-roll 225 мс)
        // --------------------------------------------------------------------
        if (!isRec && utteranceBuffer && now >= cooldownUntilMs) {
          if ((rms16 >= VOICE_RMS_THRESH && !zeroWarning) || forceRecordTrigger) {
            forceRecordTrigger = false;
            isRec = true;
            recordingUtterance = true;
            recStartMs = now;
            lastSpeechWindowMs = now;
            maxUtteranceRms = rms16;
            utteranceCount = 0;

            size_t startIdx = (writePos + RING_BUFFER_SIZE - PREROLL_SAMPLES) % RING_BUFFER_SIZE;
            for (size_t k = 0; k < PREROLL_SAMPLES; ++k) {
              utteranceBuffer[utteranceCount++] = pcmRingBuffer[(startIdx + k) % RING_BUFFER_SIZE];
            }
            Serial.println("\n[VOICE VAD] >>> Начало записи голосовой команды...");
          }
        } else if (isRec) {
          if (rms16 > maxUtteranceRms) {
            maxUtteranceRms = rms16;
          }
          if (rms16 >= VOICE_SILENCE_THRESH) {
            lastSpeechWindowMs = now;
          }

          bool silenceTimeout = (now - lastSpeechWindowMs >= SILENCE_HOLD_MS);
          bool bufferFull     = (utteranceCount >= MAX_UTTERANCE_SAMPLES);

          if (silenceTimeout || bufferFull) {
            isRec = false;
            recordingUtterance = false;

            uint32_t speechDurationMs = lastSpeechWindowMs - recStartMs;
            if (utteranceCount >= MIN_UTTERANCE_SAMPLES &&
                maxUtteranceRms >= MIN_VALID_SPEECH_RMS &&
                speechDurationMs >= 200) {
              sendingToServer = true;
              sendUtteranceToMac(utteranceBuffer, utteranceCount);
              sendingToServer = false;
              cooldownUntilMs = millis() + 800;
            } else {
              Serial.println("[VOICE VAD] Короткий шум проигнорирован.");
            }
            utteranceCount = 0;
          }
        }

        latestRms16   = rms16;
        latestPeak16  = peak16;
        latestDbfs    = dbfs;
        smoothLevel01 = curSmooth;

        accumSum   = 0.0;
        accumSumSq = 0.0;
        accumPeak  = 0;
        accumCount = 0;
        zeroCount  = 0;
        windowStartMs = now;
      }

      vTaskDelay(1);
    }
  }

  void printVuMeter(float dbfs, int barWidth = 20) {
    const float minDb = -60.0f;
    const float maxDb = 0.0f;
    int filled = 0;
    if (dbfs > minDb) {
      float norm = (dbfs - minDb) / (maxDb - minDb);
      if (norm > 1.0f) norm = 1.0f;
      filled = (int)(norm * barWidth);
    }
    Serial.print("[");
    for (int i = 0; i < barWidth; ++i) {
      Serial.print((i < filled) ? "#" : "-");
    }
    Serial.print("]");
  }

public:
  MicInput()
    : i2sMic(I2S_NUM_0),
      taskHandle(nullptr),
      ringWriteIdx(0),
      utteranceBuffer(nullptr),
      utteranceCount(0),
      initialized(false),
      latestRms16(0),
      latestPeak16(0),
      latestDbfs(-96.0f),
      smoothLevel01(0.0f),
      zeroWarning(false),
      voiceActive(false),
      recordingUtterance(false),
      sendingToServer(false),
      forceRecordTrigger(false),
      lastVoiceDetectedMs(0),
      cooldownUntilMs(0),
      serialLogEnabled(true),
      lastSerialPrintMs(0) {
    memset(dmaRawBuffer, 0, sizeof(dmaRawBuffer));
    memset(pcmRingBuffer, 0, sizeof(pcmRingBuffer));
  }

  bool begin() {
    Serial.println("--------------------------------------------------");
    Serial.println("[MIC] Инициализация микрофона INMP441 (I2S_NUM_0)...");
    Serial.printf("[MIC] Пины: SCK(BCLK)=GPIO%d, WS(LRCLK)=GPIO%d, SD(DIN)=GPIO%d, L/R=GND\n",
                  PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_DIN);

    if (!utteranceBuffer) {
      utteranceBuffer = static_cast<int16_t *>(malloc(MAX_UTTERANCE_SAMPLES * sizeof(int16_t)));
      Serial.printf("[MIC OK] Буфер голосовых команд (44 КБ) выделен. Свободно Heap: %u байт\n",
                    (unsigned)ESP.getFreeHeap());
    }

    i2sMic.setPins(PIN_MIC_BCLK, PIN_MIC_WS, -1, PIN_MIC_DIN);
    i2sMic.setTimeout(50);

    if (!i2sMic.begin(I2S_MODE_STD, I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
      esp_err_t err = (esp_err_t)i2sMic.lastError();
      Serial.printf("[MIC ERROR] Не удалось инициализировать I2S RX! Ошибка: %s (0x%X)\n",
                    esp_err_to_name(err), err);
      initialized = false;
      return false;
    }

    initialized = true;
    Serial.println("[MIC OK] Шина I2S RX успешно запущена (16000 Гц -> STT 8000 Гц).");

    if (taskHandle == nullptr) {
      xTaskCreatePinnedToCore(taskEntry, "MicReaderTask", 6144, this, 2, &taskHandle, 0);
      Serial.println("[MIC OK] Фоновая задача чтения и STT-отправки запущена на Core 0.");
    }
    Serial.println("--------------------------------------------------");
    return true;
  }

  void loop() {
    if (!initialized || !serialLogEnabled) return;

    uint32_t now = millis();
    if (now - lastSerialPrintMs >= PRINT_INTERVAL_MS) {
      lastSerialPrintMs = now;

      int32_t rms16  = latestRms16;
      int32_t peak16 = latestPeak16;
      float   dbfs   = latestDbfs;
      bool    zWarn  = zeroWarning;

      char statBuf[64];
      snprintf(statBuf, sizeof(statBuf), "[MIC] RMS: %5d | Peak: %5d | %5.1f dBFS ",
               (int)rms16, (int)peak16, dbfs);
      Serial.print(statBuf);
      printVuMeter(dbfs, 20);

      if (zWarn) {
        Serial.print(" [ВНИМАНИЕ: линия SD выдаёт 0! Проверьте контакты GPIO 4/5/6 и GND]");
      } else if (sendingToServer) {
        Serial.print(" [ОТПРАВКА В STT...]");
      } else if (recordingUtterance) {
        Serial.print(" [ЗАПИСЬ КОМАНДЫ...]");
      } else if (rms16 >= VOICE_RMS_THRESH) {
        Serial.print(" [ГОЛОС]");
      }
      Serial.println();
    }
  }

  void triggerManualRecord() { forceRecordTrigger = true; }

  bool isInitialized() const { return initialized; }
  int32_t getRms16() const { return latestRms16; }
  int32_t getPeak16() const { return latestPeak16; }
  float getDbfs() const { return latestDbfs; }
  float getLevel01() const { return smoothLevel01; }
  bool isVoiceActive() const { return voiceActive || recordingUtterance; }
  bool isRecordingUtterance() const { return recordingUtterance; }
  bool isSendingToServer() const { return sendingToServer; }
  bool isZeroSignalWarning() const { return zeroWarning; }

  void setSerialLogEnabled(bool en) { serialLogEnabled = en; }
  bool isSerialLogEnabled() const { return serialLogEnabled; }
};

#endif // MIC_INPUT_H
