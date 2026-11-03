#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"
#include "UIComponents.h"
#include "NetworkConfig.h"
#include "MediaClient.h"
#include "ScreenHome.h"
#include "ScreenMusic.h"
#include "ScreenVideo.h"
#include "ScreenAssistant.h"
#include "OverlayVolume.h"
#include "OverlayPhrase.h"
#include "RobotEyes.h"
#include "MicInput.h"
#include "SpeakerOutput.h"

// ============================================================================
// Конфигурация аппаратного подключения дисплея ILI9341 к ESP32-S3
// Подтвержденная распиновка: CS=10, DC=9, RESET=14, MOSI=11, SCK=12
// ============================================================================
#define TFT_PINOUT_TABLE_MODE 1

#define TFT_CS   10
#if TFT_PINOUT_TABLE_MODE
  #define TFT_DC   9
  #define TFT_RST  14
#else
  #define TFT_DC   14
  #define TFT_RST  9
#endif
#define TFT_MOSI 11
#define TFT_SCK  12
#define TFT_BL   8   // Резервный выход 3.3V (HIGH) для пина LED подсветки экрана (если оба пина 3V3 заняты)

// Создание объекта дисплея с аппаратным SPI
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// ============================================================================
// Состояние интерфейса, экраны, сетевой клиент, микрофон и усилитель
// ============================================================================
ScreenHome      screenHome;
ScreenMusic     screenMusic;
ScreenVideo     screenVideo;
ScreenAssistant screenAssistant;
OverlayVolume   overlayVolume;
OverlayPhrase   overlayPhrase;
MediaClient     mediaClient;
RobotEyes       robotEyes;
MicInput        micInput;
SpeakerOutput   speakerOut;

ScreenID currentScreen = SCREEN_HOME;
bool autoDemoMode = false; // По умолчанию авто-тур выключен для работы с реальными данными
bool autoVoiceListenMode = true; // Автопереход READY <-> LISTENING по голосу с микрофона INMP441

// Предыдущее медиа-состояние для умного автопереключения экранов
char prevMediaTitle[160] = "";
char prevMediaSource[40] = "";
bool prevIsPlaying = false;
bool prevServerOk = false;

// ============================================================================
// Данные времени и погоды
// ============================================================================
struct ClockState {
  uint8_t hours = 14;
  uint8_t minutes = 35;
  uint8_t seconds = 48;
} sysClock;

struct WeatherState {
  int8_t tempC = 18;
  const char *cityName = "Алматы";
  const char *description = "Ашық күн"; // Ясно
  bool wifiConnected = true;
} sysWeather;

// Таймеры неблокирующего цикла millis()
unsigned long lastClockTick   = 0;
unsigned long lastEqTick      = 0;
unsigned long lastAnimTick    = 0;
unsigned long lastMicUiTick   = 0;
unsigned long lastDemoStep    = 0;
uint8_t       demoStepCounter = 0;

// Кэш состояния индикатора микрофона в шапке для дифференциальной отрисовки
int16_t prevHeaderMicStep = -1;
bool    prevHeaderMicVoice = false;
bool    prevHeaderMicWarn  = false;
bool    prevVoiceActiveState = false;
bool    prevSendingState = false;
unsigned long speakingHoldUntilMs = 0;
unsigned long lastActivityMs = 0;
int32_t activeTimerRemainingSec = 0;

// ============================================================================
// Прототипы функций
// ============================================================================
void switchScreen(ScreenID newScreen);
void redrawCurrentScreen();
void showFaceView();
void hideFaceView();
void updateClockLogic();
void handleSerialCommands();
void handleNetworkUpdates();
void runAutoDemoTour();
void printHelpMenu();

// ============================================================================
// Показ и скрытие независимого режима «Живые глаза робота» (Face View)
// ============================================================================
void showFaceView() {
  overlayPhrase.dismiss();
  robotEyes.show(tft);
  if (overlayVolume.isVisible()) {
    overlayVolume.draw(tft);
  }
}

void hideFaceView() {
  if (robotEyes.isVisible()) {
    overlayPhrase.dismiss();
    robotEyes.hide();
    switchScreen(currentScreen);
  }
}

// ============================================================================
// Переключение активного экрана
// ============================================================================
void switchScreen(ScreenID newScreen) {
  overlayPhrase.dismiss();
  robotEyes.hide();
  currentScreen = newScreen;

  // Форматируем строку времени для шапки
  char timeHeader[8];
  snprintf(timeHeader, sizeof(timeHeader), "%02u:%02u", sysClock.hours, sysClock.minutes);
  bool wifiOk = mediaClient.isWifiConnected();
  bool macOk = mediaClient.isServerConnected();

  // Отрисовка выбранного экрана
  switch (currentScreen) {
    case SCREEN_HOME:
      screenHome.drawStatic(tft, timeHeader, wifiOk, macOk);
      screenHome.updateClock(tft, sysClock.hours, sysClock.minutes, sysClock.seconds);
      break;

    case SCREEN_MUSIC:
      screenMusic.drawStatic(tft, timeHeader, wifiOk);
      break;

    case SCREEN_VIDEO:
      screenVideo.drawStatic(tft, timeHeader, wifiOk);
      break;

    case SCREEN_ASSISTANT:
      screenAssistant.setMicTelemetry(
        micInput.getLevel01(),
        micInput.getRms16(),
        micInput.getDbfs(),
        micInput.isInitialized(),
        micInput.isZeroSignalWarning(),
        micInput.isVoiceActive()
      );
      screenAssistant.drawStatic(tft, timeHeader, wifiOk);
      break;

    default:
      break;
  }

  // Принудительно отрисовываем индикатор микрофона INMP441 в шапке
  prevHeaderMicStep = -1;
  drawHeaderMicStatus(
    tft,
    micInput.getLevel01(),
    micInput.isVoiceActive(),
    micInput.isInitialized(),
    micInput.isZeroSignalWarning()
  );

  // Если оверлей громкости был открыт во время смены экрана — перерисовываем поверх
  if (overlayVolume.isVisible()) {
    overlayVolume.draw(tft);
  }
}

// Полная перерисовка текущего экрана (используется при закрытии оверлея)
void redrawCurrentScreen() {
  if (robotEyes.isVisible()) {
    robotEyes.show(tft);
  } else {
    switchScreen(currentScreen);
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   ESP32-S3 Voice Assistant - Full System v2.3");
  Serial.println("   Дисплей: ILI9341 (SPI: CS=10, DC=" + String(TFT_DC) + ", RST=" + String(TFT_RST) + ", MOSI=11, SCK=12)");
  Serial.println("   Микрофон: INMP441 (I2S_NUM_0: SCK=4, WS=5, SD=6)");
  Serial.println("   Усилитель: MAX98357A (I2S_NUM_1: BCLK=17, LRC=18, DIN=7)");
  Serial.println("   Разрешение: 320x240 (Горизонтальный режим)");
  Serial.println("==================================================");

  // 0. Включаем GPIO 8 в HIGH (3.3V) как питание управляющего входа LED подсветки TFT
  // (если оба штатных пина 3V3 заняты под TFT VCC и INMP441 VDD)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // 1. Инициализация аппаратной шины SPI (SCK=12, MISO=-1, MOSI=11, SS=10)
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  // 2. Инициализация дисплея на частоте 10 МГц
  tft.begin(10000000);

  // 3. Установка альбомной (горизонтальной) ориентации 320x240 и отключение автопереноса GFX
  tft.setRotation(1);
  tft.setTextWrap(false);

  // 4. Инициализация микрофона INMP441 (неблокирующая задача FreeRTOS на Core 0)
  micInput.begin();

  // 4a. Инициализация усилителя MAX98357A (независимая шина I2S_NUM_1)
  if (speakerOut.begin()) {
    speakerOut.playStartupChime();
  }

  // 5. Инициализация независимого модуля анимированных глаз робота (Double Buffering)
  robotEyes.init(tft);

  // 6. Первоначальная отрисовка Главного экрана
  switchScreen(SCREEN_HOME);

  // 7. Запуск неблокирующего сетевого клиента
  mediaClient.begin();

  // 8. Инициализация генератора случайных фраз
  overlayPhrase.init(millis());
  lastActivityMs = millis();

  // 9. Вывод справки по командам в Serial
  printHelpMenu();
}

// ============================================================================
// LOOP: Неблокирующий цикл событий
// ============================================================================
void loop() {
  unsigned long now = millis();

  // 0. Неблокирующее обслуживание телеметрии микрофона INMP441
  micInput.loop();

  // 1. Обслуживание сетевого обмена с MacBook (Wi-Fi и TCP JSON)
  mediaClient.loop();
  handleNetworkUpdates();

  // 2. Обработка команд из Serial Monitor
  handleSerialCommands();

  // 2a. Обновление телеметрии микрофона в UI (каждые 60 мс, без блокировки и мерцания)
  if (now - lastMicUiTick >= 60) {
    lastMicUiTick = now;

    float   lvl     = micInput.getLevel01();
    int32_t rms     = micInput.getRms16();
    float   dbfs    = micInput.getDbfs();
    bool    mOk     = micInput.isInitialized();
    bool    zWarn   = micInput.isZeroSignalWarning();
    bool    vAct    = micInput.isVoiceActive();
    bool    sending = micInput.isSendingToServer();

    // Передаем данные на экран ассистента
    screenAssistant.setMicTelemetry(lvl, rms, dbfs, mOk, zWarn, vAct);

    // Если закончилось время показа ответа (STATE_SPEAKING) — возвращаемся в STATE_READY
    if (speakingHoldUntilMs > 0 && now >= speakingHoldUntilMs) {
      speakingHoldUntilMs = 0;
      if (currentScreen == SCREEN_ASSISTANT && screenAssistant.getState() == STATE_SPEAKING) {
        screenAssistant.setState(STATE_READY);
        if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
          screenAssistant.drawStateCard(tft);
        }
      }
      if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_SPEAKING) {
        robotEyes.setEyeState(EYE_STATE_IDLE);
      }
    }

    // При отправке записанной фразы на MacBook — сразу включаем режим THINKING (ОЙЛАНУДА...)
    if (sending && !prevSendingState) {
      lastActivityMs = now;
      speakingHoldUntilMs = 0;
      speakerOut.playThinkingChirp();
      if (currentScreen == SCREEN_ASSISTANT) {
        screenAssistant.setState(STATE_THINKING);
        if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
          screenAssistant.drawStateCard(tft);
        }
      }
      if (robotEyes.isVisible()) {
        robotEyes.setEyeState(EYE_STATE_THINKING);
      }
    }
    prevSendingState = sending;

    // Автоматическое переключение состояния READY <-> LISTENING при наличии голоса
    if (autoVoiceListenMode && !autoDemoMode && !sending && speakingHoldUntilMs == 0) {
      if (vAct && !prevVoiceActiveState) {
        lastActivityMs = now;
        if (currentScreen == SCREEN_ASSISTANT && screenAssistant.getState() == STATE_READY) {
          screenAssistant.setState(STATE_LISTENING);
          if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
            screenAssistant.drawStateCard(tft);
          }
        }
        // В режиме сна EYE_STATE_SLEEPY глаза НЕ просыпаются от случайного шума в комнате
        if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_IDLE) {
          robotEyes.setEyeState(EYE_STATE_LISTENING);
        }
      } else if (!vAct && prevVoiceActiveState) {
        lastActivityMs = now;
        if (currentScreen == SCREEN_ASSISTANT && screenAssistant.getState() == STATE_LISTENING) {
          screenAssistant.setState(STATE_READY);
          if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
            screenAssistant.drawStateCard(tft);
          }
        }
        if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_LISTENING) {
          robotEyes.setEyeState(EYE_STATE_IDLE);
        }
      }
      prevVoiceActiveState = vAct;
    }

    // Автоматический переход глаз в сонный режим (EYE_STATE_SLEEPY) после 90 сек тишины
    if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_IDLE &&
        !mediaClient.getEqualizer().isActive && !vAct && !sending && speakingHoldUntilMs == 0) {
      if (now - lastActivityMs >= 90000) {
        robotEyes.setEyeState(EYE_STATE_SLEEPY);
        speakerOut.playSleepChirp();
      }
    }

    // Дифференциальное обновление мини-шкалы микрофона в верхней шапке
    if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
      int16_t curStep = (int16_t)(lvl * 24.0f + 0.5f);
      if (curStep != prevHeaderMicStep || vAct != prevHeaderMicVoice || zWarn != prevHeaderMicWarn) {
        drawHeaderMicStatus(tft, lvl, vAct, mOk, zWarn);
        prevHeaderMicStep  = curStep;
        prevHeaderMicVoice = vAct;
        prevHeaderMicWarn  = zWarn;
      }
    }
  }

  // 3. Системные часы: обновление каждую секунду
  if (now - lastClockTick >= 1000) {
    lastClockTick = now;
    updateClockLogic();
  }

  // 3a. Если активен независимый экран «Живые глаза» (Face View) — обновляем его
  if (robotEyes.isVisible() && !overlayVolume.isVisible()) {
    robotEyes.update(tft);
  }

  // 4. Анимация реактивного 16-полосного эквалайзера (активна на экране музыки, каждые 30 мс ~ 33 FPS)
  if (!robotEyes.isVisible() && currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
    if (now - lastEqTick >= 30) {
      lastEqTick = now;
      screenMusic.updateEqualizer(tft);
    }
  }

  // 5. Анимация ядра ассистента (активна на экране ассистента, каждые 70 мс)
  if (!robotEyes.isVisible() && currentScreen == SCREEN_ASSISTANT && !overlayVolume.isVisible()) {
    if (now - lastAnimTick >= 70) {
      lastAnimTick = now;
      screenAssistant.updateAnimation(tft);
    }
  }

  // 6. Проверка таймера автоскрытия оверлея громкости
  if (overlayVolume.checkTimeout()) {
    redrawCurrentScreen();
  }

  // 6a. Проверка таймера автоскрытия всплывающего баннера голосового ответа
  bool phraseJustShown = false;
  if (!overlayVolume.isVisible() && overlayPhrase.loopUpdate(now, phraseJustShown)) {
    if (robotEyes.isVisible()) {
      overlayPhrase.clearEyesArea(tft);
    } else {
      redrawCurrentScreen();
    }
  }

  // 7. Автоматический демонстрационный тур (если включен пользователем)
  if (autoDemoMode) {
    runAutoDemoTour();
  }
}

// ============================================================================
// Обработка данных от MacBook
// ============================================================================
void handleNetworkUpdates() {
  // 000. Событие завершения таймера от MacBook
  if (mediaClient.popTimerDoneFlag()) {
    activeTimerRemainingSec = 0;
    screenHome.setTimerRemaining(0);
    Serial.println("[TIMER] Голосовой таймер завершён (событие от MacBook)!");
    speakerOut.playTimerAlarm();
    if (robotEyes.isVisible()) {
      if (robotEyes.getEyeState() == EYE_STATE_SLEEPY) {
        speakerOut.playWakeChirp();
      }
      robotEyes.setEyeState(EYE_STATE_HAPPY);
      speakingHoldUntilMs = millis() + 6500;
    }
    overlayPhrase.showVoiceReply("ТАЙМЕР", "Уақыт бітті! • Время вышло!", millis(), 7000);
    if (!overlayVolume.isVisible()) {
      overlayPhrase.draw(tft, robotEyes.isVisible());
    }
    if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible()) {
      screenHome.drawCards(tft, mediaClient.isServerConnected());
    }
  }

  // 00. Обработка результата распознавания голосовой команды (STT + ответ помощника)
  if (mediaClient.popNewVoiceResultFlag()) {
    const VoiceResultData &vr = mediaClient.getVoiceResult();
    if (strcmp(vr.status, "ignored") == 0 || strcmp(vr.status, "echo_ignored") == 0) {
      // Фраза без триггер-слова «Дос» или эхо динамиков MacBook — экран и динамики не беспокоим
      return;
    }
    Serial.printf("\n[VOICE AI] Распознано: \"%s\" | Ответ: \"%s\" | Экран: \"%s\"\n",
                  vr.heard, vr.reply, vr.screen);

    if (strcmp(vr.status, "empty") == 0) {
      // Речь не распознана — мягко возвращаемся в состояние готовности
      if (currentScreen == SCREEN_ASSISTANT && screenAssistant.getState() == STATE_THINKING) {
        screenAssistant.setState(STATE_READY);
        if (!robotEyes.isVisible() && !overlayVolume.isVisible()) {
          screenAssistant.drawStateCard(tft);
        }
      }
      if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_THINKING) {
        robotEyes.setEyeState(EYE_STATE_CONFUSED);
        speakingHoldUntilMs = millis() + 2500;
      }
      speakerOut.playConfusedChirp();
    } else if (strcmp(vr.status, "thinking") == 0) {
      // Промежуточный статус: речь распознана, MacBook ищет в интернете и генерирует ответ LLM
      lastActivityMs = millis();
      screenAssistant.setVoiceDialog(vr.heard, vr.reply);
      if (robotEyes.isVisible()) {
        if (robotEyes.getEyeState() == EYE_STATE_SLEEPY) {
          speakerOut.playWakeChirp();
        }
        robotEyes.setEyeState(EYE_STATE_THINKING);
        speakingHoldUntilMs = millis() + 8000;
      }
      if (!robotEyes.isVisible() && currentScreen == SCREEN_ASSISTANT) {
        screenAssistant.setState(STATE_THINKING);
        speakingHoldUntilMs = millis() + 8000;
        if (!overlayVolume.isVisible()) {
          screenAssistant.drawStateCard(tft);
        }
      } else {
        overlayPhrase.showVoiceReply(vr.heard, vr.reply, millis(), 8000);
        if (!overlayVolume.isVisible()) {
          overlayPhrase.draw(tft, robotEyes.isVisible());
        }
      }
    } else {
      lastActivityMs = millis();
      // Сохраняем распознанный диалог для карточки ассистента
      screenAssistant.setVoiceDialog(vr.heard, vr.reply);

      bool wasSleeping = (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_SLEEPY);
      bool isGoingToSleep = (strcmp(vr.emotion, "sleepy") == 0 || strcmp(vr.screen, "sleep_eyes") == 0);
      if (wasSleeping && !isGoingToSleep) {
        speakerOut.playWakeChirp();
      }

      // Обработка голосового таймера обратного отсчёта
      if (vr.timerSec > 0) {
        activeTimerRemainingSec = vr.timerSec;
        screenHome.setTimerRemaining(activeTimerRemainingSec);
      } else if (vr.timerSec < 0) {
        activeTimerRemainingSec = 0;
        screenHome.setTimerRemaining(0);
      }

      // Если команда переключает экран или режим живых глаз
      if (strlen(vr.screen) > 0) {
        if (strcmp(vr.screen, "eyes") == 0) {
          showFaceView();
          robotEyes.setEyeState(EYE_STATE_HAPPY);
        } else if (strcmp(vr.screen, "sleep_eyes") == 0) {
          showFaceView();
          robotEyes.setEyeState(EYE_STATE_SLEEPY);
          speakerOut.playSleepChirp();
        } else if (strcmp(vr.screen, "hide_eyes") == 0) {
          hideFaceView();
        } else if (strcmp(vr.screen, "home") == 0) {
          switchScreen(SCREEN_HOME);
        } else if (strcmp(vr.screen, "music") == 0) {
          switchScreen(SCREEN_MUSIC);
        } else if (strcmp(vr.screen, "video") == 0) {
          switchScreen(SCREEN_VIDEO);
        } else if (strcmp(vr.screen, "assistant") == 0) {
          switchScreen(SCREEN_ASSISTANT);
        }
      }

      // Звуковые эмоции R2-D2 на MAX98357A
      if (strcmp(vr.emotion, "happy") == 0) {
        speakerOut.playHappyChime();
      } else if (strcmp(vr.emotion, "confused") == 0) {
        speakerOut.playConfusedChirp();
      }

      // Устанавливаем эмоцию живых глаз, если они активны
      if (robotEyes.isVisible()) {
        if (isGoingToSleep) {
          robotEyes.setEyeState(EYE_STATE_SLEEPY);
          speakingHoldUntilMs = 0;
        } else if (strcmp(vr.emotion, "happy") == 0) {
          robotEyes.setEyeState(EYE_STATE_HAPPY);
          speakingHoldUntilMs = millis() + 6500;
        } else if (strcmp(vr.emotion, "confused") == 0) {
          robotEyes.setEyeState(EYE_STATE_CONFUSED);
          speakingHoldUntilMs = millis() + 6500;
        } else {
          robotEyes.setEyeState(EYE_STATE_SPEAKING);
          speakingHoldUntilMs = millis() + 6500;
        }
      }

      // Отображаем короткий ответ на экране!
      if (!robotEyes.isVisible() && currentScreen == SCREEN_ASSISTANT) {
        screenAssistant.setState(STATE_SPEAKING);
        speakingHoldUntilMs = millis() + 7000;
        if (!overlayVolume.isVisible()) {
          screenAssistant.drawStateCard(tft);
        }
      } else {
        // На экранах Home / Music / Video / RobotEyes показываем всплывающий баннер с ответом
        overlayPhrase.showVoiceReply(vr.heard, vr.reply, millis(), 6500);
        if (!overlayVolume.isVisible()) {
          overlayPhrase.draw(tft, robotEyes.isVisible());
        }
      }
    }
  }

  // 00a. Обновление живой погоды, курса USD/KZT и батареи MacBook
  if (mediaClient.popNewTelemetryFlag()) {
    const LiveTelemetryData &lt = mediaClient.getTelemetry();
    screenHome.setLiveTelemetry(lt.tempC, lt.descKz, lt.usdKzt, lt.macBattery, lt.macCharging);
    if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible() && !overlayPhrase.isVisible()) {
      screenHome.drawCards(tft, mediaClient.isServerConnected());
    }
  }

  // 0. Точная синхронизация времени с MacBook
  if (mediaClient.popNewTimeFlag()) {
    const ServerTimeData &st = mediaClient.getServerTime();
    bool minChanged = (sysClock.hours != st.hours || sysClock.minutes != st.minutes);
    sysClock.hours = st.hours;
    sysClock.minutes = st.minutes;
    sysClock.seconds = st.seconds;
    if (strlen(st.dateStr) > 0) {
      screenHome.setDate(st.dateStr);
    }
    if (minChanged && !robotEyes.isVisible()) {
      char timeHeader[8];
      snprintf(timeHeader, sizeof(timeHeader), "%02u:%02u", sysClock.hours, sysClock.minutes);
      updateHeaderTime(tft, timeHeader);
    }
    if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible()) {
      screenHome.updateClock(tft, sysClock.hours, sysClock.minutes, sysClock.seconds);
    }
  }

  // 0a. Приём реальных спектральных уровней 16 диапазонов эквалайзера
  if (mediaClient.popNewEqualizerFlag()) {
    const EqualizerData &eq = mediaClient.getEqualizer();
    screenMusic.setEqualizerBands(eq.bands, EQ_BAND_COUNT, eq.isActive, eq.lastUpdateMs);

    // Передаём энергию баса и высоких частот в RobotEyes для танца глаз под музыку!
    float bassAvg = (float)((int)eq.bands[0] + (int)eq.bands[1] + (int)eq.bands[2]) / 300.0f;
    float highAvg = (float)((int)eq.bands[9] + (int)eq.bands[11] + (int)eq.bands[13]) / 300.0f;
    robotEyes.setMusicEnergy(bassAvg, highAvg, eq.isActive);

    if (eq.isActive) {
      lastActivityMs = millis();
      if (robotEyes.isVisible() && robotEyes.getEyeState() == EYE_STATE_SLEEPY) {
        robotEyes.setEyeState(EYE_STATE_IDLE);
      }
    }
  }

  // 1. Проверяем изменение системной громкости
  if (mediaClient.popNewVolumeFlag()) {
    uint8_t newVol = mediaClient.getMedia().volume;
    Serial.printf("[NET] Обновлена системная громкость MacBook: %u%%\n", newVol);
    overlayVolume.setVolume(newVol);
    overlayVolume.draw(tft);
  }

  // 2. Проверяем изменение медиаданных (трек, видео, плеер, статус)
  if (mediaClient.popNewDataFlag()) {
    const MediaData &m = mediaClient.getMedia();
    bool isYouTube = (strstr(m.source, "YouTube") != nullptr);
    bool macOk = mediaClient.isServerConnected();

    Serial.printf("[NET MEDIA] Источник: %s | Статус: %s | Трек: %s | Автор: %s | Позиция: %lu/%lu с\n",
                  m.source, m.state, m.title, m.artist,
                  (unsigned long)m.position, (unsigned long)m.duration);

    // Обновляем данные музыкального экрана и экрана видео
    screenMusic.setTrackData(m.title, m.artist, m.source, m.position, m.duration, m.isPlaying, m.isConnected);
    screenVideo.setVideoData(m.title, m.artist, m.source, m.position, m.duration, m.isPlaying, m.isConnected);

    // Проверяем, изменился ли трек / источник / старт воспроизведения
    bool titleChanged = (strcmp(prevMediaTitle, m.title) != 0);
    bool sourceChanged = (strcmp(prevMediaSource, m.source) != 0);
    bool startedPlaying = (!prevIsPlaying && m.isPlaying);
    bool hasValidMedia = (strlen(m.title) > 0 &&
                          strcmp(m.title, "Ойнатылмай тұр") != 0 &&
                          strcmp(m.state, "stopped") != 0);

    strncpy(prevMediaTitle, m.title, sizeof(prevMediaTitle) - 1);
    prevMediaTitle[sizeof(prevMediaTitle) - 1] = '\0';
    strncpy(prevMediaSource, m.source, sizeof(prevMediaSource) - 1);
    prevMediaSource[sizeof(prevMediaSource) - 1] = '\0';
    prevIsPlaying = m.isPlaying;

    // Если начал играть новый трек/видео или сменился источник — автоматически открываем нужный плеер
    if (hasValidMedia && (startedPlaying || titleChanged || sourceChanged) && currentScreen != SCREEN_ASSISTANT) {
      ScreenID targetScreen = isYouTube ? SCREEN_VIDEO : SCREEN_MUSIC;
      if (currentScreen != targetScreen) {
        switchScreen(targetScreen);
        prevServerOk = macOk;
        return;
      }
    }

    // Если открыт экран Home — перерисовываем только при изменении статуса связи
    if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible()) {
      if (prevServerOk != macOk) {
        char timeHeader[8];
        snprintf(timeHeader, sizeof(timeHeader), "%02u:%02u", sysClock.hours, sysClock.minutes);
        screenHome.drawStatic(tft, timeHeader, mediaClient.isWifiConnected(), macOk);
        screenHome.updateClock(tft, sysClock.hours, sysClock.minutes, sysClock.seconds);
        if (overlayPhrase.isVisible()) {
          overlayPhrase.draw(tft, false);
        }
      }
    }
    // Если открыт музыкальный экран — дифференциально обновляем данные
    else if (!robotEyes.isVisible() && currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
      if (!overlayPhrase.isVisible()) {
        screenMusic.updateData(tft);
      }
    }
    // Если открыт экран видео — дифференциально обновляем данные
    else if (!robotEyes.isVisible() && currentScreen == SCREEN_VIDEO && !overlayVolume.isVisible()) {
      if (!overlayPhrase.isVisible()) {
        screenVideo.updateData(tft);
      }
    }

    prevServerOk = macOk;
  }
}

// ============================================================================
// Логика часов и времени
// ============================================================================
void updateClockLogic() {
  sysClock.seconds++;
  if (sysClock.seconds >= 60) {
    sysClock.seconds = 0;
    sysClock.minutes++;
    if (sysClock.minutes >= 60) {
      sysClock.minutes = 0;
      sysClock.hours = (sysClock.hours + 1) % 24;
    }

    // При смене минуты обновляем время в шапке
    if (!robotEyes.isVisible()) {
      char timeHeader[8];
      snprintf(timeHeader, sizeof(timeHeader), "%02u:%02u", sysClock.hours, sysClock.minutes);
      updateHeaderTime(tft, timeHeader);
    }
  }

  // На главном экране обновляем большие часы
  if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible()) {
    screenHome.updateClock(tft, sysClock.hours, sysClock.minutes, sysClock.seconds);
  }

  // Обратный отсчёт голосового таймера (если запущен)
  if (activeTimerRemainingSec > 0) {
    activeTimerRemainingSec--;
    screenHome.setTimerRemaining(activeTimerRemainingSec);
    if (activeTimerRemainingSec == 0) {
      Serial.println("[TIMER] Голосовой таймер завершён!");
      speakerOut.playTimerAlarm();
      if (robotEyes.isVisible()) {
        robotEyes.setEyeState(EYE_STATE_HAPPY);
        speakingHoldUntilMs = millis() + 6000;
      }
      overlayPhrase.showVoiceReply("Голосовой таймер", "ТАЙМЕР АЯҚТАЛДЫ! • Время вышло!", millis(), 6500);
      if (!overlayVolume.isVisible()) {
        overlayPhrase.draw(tft, robotEyes.isVisible());
      }
      if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible()) {
        screenHome.drawCards(tft, mediaClient.isServerConnected());
      }
    } else if (!robotEyes.isVisible() && currentScreen == SCREEN_HOME && !overlayVolume.isVisible() && !overlayPhrase.isVisible()) {
      screenHome.drawCards(tft, mediaClient.isServerConnected());
    }
  }

  // На музыкальном экране продвигаем прогресс трека (только если играет)
  if (!robotEyes.isVisible() && currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
    screenMusic.advanceTrackSecond(tft);
  }

  // На экране видео продвигаем прогресс видео (только если играет)
  if (!robotEyes.isVisible() && currentScreen == SCREEN_VIDEO && !overlayVolume.isVisible()) {
    screenVideo.advanceSecond(tft);
  }
}

// ============================================================================
// Автоматический демонстрационный тур
// ============================================================================
void runAutoDemoTour() {
  unsigned long now = millis();

  if (now - lastDemoStep >= 6000) {
    lastDemoStep = now;
    demoStepCounter = (demoStepCounter + 1) % 8;

    switch (demoStepCounter) {
      case 0:
        Serial.println("[DEMO TOUR] -> Главный экран");
        switchScreen(SCREEN_HOME);
        break;

      case 1:
        Serial.println("[DEMO TOUR] -> Регулировка громкости (+5%)");
        overlayVolume.changeVolume(+5);
        overlayVolume.draw(tft);
        break;

      case 2:
        Serial.println("[DEMO TOUR] -> Музыкальный экран");
        switchScreen(SCREEN_MUSIC);
        break;

      case 3:
        Serial.println("[DEMO TOUR] -> Экран видео (YouTube)");
        switchScreen(SCREEN_VIDEO);
        break;

      case 4:
        Serial.println("[DEMO TOUR] -> Экран ассистента: READY");
        screenAssistant.setState(STATE_READY);
        switchScreen(SCREEN_ASSISTANT);
        break;

      case 5:
        Serial.println("[DEMO TOUR] -> Ассистент: LISTENING (Слушаю)");
        screenAssistant.setState(STATE_LISTENING);
        screenAssistant.drawStateCard(tft);
        break;

      case 6:
        Serial.println("[DEMO TOUR] -> Ассистент: THINKING (Обработка)");
        screenAssistant.setState(STATE_THINKING);
        screenAssistant.drawStateCard(tft);
        break;

      case 7:
        Serial.println("[DEMO TOUR] -> Ассистент: SPEAKING (Ответ)");
        screenAssistant.setState(STATE_SPEAKING);
        screenAssistant.drawStateCard(tft);
        break;
    }
  }
}

// ============================================================================
// Обработка команд пользователя из Serial Monitor
// ============================================================================
void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') continue;

    // Любое ручное действие временно отключает авто-тур
    if (c != 'a' && c != 'A' && c != '?' && c != 'h') {
      if (autoDemoMode) {
        autoDemoMode = false;
        Serial.println("[SYSTEM] Авто-демо режим приостановлен. Ручное управление активно.");
      }
    }

    switch (c) {
      case '1':
        Serial.println("[COMMAND] Экран 1: Главный (Home)");
        switchScreen(SCREEN_HOME);
        break;

      case '2':
        Serial.println("[COMMAND] Экран 2: Музыкальный плеер (Music)");
        switchScreen(SCREEN_MUSIC);
        break;

      case '3':
        Serial.println("[COMMAND] Экран 3: Видео-плеер (YouTube / Video)");
        switchScreen(SCREEN_VIDEO);
        break;

      case '4':
        Serial.println("[COMMAND] Экран 4: Голосовой помощник (Assistant)");
        switchScreen(SCREEN_ASSISTANT);
        break;

      case '5':
        Serial.println("[COMMAND] Экран 5: Живые глаза робота (showFaceView)");
        showFaceView();
        break;

      case '0':
        Serial.println("[COMMAND] Возврат к основному интерфейсу (hideFaceView)");
        hideFaceView();
        break;

      case 'e':
      case 'E': {
        if (!robotEyes.isVisible()) {
          showFaceView();
        } else {
          uint8_t nextSt = ((uint8_t)robotEyes.getEyeState() + 1) % 7;
          robotEyes.setEyeState((EyeState)nextSt);
        }
        const char *stNames[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING", "HAPPY", "CONFUSED", "SLEEPY"};
        Serial.printf("[COMMAND] Эмоция глаз (RobotEyes): %s\n", stNames[(uint8_t)robotEyes.getEyeState()]);
        break;
      }

      case '+':
      case '=':
        overlayVolume.changeVolume(+5);
        overlayVolume.draw(tft);
        mediaClient.sendSetVolume(overlayVolume.getVolume());
        Serial.printf("[COMMAND] Громкость +5%% (отправлено на Mac: %u%%)\n", overlayVolume.getVolume());
        break;

      case '-':
      case '_':
        overlayVolume.changeVolume(-5);
        overlayVolume.draw(tft);
        mediaClient.sendSetVolume(overlayVolume.getVolume());
        Serial.printf("[COMMAND] Громкость -5%% (отправлено на Mac: %u%%)\n", overlayVolume.getVolume());
        break;

      case ' ':
      case 'p':
      case 'P':
        Serial.println("[COMMAND] Play/Pause -> отправка на MacBook");
        mediaClient.sendPlayPause();
        if (currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
          screenMusic.togglePlay();
          screenMusic.updatePlayButton(tft);
        } else if (currentScreen == SCREEN_VIDEO && !overlayVolume.isVisible()) {
          screenVideo.togglePlay();
          screenVideo.drawControls(tft);
          screenVideo.drawStatusBadge(tft);
        }
        break;

      case 'n':
      case 'N':
        Serial.println("[COMMAND] Следующий трек / видео (NEXT_TRACK)");
        mediaClient.sendNextTrack();
        break;

      case 'b':
      case 'B':
        Serial.println("[COMMAND] Предыдущий трек / видео (PREVIOUS_TRACK)");
        mediaClient.sendPrevTrack();
        break;

      case 'f':
      case 'F':
        Serial.println("[COMMAND] Перемотка вперед +10с (SEEK_FORWARD)");
        mediaClient.sendSeekForward(10);
        if (currentScreen == SCREEN_VIDEO && !overlayVolume.isVisible()) {
          screenVideo.seekLocal(+10, tft);
        } else if (currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
          screenMusic.seekLocal(+10, tft);
        }
        break;

      case 'r':
      case 'R':
        Serial.println("[COMMAND] Перемотка назад -10с (SEEK_BACKWARD)");
        mediaClient.sendSeekBackward(10);
        if (currentScreen == SCREEN_VIDEO && !overlayVolume.isVisible()) {
          screenVideo.seekLocal(-10, tft);
        } else if (currentScreen == SCREEN_MUSIC && !overlayVolume.isVisible()) {
          screenMusic.seekLocal(-10, tft);
        }
        break;

      case 'v':
      case 'V':
        Serial.println("[COMMAND] Запрос статуса (GET_STATUS)");
        mediaClient.sendGetStatus();
        break;

      case 's':
      case 'S':
        if (currentScreen != SCREEN_ASSISTANT) {
          switchScreen(SCREEN_ASSISTANT);
        }
        screenAssistant.nextState();
        screenAssistant.drawStateCard(tft);
        Serial.printf("[COMMAND] Состояние ассистента: %u\n", (uint8_t)screenAssistant.getState());
        break;

      case 'a':
      case 'A':
        autoDemoMode = !autoDemoMode;
        Serial.printf("[COMMAND] Авто-демо тур: %s\n", autoDemoMode ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН");
        lastDemoStep = millis();
        break;

      case 'm':
      case 'M':
        micInput.setSerialLogEnabled(!micInput.isSerialLogEnabled());
        Serial.printf("[COMMAND] Телеметрия микрофона INMP441 в Serial: %s\n",
                      micInput.isSerialLogEnabled() ? "ВКЛЮЧЕНА" : "ВЫКЛЮЧЕНА");
        break;

      case 'g':
      case 'G':
        Serial.println("[COMMAND] Тест динамика MAX98357A: тон 440 Гц (1 сек)");
        speakerOut.playTone(440, 1000, 9000);
        break;

      case 't':
      case 'T':
        overlayPhrase.triggerRandom(millis());
        Serial.printf("[COMMAND] Вывод случайной фразы: \"%s\"\n", overlayPhrase.getCurrentPhrase());
        overlayPhrase.draw(tft, robotEyes.isVisible());
        if (robotEyes.isVisible()) {
          robotEyes.lookAt(0, 20, 4000);
        }
        break;

      case 'h':
      case 'H':
      case '?':
        printHelpMenu();
        break;

      default:
        Serial.printf("[UNKNOWN] Неизвестная команда '%c'. Введите 'h' для справки.\n", c);
        break;
    }
  }
}

// ============================================================================
// Вывод интерактивного меню в Serial Monitor
// ============================================================================
void printHelpMenu() {
  Serial.println();
  Serial.println("==================================================");
  Serial.println("     УПРАВЛЕНИЕ ЧЕРЕЗ SERIAL MONITOR (115200)");
  Serial.println("==================================================");
  Serial.println("  '1'  - Главный экран (Часы, дата, погода, статус MacBook)");
  Serial.println("  '2'  - Музыкальный экран (Плеер, эквалайзер, трек)");
  Serial.println("  '3'  - Экран видео (YouTube, длительность, прогресс)");
  Serial.println("  '4'  - Экран голосового помощника (Ядро AI + VU-метр INMP441)");
  Serial.println("  '5'  - Режим «Живые глаза робота» (showFaceView)");
  Serial.println("  '0'  - Скрыть глаза и вернуться в UI (hideFaceView)");
  Serial.println("  'e'  - Сменить эмоцию глаз (IDLE/LISTEN/THINK/SPEAK/HAPPY/CONFUSED)");
  Serial.println("  'm'  - Вкл / Выкл лог уровня микрофона INMP441 в Serial");
  Serial.println("  'g'  - Тест динамика MAX98357A (тон 440 Гц, 1 сек)");
  Serial.println("  '+'  - Увеличить системную громкость Mac (+5%)");
  Serial.println("  '-'  - Уменьшить системную громкость Mac (-5%)");
  Serial.println("  ' '  - Пауза / Воспроизведение на MacBook");
  Serial.println("  'n'  - Следующий трек / видео");
  Serial.println("  'b'  - Предыдущий трек / видео");
  Serial.println("  'f'  - Перемотка вперед на +10 сек");
  Serial.println("  'r'  - Перемотка назад на -10 сек");
  Serial.println("  'v'  - Запросить актуальный статус воспроизведения");
  Serial.println("  's'  - Сменить состояние ассистента (READY/LISTEN/THINK/SPEAK)");
  Serial.println("  't'  - Показать случайную фразу из списка на экране");
  Serial.println("  'a'  - Вкл / Выкл демонстрационный тур");
  Serial.println("  'h'  - Показать данную справку");
  Serial.println("==================================================");
}
