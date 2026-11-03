#ifndef MEDIA_CLIENT_H
#define MEDIA_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include "NetworkConfig.h"

#define EQ_BAND_COUNT 16

// ============================================================================
// Структура медиасостояния MacBook
// ============================================================================
struct MediaData {
  bool isConnected = false;
  char title[160] = "Ойнатылмай тұр";
  char artist[96] = "MacBook Air • Күтуде";
  char source[40] = "macOS";
  char state[16] = "stopped"; // "playing", "paused", "stopped"
  bool isPlaying = false;
  uint32_t position = 0;
  uint32_t duration = 0;
  uint8_t volume = 50;
};

// ============================================================================
// Структура реального спектра аудиосигнала (16 частотных диапазонов 0..100)
// ============================================================================
struct EqualizerData {
  uint8_t bands[EQ_BAND_COUNT] = {0};
  bool isActive = false;
  unsigned long lastUpdateMs = 0;
};

// Структура синхронизации времени с MacBook
struct ServerTimeData {
  uint8_t hours = 0;
  uint8_t minutes = 0;
  uint8_t seconds = 0;
  char dateStr[48] = "";
};

// Структура живой погоды, курса валют (USD/KZT) и батареи MacBook
struct LiveTelemetryData {
  int8_t tempC = 18;
  char descKz[32] = "Ашық";
  uint16_t usdKzt = 495;
  uint8_t macBattery = 100;
  bool macCharging = false;
};

// Структура результата распознавания голосовой команды от MacBook
struct VoiceResultData {
  char status[16]  = "";
  char heard[128]  = "";
  char reply[128]  = "";
  char screen[24]  = "";
  char emotion[24] = "";
  int32_t timerSec = 0;
};

class MediaClient {
private:
  WiFiClient client;
  MediaData currentMedia;
  EqualizerData currentEq;
  ServerTimeData serverTime;
  LiveTelemetryData liveTelemetry;
  VoiceResultData voiceResult;

  const char *wifiSsid;
  const char *wifiPass;
  const char *serverHost;
  uint16_t serverPort;
  const char *authToken;

  unsigned long lastWifiRetry = 0;
  unsigned long lastServerRetry = 0;
  bool wasConnected = false;
  bool hasNewData = false;
  bool hasNewVolume = false;
  bool hasNewTime = false;
  bool hasNewEqualizer = false;
  bool hasNewTelemetry = false;
  bool hasTimerDone = false;
  bool hasNewVoiceResult = false;

  char rxBuffer[CLIENT_BUFFER_SIZE];
  uint16_t rxIndex = 0;

  // Парсинг JSON-строки от сервера
  void parseJsonLine(const char *jsonStr) {
    cJSON *root = cJSON_Parse(jsonStr);
    if (!root) {
      Serial.println("[MediaClient] Ошибка разбора JSON");
      return;
    }

    cJSON *typeItem = cJSON_GetObjectItem(root, "type");
    if (typeItem && cJSON_IsString(typeItem)) {
      const char *typeVal = typeItem->valuestring;

      if (strcmp(typeVal, "equalizer") == 0) {
        cJSON *bandsArr = cJSON_GetObjectItem(root, "bands");
        if (bandsArr && cJSON_IsArray(bandsArr)) {
          int arrSize = cJSON_GetArraySize(bandsArr);
          bool anyNonZero = false;
          for (uint8_t i = 0; i < EQ_BAND_COUNT; i++) {
            int val = 0;
            if (i < arrSize) {
              cJSON *item = cJSON_GetArrayItem(bandsArr, i);
              if (item && cJSON_IsNumber(item)) {
                val = item->valueint;
              }
            }
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            currentEq.bands[i] = (uint8_t)val;
            if (val > 0) {
              anyNonZero = true;
            }
          }

          cJSON *st = cJSON_GetObjectItem(root, "state");
          if (st && cJSON_IsString(st)) {
            currentEq.isActive = anyNonZero || (strcmp(st->valuestring, "playing") == 0);
          } else {
            currentEq.isActive = anyNonZero;
          }
          currentEq.lastUpdateMs = millis();
          hasNewEqualizer = true;
        }
      }
      else if (strcmp(typeVal, "media_state") == 0) {
        // 1. Статус воспроизведения
        cJSON *st = cJSON_GetObjectItem(root, "state");
        if (st && cJSON_IsString(st)) {
          strncpy(currentMedia.state, st->valuestring, sizeof(currentMedia.state) - 1);
          currentMedia.state[sizeof(currentMedia.state) - 1] = '\0';
          currentMedia.isPlaying = (strcmp(currentMedia.state, "playing") == 0);
        }

        // 2. Название трека / видео
        cJSON *t = cJSON_GetObjectItem(root, "title");
        if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
          strncpy(currentMedia.title, t->valuestring, sizeof(currentMedia.title) - 1);
          currentMedia.title[sizeof(currentMedia.title) - 1] = '\0';
        } else if (strcmp(currentMedia.state, "stopped") == 0) {
          strncpy(currentMedia.title, "Ойнатылмай тұр", sizeof(currentMedia.title) - 1);
          currentMedia.title[sizeof(currentMedia.title) - 1] = '\0';
        }

        // 3. Исполнитель / автор канала
        cJSON *a = cJSON_GetObjectItem(root, "artist");
        if (a && cJSON_IsString(a) && strlen(a->valuestring) > 0) {
          strncpy(currentMedia.artist, a->valuestring, sizeof(currentMedia.artist) - 1);
          currentMedia.artist[sizeof(currentMedia.artist) - 1] = '\0';
        } else if (strcmp(currentMedia.state, "stopped") == 0) {
          strncpy(currentMedia.artist, "MacBook Air • Күтуде", sizeof(currentMedia.artist) - 1);
          currentMedia.artist[sizeof(currentMedia.artist) - 1] = '\0';
        }

        // 4. Источник
        cJSON *s = cJSON_GetObjectItem(root, "source");
        if (s && cJSON_IsString(s)) {
          strncpy(currentMedia.source, s->valuestring, sizeof(currentMedia.source) - 1);
          currentMedia.source[sizeof(currentMedia.source) - 1] = '\0';
        }

        // 5. Позиция и длительность (поддержка длинных видео > 1 часа)
        cJSON *pos = cJSON_GetObjectItem(root, "position");
        if (pos && cJSON_IsNumber(pos)) {
          currentMedia.position = (pos->valueint > 0) ? (uint32_t)pos->valueint : 0;
        }

        cJSON *dur = cJSON_GetObjectItem(root, "duration");
        if (dur && cJSON_IsNumber(dur)) {
          currentMedia.duration = (dur->valueint > 0) ? (uint32_t)dur->valueint : 0;
        }

        // 6. Громкость
        cJSON *vol = cJSON_GetObjectItem(root, "volume");
        if (vol && cJSON_IsNumber(vol)) {
          uint8_t newVol = (uint8_t)vol->valueint;
          if (newVol != currentMedia.volume) {
            currentMedia.volume = newVol;
            hasNewVolume = true;
          }
        }

        // 7. Точное время с MacBook Air
        cJSON *tm = cJSON_GetObjectItem(root, "time");
        if (tm && cJSON_IsObject(tm)) {
          cJSON *h = cJSON_GetObjectItem(tm, "hours");
          cJSON *m = cJSON_GetObjectItem(tm, "minutes");
          cJSON *sec = cJSON_GetObjectItem(tm, "seconds");
          if (h && m && sec) {
            serverTime.hours = (uint8_t)h->valueint;
            serverTime.minutes = (uint8_t)m->valueint;
            serverTime.seconds = (uint8_t)sec->valueint;
            hasNewTime = true;
          }
        }

        // 8. Дата
        cJSON *dt = cJSON_GetObjectItem(root, "date");
        if (dt && cJSON_IsString(dt)) {
          strncpy(serverTime.dateStr, dt->valuestring, sizeof(serverTime.dateStr) - 1);
          serverTime.dateStr[sizeof(serverTime.dateStr) - 1] = '\0';
        }

        // 9. Живая погода, курс USD/KZT и батарея MacBook
        cJSON *wObj = cJSON_GetObjectItem(root, "weather");
        if (wObj && cJSON_IsObject(wObj)) {
          cJSON *tmp = cJSON_GetObjectItem(wObj, "temp");
          cJSON *dkz = cJSON_GetObjectItem(wObj, "desc_kz");
          cJSON *usd = cJSON_GetObjectItem(wObj, "usd_kzt");
          cJSON *bat = cJSON_GetObjectItem(wObj, "mac_battery");
          cJSON *chg = cJSON_GetObjectItem(wObj, "mac_charging");

          if (tmp && cJSON_IsNumber(tmp)) {
            int8_t newTmp = (int8_t)tmp->valueint;
            if (newTmp != liveTelemetry.tempC) {
              liveTelemetry.tempC = newTmp;
              hasNewTelemetry = true;
            }
          }
          if (dkz && cJSON_IsString(dkz) && strlen(dkz->valuestring) > 0) {
            if (strcmp(liveTelemetry.descKz, dkz->valuestring) != 0) {
              strncpy(liveTelemetry.descKz, dkz->valuestring, sizeof(liveTelemetry.descKz) - 1);
              liveTelemetry.descKz[sizeof(liveTelemetry.descKz) - 1] = '\0';
              hasNewTelemetry = true;
            }
          }
          if (usd && cJSON_IsNumber(usd)) {
            uint16_t newUsd = (uint16_t)usd->valueint;
            if (newUsd != liveTelemetry.usdKzt) {
              liveTelemetry.usdKzt = newUsd;
              hasNewTelemetry = true;
            }
          }
          if (bat && cJSON_IsNumber(bat)) {
            uint8_t newBat = (uint8_t)bat->valueint;
            if (newBat != liveTelemetry.macBattery) {
              liveTelemetry.macBattery = newBat;
              hasNewTelemetry = true;
            }
          }
          if (chg) {
            bool newChg = cJSON_IsTrue(chg);
            if (newChg != liveTelemetry.macCharging) {
              liveTelemetry.macCharging = newChg;
              hasNewTelemetry = true;
            }
          }
        }

        hasNewData = true;
      }
      else if (strcmp(typeVal, "response") == 0) {
        cJSON *cmd = cJSON_GetObjectItem(root, "command");
        cJSON *succ = cJSON_GetObjectItem(root, "success");
        bool isOk = succ ? cJSON_IsTrue(succ) : false;
        Serial.printf("[MediaClient] Ответ сервера на '%s': %s\n",
                      cmd ? cmd->valuestring : "unknown",
                      isOk ? "УСПЕХ" : "ОШИБКА");

        cJSON *vol = cJSON_GetObjectItem(root, "volume");
        if (vol && cJSON_IsNumber(vol)) {
          currentMedia.volume = (uint8_t)vol->valueint;
          hasNewVolume = true;
        }
      }
      else if (strcmp(typeVal, "voice_result") == 0) {
        cJSON *st  = cJSON_GetObjectItem(root, "status");
        cJSON *hrd = cJSON_GetObjectItem(root, "heard");
        cJSON *rep = cJSON_GetObjectItem(root, "reply");
        cJSON *scr = cJSON_GetObjectItem(root, "screen");
        cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        cJSON *tmr = cJSON_GetObjectItem(root, "timer_sec");

        strncpy(voiceResult.status,  (st  && cJSON_IsString(st))  ? st->valuestring  : "ok", sizeof(voiceResult.status) - 1);
        strncpy(voiceResult.heard,   (hrd && cJSON_IsString(hrd)) ? hrd->valuestring : "",   sizeof(voiceResult.heard) - 1);
        strncpy(voiceResult.reply,   (rep && cJSON_IsString(rep)) ? rep->valuestring : "",   sizeof(voiceResult.reply) - 1);
        strncpy(voiceResult.screen,  (scr && cJSON_IsString(scr)) ? scr->valuestring : "",   sizeof(voiceResult.screen) - 1);
        strncpy(voiceResult.emotion, (emo && cJSON_IsString(emo)) ? emo->valuestring : "",   sizeof(voiceResult.emotion) - 1);
        voiceResult.timerSec = (tmr && cJSON_IsNumber(tmr)) ? tmr->valueint : 0;

        voiceResult.status[sizeof(voiceResult.status) - 1]   = '\0';
        voiceResult.heard[sizeof(voiceResult.heard) - 1]     = '\0';
        voiceResult.reply[sizeof(voiceResult.reply) - 1]     = '\0';
        voiceResult.screen[sizeof(voiceResult.screen) - 1]   = '\0';
        voiceResult.emotion[sizeof(voiceResult.emotion) - 1] = '\0';

        hasNewVoiceResult = true;
      }
      else if (strcmp(typeVal, "timer_done") == 0) {
        Serial.println("[MediaClient] Событие от MacBook: голосовой таймер завершён");
        hasTimerDone = true;
      }
    }

    cJSON_Delete(root);
  }

  void resetEqualizerOnDisconnect() {
    for (uint8_t i = 0; i < EQ_BAND_COUNT; i++) {
      currentEq.bands[i] = 0;
    }
    currentEq.isActive = false;
    hasNewEqualizer = true;
  }

public:
  MediaClient()
    : wifiSsid(WIFI_SSID), wifiPass(WIFI_PASSWORD),
      serverHost(SERVER_HOST), serverPort(SERVER_PORT), authToken(AUTH_TOKEN) {}

  void begin(const char *ssid = WIFI_SSID, const char *pass = WIFI_PASSWORD,
             const char *host = SERVER_HOST, uint16_t port = SERVER_PORT,
             const char *token = AUTH_TOKEN) {
    wifiSsid = ssid;
    wifiPass = pass;
    serverHost = host;
    serverPort = port;
    authToken = token;

    rxIndex = 0;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    if (wifiSsid && strlen(wifiSsid) > 0 && strcmp(wifiSsid, "YOUR_WIFI_SSID") != 0) {
      Serial.printf("[MediaClient] Подключение к Wi-Fi: %s...\n", wifiSsid);
      WiFi.begin(wifiSsid, wifiPass);
    } else {
      Serial.println("[MediaClient] Внимание: Wi-Fi SSID не настроен. Задайте имя сети в NetworkConfig.h");
    }
  }

  bool isWifiConnected() const {
    return (WiFi.status() == WL_CONNECTED);
  }

  bool isServerConnected() {
    return client.connected();
  }

  const MediaData& getMedia() const {
    return currentMedia;
  }

  const EqualizerData& getEqualizer() const {
    return currentEq;
  }

  const ServerTimeData& getServerTime() const {
    return serverTime;
  }

  const LiveTelemetryData& getTelemetry() const {
    return liveTelemetry;
  }

  const VoiceResultData& getVoiceResult() const {
    return voiceResult;
  }

  bool popNewTelemetryFlag() {
    bool res = hasNewTelemetry;
    hasNewTelemetry = false;
    return res;
  }

  bool popTimerDoneFlag() {
    bool res = hasTimerDone;
    hasTimerDone = false;
    return res;
  }

  bool popNewVoiceResultFlag() {
    bool res = hasNewVoiceResult;
    hasNewVoiceResult = false;
    return res;
  }

  bool popNewDataFlag() {
    bool res = hasNewData;
    hasNewData = false;
    return res;
  }

  bool popNewEqualizerFlag() {
    bool res = hasNewEqualizer;
    hasNewEqualizer = false;
    return res;
  }

  bool popNewVolumeFlag() {
    bool res = hasNewVolume;
    hasNewVolume = false;
    return res;
  }

  bool popNewTimeFlag() {
    bool res = hasNewTime;
    hasNewTime = false;
    return res;
  }

  // Неблокирующий цикл обслуживания сетевого соединения и приема данных
  void loop() {
    unsigned long now = millis();

    // 1. Проверка Wi-Fi
    if (WiFi.status() != WL_CONNECTED) {
      if (currentMedia.isConnected || wasConnected) {
        currentMedia.isConnected = false;
        wasConnected = false;
        resetEqualizerOnDisconnect();
        hasNewData = true;
      }
      if (now - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
        lastWifiRetry = now;
        if (wifiSsid && strlen(wifiSsid) > 0 && strcmp(wifiSsid, "YOUR_WIFI_SSID") != 0) {
          Serial.println("[MediaClient] Повторная попытка соединения с Wi-Fi...");
          WiFi.reconnect();
        }
      }
      return;
    }

    // 2. Проверка TCP соединения с MacBook
    if (!client.connected()) {
      currentMedia.isConnected = false;
      if (wasConnected) {
        wasConnected = false;
        Serial.println("[MediaClient] Связь с MacBook потеряна!");
        resetEqualizerOnDisconnect();
        hasNewData = true;
      }

      if (now - lastServerRetry >= SERVER_RETRY_INTERVAL_MS) {
        lastServerRetry = now;
        Serial.printf("[MediaClient] Подключение к MacBook (%s:%u)...\n", serverHost, serverPort);
        if (client.connect(serverHost, serverPort)) {
          client.setNoDelay(true);
          wasConnected = true;
          currentMedia.isConnected = true;
          rxIndex = 0;
          Serial.println("[MediaClient] Соединение с MacBook установлено!");

          // Отправляем токен авторизации
          cJSON *auth = cJSON_CreateObject();
          cJSON_AddStringToObject(auth, "type", "auth");
          cJSON_AddStringToObject(auth, "token", authToken);
          char *out = cJSON_PrintUnformatted(auth);
          client.print(out);
          client.print("\n");
          cJSON_Delete(auth);
          free(out);

          // Запрашиваем актуальный статус
          sendGetStatus();
        } else {
          Serial.println("[MediaClient] Не удалось подключиться к серверу MacBook.");
        }
      }
      return;
    }

    currentMedia.isConnected = true;

    // 3. Чтение входящих байтов из сокета (с ограничением числа пакетов за итерацию для отзывчивости UI)
    uint8_t linesProcessed = 0;
    while (client.available() > 0 && linesProcessed < 12) {
      char c = (char)client.read();
      if (c == '\n') {
        rxBuffer[rxIndex] = '\0';
        if (rxIndex > 0) {
          parseJsonLine(rxBuffer);
          linesProcessed++;
        }
        rxIndex = 0;
      } else if (c != '\r') {
        if (rxIndex < sizeof(rxBuffer) - 1) {
          rxBuffer[rxIndex++] = c;
        } else {
          // Защита от переполнения буфера при поврежденной строке
          rxIndex = 0;
        }
      }
    }
  }

  // ==========================================================================
  // Отправка команд на MacBook
  // ==========================================================================
  void sendCommand(const char *cmd, int value = -999) {
    if (!client.connected()) {
      Serial.printf("[MediaClient] Ошибка: нет связи с MacBook для команды '%s'\n", cmd);
      return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "command");
    cJSON_AddStringToObject(root, "command", cmd);
    if (value != -999) {
      cJSON_AddNumberToObject(root, "value", value);
    }

    char *out = cJSON_PrintUnformatted(root);
    client.print(out);
    client.print("\n");
    cJSON_Delete(root);
    free(out);

    Serial.printf("[MediaClient] Отправлена команда '%s' (значение: %d)\n", cmd, value);
  }

  void sendPlayPause() {
    sendCommand("PLAY_PAUSE");
  }

  void sendNextTrack() {
    sendCommand("NEXT_TRACK");
  }

  void sendPrevTrack() {
    sendCommand("PREVIOUS_TRACK");
  }

  void sendSeekForward(int seconds = 10) {
    sendCommand("SEEK_FORWARD", seconds);
  }

  void sendSeekBackward(int seconds = 10) {
    sendCommand("SEEK_BACKWARD", seconds);
  }

  void sendSeekTo(int positionSeconds) {
    sendCommand("SEEK_TO", positionSeconds);
  }

  void sendSetVolume(uint8_t volumePercent) {
    sendCommand("SET_VOLUME", volumePercent);
  }

  void sendGetStatus() {
    sendCommand("GET_STATUS");
  }
};

#endif // MEDIA_CLIENT_H
