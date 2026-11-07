#ifndef ROBOT_EYES_H
#define ROBOT_EYES_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SPITFT.h>
#include <math.h>

// ============================================================================
// Модуль процедурно анимированных «живых» глаз робота (Anki Vector / Cozmo / EVE)
// Стек: ESP32-S3 + Adafruit_GFX / Adafruit_ILI9341 (320x240, RGB565)
// Отрисовка без мерцания через внеэкранный буфер GFXcanvas16 (Double Buffering)
// + оптимизация грязных прямоугольников (Dirty Rect SPI Burst).
// ============================================================================

// Состояния глаз голосового ассистента.
// Примечание: если в проекте подключен UITheme.h, константы STATE_LISTENING,
// STATE_THINKING и STATE_SPEAKING уже объявлены в enum AssistantState.
// Для полной совместимости поддерживаются как имена EYE_STATE_*, так и
// прямой вызов setEyeState(STATE_IDLE / STATE_LISTENING / STATE_HAPPY и т.д.).
enum EyeState : uint8_t {
  EYE_STATE_IDLE      = 0,
  EYE_STATE_LISTENING = 1,
  EYE_STATE_THINKING  = 2,
  EYE_STATE_SPEAKING  = 3,
  EYE_STATE_HAPPY     = 4,
  EYE_STATE_CONFUSED  = 5,
  EYE_STATE_SLEEPY    = 6,

  // Короткие алиасы из технического задания (не конфликтующие с UITheme.h)
  STATE_IDLE          = EYE_STATE_IDLE,
#ifndef UI_THEME_H
  STATE_LISTENING     = EYE_STATE_LISTENING,
  STATE_THINKING      = EYE_STATE_THINKING,
  STATE_SPEAKING      = EYE_STATE_SPEAKING,
#endif
  STATE_HAPPY         = EYE_STATE_HAPPY,
  STATE_CONFUSED      = EYE_STATE_CONFUSED,
  STATE_SLEEPY        = EYE_STATE_SLEEPY
};

class RobotEyes {
private:
  // Внеэкранный буфер (Sprite / Double Buffer) для области глаз.
  // Размер 288x160 (92 КБ в SRAM ESP32-S3) полностью вмещает оба глаза
  // с учетом максимальных саккад, расширения и наклонов.
  static const int16_t CANVAS_W = 288;
  static const int16_t CANVAS_H = 160;
  static const int16_t CANVAS_X = (320 - CANVAS_W) / 2; // 16
  static const int16_t CANVAS_Y = (240 - CANVAS_H) / 2; // 40

  Adafruit_SPITFT *displayPtr;
  GFXcanvas16     *canvas;

  // Флаги активности и перерисовки
  bool visible;
  bool needsFullClear;
  bool scanlineEffect;
  bool autoStateColors;

  // Базовая настраиваемая геометрия глаз
  float baseWidth;
  float baseHeight;
  float baseRadius;
  float baseSpacing; // Расстояние между внутренними краями глаз

  // Цвета (RGB565)
  uint16_t bgColor;
  uint16_t curEyeColor;
  uint16_t targetEyeColor;

  // Палитра состояний (в стилистике Cozmo / Vector + золотой акцент проекта)
  static const uint16_t COLOR_EYE_CYAN    = 0x07FF; // Яркий неоновый циан (Cozmo/Vector Idle)
  static const uint16_t COLOR_EYE_LISTEN  = 0x2E7F; // Бирюзово-голубой фокус
  static const uint16_t COLOR_EYE_THINK   = 0xFDA0; // Янтарно-золотой (обработка LLM)
  static const uint16_t COLOR_EYE_SPEAK   = 0xFEA0; // Теплый золотой (голос TTS)
  static const uint16_t COLOR_EYE_HAPPY   = 0x2FA6; // Изумрудно-бирюзовый (радость)
  static const uint16_t COLOR_EYE_CONFUSE = 0xFD20; // Оранжево-янтарный (вопрос/ошибка)
  static const uint16_t COLOR_EYE_SLEEPY  = 0x3B9E; // Мягкий сапфирово-синий (режим сна Zzz)

  // Параметры одного глаза (текущие и целевые для LERP-физики)
  struct EyeParams {
    float w, h, r;             // Ширина, высота, радиус скругления
    float yOffset;             // Индивидуальное смещение по Y (асимметрия/наклон)
    float topLid;              // Закрытие верхнего века (0.0 .. 1.0)
    float botLid;              // Закрытие нижнего века (0.0 .. 1.0)
    float topSlope;            // Наклон верхнего века в пикселях (+ сведение к центру, - разведение)
    float botSlope;            // Наклон нижнего века в пикселях
    float happyArch;           // Вырез снизу для дуги-полумесяца радости (0.0 .. 1.0)
  };

  EyeParams curL, targetL;
  EyeParams curR, targetR;

  // Общее направление взгляда (смещение от центра в пикселях)
  float curGazeX, curGazeY;
  float targetGazeX, targetGazeY;
  bool  manualGazeActive;
  unsigned long manualGazeHoldUntil;

  // Текущее состояние FSM
  EyeState currentState;

  // Неблокирующие таймеры анимаций (millis)
  unsigned long lastFrameMs;
  unsigned long nextBlinkMs;
  unsigned long blinkStartMs;
  bool          isBlinking;
  bool          doubleBlinkPending;

  unsigned long nextSaccadeMs;
  unsigned long stateEnteredMs;

  // Внешний уровень громкости речи для STATE_SPEAKING (0.0 .. 1.0, < 0 если процедурный)
  float externalSpeechLevel;

  // Спектральная энергия музыки (бас + верхние частоты) для режима «Танцующие глаза»
  float musicBass01;
  float musicHigh01;
  bool  musicActive;

  // Грязный прямоугольник предыдущего кадра (в координатах canvas)
  int16_t prevMinX, prevMinY, prevMaxX, prevMaxY;

  // --------------------------------------------------------------------------
  // Линейная интерполяция (LERP)
  // --------------------------------------------------------------------------
  static inline float lerpVal(float current, float target, float speed) {
    return current + (target - current) * speed;
  }

  static inline int16_t clampI16(int16_t v, int16_t lo, int16_t hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
  }

  // Плавная интерполяция цвета RGB565
  static uint16_t lerpColor565(uint16_t c1, uint16_t c2, float t) {
    if (c1 == c2) return c1;
    int r1 = (c1 >> 11) & 0x1F;
    int g1 = (c1 >> 5)  & 0x3F;
    int b1 =  c1        & 0x1F;

    int r2 = (c2 >> 11) & 0x1F;
    int g2 = (c2 >> 5)  & 0x3F;
    int b2 =  c2        & 0x1F;

    int r = r1 + (int)((r2 - r1) * t);
    int g = g1 + (int)((g2 - g1) * t);
    int b = b1 + (int)((b2 - b1) * t);

    return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
  }

  // Приглушение цвета RGB565 (для сканлайнов в стиле Vector/Cozmo)
  static inline uint16_t dimColor565(uint16_t c, uint8_t num, uint8_t den) {
    uint16_t r = ((c >> 11) & 0x1F) * num / den;
    uint16_t g = ((c >> 5)  & 0x3F) * num / den;
    uint16_t b = ( c        & 0x1F) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
  }

  // Сброс целевых параметров к базовой геометрии
  void resetTargetsToBase() {
    targetL.w = baseWidth;
    targetL.h = baseHeight;
    targetL.r = baseRadius;
    targetL.yOffset   = 0.0f;
    targetL.topLid    = 0.0f;
    targetL.botLid    = 0.0f;
    targetL.topSlope  = 0.0f;
    targetL.botSlope  = 0.0f;
    targetL.happyArch = 0.0f;

    targetR = targetL;
  }

  // Планирование следующего случайного моргания
  void scheduleNextBlink(unsigned long now, uint32_t minMs = 3000, uint32_t maxMs = 6000) {
    nextBlinkMs = now + random(minMs, maxMs);
  }

  // --------------------------------------------------------------------------
  // Обновление целевых параметров в соответствии с текущим состоянием FSM
  // --------------------------------------------------------------------------
  void updateStateTargets(unsigned long now) {
    resetTargetsToBase();

    switch (currentState) {
      // ----------------------------------------------------------------------
      // 1. STATE_IDLE: медленное микро-дыхание, случайные саккады и моргания
      //    + Режим «Танцующие под бас глаза», когда играет музыка!
      // ----------------------------------------------------------------------
      case EYE_STATE_IDLE: {
        if (musicActive && (musicBass01 > 0.04f || musicHigh01 > 0.04f)) {
          // Реактивная пульсация в такт басу и верхним частотам FFT
          if (autoStateColors) {
            uint16_t beatColor = lerpColor565(COLOR_EYE_CYAN, COLOR_EYE_HAPPY, musicBass01 * 0.75f);
            if (musicBass01 > 0.65f) {
              beatColor = lerpColor565(beatColor, COLOR_EYE_SPEAK, (musicBass01 - 0.65f) * 2.2f);
            }
            targetEyeColor = beatColor;
          }

          float grooveSway = sinf((float)now * 0.0085f) * 4.5f;
          float bassBounce = -musicBass01 * 12.0f;
          float dw = musicBass01 * 11.0f - musicHigh01 * 3.0f;
          float dh = musicBass01 * 14.0f + musicHigh01 * 6.0f;

          targetL.w = baseWidth  + dw;
          targetL.h = baseHeight + dh;
          targetR.w = baseWidth  + dw;
          targetR.h = baseHeight + dh;

          targetL.yOffset = bassBounce + grooveSway * 0.6f;
          targetR.yOffset = bassBounce - grooveSway * 0.6f;
          targetL.happyArch = 0.18f + musicBass01 * 0.48f;
          targetR.happyArch = 0.18f + musicBass01 * 0.48f;

          if (!manualGazeActive) {
            targetGazeX = grooveSway * 2.2f;
            targetGazeY = bassBounce * 0.4f;
          }
        } else {
          if (autoStateColors) targetEyeColor = COLOR_EYE_CYAN;

          // Синусоидальное микро-дыхание
          float breath = sinf((float)now * 0.0026f);
          float dh = breath * 3.5f;
          float dw = breath * 1.5f;

          targetL.w = baseWidth  + dw;
          targetL.h = baseHeight + dh;
          targetR.w = baseWidth  + dw;
          targetR.h = baseHeight + dh;

          // Случайные саккады (перевод взгляда влево/вправо/вверх/по центру)
          if (!manualGazeActive && now >= nextSaccadeMs) {
            uint8_t roll = random(0, 100);
            if (roll < 42) {
              targetGazeX = 0.0f;
              targetGazeY = 0.0f;
            } else if (roll < 75) {
              targetGazeX = (float)random(-26, 27);
              targetGazeY = (float)random(-5, 6);
            } else {
              targetGazeX = (float)random(-24, 25);
              targetGazeY = (float)random(-14, 13);
            }
            nextSaccadeMs = now + random(1200, 3600);
          }
        }
        break;
      }

      // ----------------------------------------------------------------------
      // 2. STATE_LISTENING: фокус внимания, расширение по вертикали, легкий наклон
      // ----------------------------------------------------------------------
      case EYE_STATE_LISTENING: {
        if (autoStateColors) targetEyeColor = COLOR_EYE_LISTEN;

        // Легкая микро-пульсация внимания
        float focusPulse = 1.0f + 0.03f * sinf((float)now * 0.006f);

        targetL.w = baseWidth  * 1.05f;
        targetL.h = baseHeight * 1.18f * focusPulse;
        targetL.r = baseRadius * 1.15f;

        targetR.w = baseWidth  * 1.05f;
        targetR.h = baseHeight * 1.18f * focusPulse;
        targetR.r = baseRadius * 1.15f;

        // Эффект легкого наклона головы вбок (любопытный фокус)
        targetL.yOffset  = -3.0f;
        targetR.yOffset  =  3.0f;
        targetL.topSlope = -4.0f;
        targetR.topSlope =  4.0f;

        if (!manualGazeActive) {
          targetGazeX = 0.0f;
          targetGazeY = -3.0f;
        }
        break;
      }

      // ----------------------------------------------------------------------
      // 3. STATE_THINKING: глубокая концентрация, органический дрейф взгляда,
      //    когнитивное дыхание формы (squash & stretch) и наклон головы
      // ----------------------------------------------------------------------
      case EYE_STATE_THINKING: {
        float t = (float)(now - stateEnteredMs) * 0.001f;

        if (autoStateColors) {
          // Пульсация янтарного свечения мысли (нейросеть обрабатывает запрос)
          float glow = 0.5f + 0.5f * sinf(t * 3.2f);
          targetEyeColor = lerpColor565(COLOR_EYE_THINK, 0xFEE4, glow * 0.45f);
        }

        // Органический дрейф взгляда в верхней «зоне размышлений» (плавная траектория Лиссажу)
        float wanderX = 18.0f * sinf(t * 1.4f) + 6.0f * sinf(t * 2.8f);
        float wanderY = -12.0f + 4.5f * cosf(t * 1.9f) - 2.5f * fabsf(sinf(t * 0.7f));
        if (!manualGazeActive) {
          targetGazeX = wanderX;
          targetGazeY = wanderY;
        }

        // Когнитивное «дыхание»: ритмичное сжатие и расширение формы глаз
        float thinkPulse  = sinf(t * 2.6f);
        float thinkNuance = cosf(t * 1.3f);
        float hScale = 0.54f + 0.08f * thinkPulse;
        float wScale = 1.04f - 0.04f * thinkPulse;

        // Плавный наклон головы в процессе мышления
        float tilt = 4.0f * sinf(t * 1.4f);
        targetL.yOffset = -tilt;
        targetR.yOffset =  tilt;

        // Наклон век реагирует на направление блуждания взгляда
        float browReaction = 3.5f * (wanderX / 24.0f);

        // Левый глаз
        targetL.w = baseWidth  * (wScale + 0.02f * thinkNuance);
        targetL.h = baseHeight * (hScale + 0.03f * thinkNuance);
        targetL.r = baseRadius * 0.85f;
        targetL.topLid   = 0.16f + 0.05f * thinkPulse;
        targetL.botLid   = 0.12f + 0.02f * thinkNuance;
        targetL.topSlope = 6.0f + browReaction;
        targetL.botSlope = 2.0f;

        // Правый глаз
        targetR.w = baseWidth  * (wScale - 0.02f * thinkNuance);
        targetR.h = baseHeight * (hScale - 0.03f * thinkNuance);
        targetR.r = baseRadius * 0.85f;
        targetR.topLid   = 0.12f - 0.04f * thinkPulse;
        targetR.botLid   = 0.12f;
        targetR.topSlope = -5.0f + browReaction;
        targetR.botSlope = -2.0f;
        break;
      }

      // ----------------------------------------------------------------------
      // 4. STATE_SPEAKING: пульсация по высоте в такт речи + учащенное моргание
      // ----------------------------------------------------------------------
      case EYE_STATE_SPEAKING: {
        if (autoStateColors) targetEyeColor = COLOR_EYE_SPEAK;

        // Огибающая речи: либо от реального аудио-уровня, либо процедурная многочастотная
        float voiceMod = 0.0f;
        if (externalSpeechLevel >= 0.0f) {
          voiceMod = externalSpeechLevel;
        } else {
          float w1 = sinf((float)now * 0.019f);
          float w2 = cosf((float)now * 0.011f);
          float w3 = sinf((float)now * 0.031f);
          voiceMod = 0.5f + 0.5f * (0.45f * w1 + 0.35f * w2 + 0.20f * w3);
        }

        float scaleH = 0.88f + 0.30f * voiceMod; // 0.88x .. 1.18x по высоте
        float scaleW = 1.04f - 0.06f * voiceMod; // Сохранение объема (squash & stretch)

        targetL.w = baseWidth  * scaleW;
        targetL.h = baseHeight * scaleH;
        targetR.w = baseWidth  * scaleW;
        targetR.h = baseHeight * scaleH;

        // Легкая радостная дуга снизу во время речи
        targetL.happyArch = 0.18f * voiceMod;
        targetR.happyArch = 0.18f * voiceMod;

        if (!manualGazeActive && now >= nextSaccadeMs) {
          targetGazeX = (float)random(-8, 9);
          targetGazeY = (float)random(-4, 5);
          nextSaccadeMs = now + random(900, 2000);
        }
        break;
      }

      // ----------------------------------------------------------------------
      // 5. STATE_HAPPY: радостные дуги-полумесяцы (^ ^) в стиле Cozmo/Vector
      // ----------------------------------------------------------------------
      case EYE_STATE_HAPPY: {
        if (autoStateColors) targetEyeColor = COLOR_EYE_HAPPY;

        float bounce = sinf((float)now * 0.010f) * 3.0f;

        targetL.w = baseWidth  * 1.10f;
        targetL.h = baseHeight * 0.92f;
        targetL.r = baseRadius * 1.25f;
        targetL.happyArch = 0.78f;
        targetL.yOffset   = bounce;

        targetR.w = baseWidth  * 1.10f;
        targetR.h = baseHeight * 0.92f;
        targetR.r = baseRadius * 1.25f;
        targetR.happyArch = 0.78f;
        targetR.yOffset   = bounce;

        if (!manualGazeActive) {
          targetGazeX = 0.0f;
          targetGazeY = -4.0f;
        }
        break;
      }

      // ----------------------------------------------------------------------
      // 6. STATE_CONFUSED: живое недоумение (🤨), наклон головы («huh?»),
      //    вздернутая удивленная бровь с микро-твичем и подозрительный прищур
      // ----------------------------------------------------------------------
      case EYE_STATE_CONFUSED: {
        float t = (float)(now - stateEnteredMs) * 0.001f;

        if (autoStateColors) {
          float pulse = 0.5f + 0.5f * sinf(t * 2.8f);
          targetEyeColor = lerpColor565(COLOR_EYE_CONFUSE, 0xFE00, pulse * 0.35f);
        }

        // Выразительный наклон головы («huh?», как у озадаченного щенка или робота Cozmo)
        float tiltCycle  = sinf(t * 1.5f);
        float tiltAmount = tiltCycle * 6.5f;

        targetL.yOffset =  4.0f + tiltAmount;
        targetR.yOffset = -6.0f - tiltAmount;

        // Волна озадаченности и периодический микро-твич поднятой брови (каждые ~2.2 сек)
        float archPulse = sinf(t * 2.8f);
        float phaseMod  = fmodf(t, 2.2f);
        float twitch    = 0.0f;
        if (phaseMod < 0.22f) {
          twitch = 3.5f * sinf(phaseMod * (3.14159f / 0.22f));
        }

        // Правый глаз: широко распахнут от удивления, бровь вздернута вверх
        targetR.w = baseWidth  * (1.08f + 0.04f * archPulse);
        targetR.h = baseHeight * (1.18f + 0.08f * archPulse) + twitch;
        targetR.r = baseRadius * 1.25f;
        targetR.topLid   = 0.0f;
        targetR.botLid   = 0.05f;
        targetR.topSlope = -11.0f - 3.5f * archPulse - twitch * 1.5f;
        targetR.botSlope = -3.0f;

        // Левый глаз: скептически прищурен, бровь нахмурена с подозрением
        targetL.w = baseWidth  * (0.98f - 0.03f * archPulse);
        targetL.h = baseHeight * (0.48f - 0.06f * archPulse);
        targetL.r = baseRadius * 0.70f;
        targetL.topLid   = 0.28f + 0.06f * archPulse;
        targetL.botLid   = 0.22f + 0.03f * archPulse;
        targetL.topSlope = 13.0f + 3.0f * archPulse;
        targetL.botSlope = 3.0f;

        // Пытливое оглядывание по сторонам в попытке осмыслить происходящее
        if (!manualGazeActive) {
          float gazeX = 14.0f * sinf(t * 1.6f) + 5.0f * cosf(t * 3.2f);
          float gazeY = -3.0f + 4.5f * sinf(t * 2.1f);
          targetGazeX = gazeX;
          targetGazeY = gazeY;
        }
        break;
      }

      // ----------------------------------------------------------------------
      // 7. STATE_SLEEPY: сонный режим (полуприкрытые веки, глубокое дыхание и Zzz)
      // ----------------------------------------------------------------------
      case EYE_STATE_SLEEPY: {
        float t = (float)(now - stateEnteredMs) * 0.001f;
        float sleepBreath = sinf(t * 1.35f);

        if (autoStateColors) {
          targetEyeColor = lerpColor565(COLOR_EYE_SLEEPY, COLOR_EYE_CYAN, 0.25f + 0.15f * sleepBreath);
        }

        targetL.w = baseWidth  * (1.02f + 0.02f * sleepBreath);
        targetL.h = baseHeight * (0.76f + 0.05f * sleepBreath);
        targetL.r = baseRadius * 0.85f;
        targetL.topLid   = 0.54f - 0.05f * sleepBreath;
        targetL.botLid   = 0.18f;
        targetL.topSlope = -4.0f;
        targetL.yOffset  = 6.0f + 2.5f * sleepBreath;

        targetR.w = baseWidth  * (1.02f + 0.02f * sleepBreath);
        targetR.h = baseHeight * (0.76f + 0.05f * sleepBreath);
        targetR.r = baseRadius * 0.85f;
        targetR.topLid   = 0.54f - 0.05f * sleepBreath;
        targetR.botLid   = 0.18f;
        targetR.topSlope = -4.0f;
        targetR.yOffset  = 6.0f + 2.5f * sleepBreath;

        if (!manualGazeActive) {
          targetGazeX = 2.0f * sinf(t * 0.6f);
          targetGazeY = 8.0f + 2.0f * sleepBreath;
        }
        break;
      }
    }

    // Переходные эмоциональные реакции (CONFUSED / подозрение и недоумение)
    // органично длятся ~3.0 секунды, после чего глаза плавно возвращаются в IDLE
    if (currentState == EYE_STATE_CONFUSED && (now - stateEnteredMs >= 3000)) {
      setEyeState(EYE_STATE_IDLE);
    }

    // Автоматический сброс ручного взгляда lookAt через тайм-аут удержания
    if (manualGazeActive && now >= manualGazeHoldUntil) {
      manualGazeActive = false;
    }

    // ------------------------------------------------------------------------
    // Обработка неблокирующего моргания (Blink Generator)
    // ------------------------------------------------------------------------
    uint32_t blinkMin = (currentState == EYE_STATE_SPEAKING || currentState == EYE_STATE_CONFUSED) ? 1600 : 3000;
    uint32_t blinkMax = (currentState == EYE_STATE_SPEAKING || currentState == EYE_STATE_CONFUSED) ? 3200 : 6000;

    if (!isBlinking && now >= nextBlinkMs) {
      isBlinking = true;
      blinkStartMs = now;
      // В состоянии покоя или недоумения шанс живого двойного моргания (как у Cozmo)
      if (!doubleBlinkPending && (currentState == EYE_STATE_IDLE || currentState == EYE_STATE_CONFUSED) && random(0, 100) < 25) {
        doubleBlinkPending = true;
      } else {
        doubleBlinkPending = false;
      }
    }

    if (isBlinking) {
      unsigned long elapsed = now - blinkStartMs;
      const unsigned long closeDur = 55; // Смыкание век (мс)
      const unsigned long openDur  = 85; // Раскрытие век (мс)

      if (elapsed <= closeDur) {
        // Фаза быстрого закрытия
        targetL.topLid = 0.56f;
        targetL.botLid = 0.56f;
        targetR.topLid = 0.56f;
        targetR.botLid = 0.56f;
      } else if (elapsed >= closeDur + openDur) {
        isBlinking = false;
        if (doubleBlinkPending) {
          nextBlinkMs = now + 90; // Короткая пауза перед вторым морганием
        } else {
          scheduleNextBlink(now, blinkMin, blinkMax);
        }
      }
    }
  }

  // --------------------------------------------------------------------------
  // Плавная интерполяция (LERP) всех параметров к целевым значениям
  // --------------------------------------------------------------------------
  void stepPhysics() {
    const float speedGaze  = 0.28f;
    const float speedShape = 0.20f;
    const float speedLids  = isBlinking ? 0.52f : 0.22f;

    curGazeX = lerpVal(curGazeX, targetGazeX, speedGaze);
    curGazeY = lerpVal(curGazeY, targetGazeY, speedGaze);

    // Плавный переход оттенка глаз
    curEyeColor = lerpColor565(curEyeColor, targetEyeColor, 0.18f);

    // Левый глаз
    curL.w         = lerpVal(curL.w,         targetL.w,         speedShape);
    curL.h         = lerpVal(curL.h,         targetL.h,         speedShape);
    curL.r         = lerpVal(curL.r,         targetL.r,         speedShape);
    curL.yOffset   = lerpVal(curL.yOffset,   targetL.yOffset,   speedShape);
    curL.topLid    = lerpVal(curL.topLid,    targetL.topLid,    speedLids);
    curL.botLid    = lerpVal(curL.botLid,    targetL.botLid,    speedLids);
    curL.topSlope  = lerpVal(curL.topSlope,  targetL.topSlope,  speedLids);
    curL.botSlope  = lerpVal(curL.botSlope,  targetL.botSlope,  speedLids);
    curL.happyArch = lerpVal(curL.happyArch, targetL.happyArch, speedShape);

    // Правый глаз
    curR.w         = lerpVal(curR.w,         targetR.w,         speedShape);
    curR.h         = lerpVal(curR.h,         targetR.h,         speedShape);
    curR.r         = lerpVal(curR.r,         targetR.r,         speedShape);
    curR.yOffset   = lerpVal(curR.yOffset,   targetR.yOffset,   speedShape);
    curR.topLid    = lerpVal(curR.topLid,    targetR.topLid,    speedLids);
    curR.botLid    = lerpVal(curR.botLid,    targetR.botLid,    speedLids);
    curR.topSlope  = lerpVal(curR.topSlope,  targetR.topSlope,  speedLids);
    curR.botSlope  = lerpVal(curR.botSlope,  targetR.botSlope,  speedLids);
    curR.happyArch = lerpVal(curR.happyArch, targetR.happyArch, speedShape);
  }

  // --------------------------------------------------------------------------
  // Отрисовка одного процедурного глаза в буфер GFXcanvas16
  // isLeftEye: определяет направление наклона бровей (зеркальная симметрия)
  // --------------------------------------------------------------------------
  void drawSingleEyeToCanvas(float centerX, float centerY, const EyeParams &ep, bool isLeftEye) {
    int16_t w = (int16_t)(ep.w + 0.5f);
    int16_t h = (int16_t)(ep.h + 0.5f);
    if (w < 6) w = 6;
    if (h < 4) h = 4;

    int16_t x0 = (int16_t)(centerX - w * 0.5f + 0.5f);
    int16_t y0 = (int16_t)(centerY - h * 0.5f + 0.5f);

    int16_t r = (int16_t)(ep.r + 0.5f);
    int16_t maxR = ((w < h) ? w : h) / 2;
    if (r > maxR) r = maxR;
    if (r < 2) r = 2;

    // 1. Основная сплошная форма глаза (чистый скругленный прямоугольник без полос)
    canvas->fillRoundRect(x0, y0, w, h, r, curEyeColor);

    // 2. Опциональный сканлайн-эффект (по умолчанию выключен для чистой заливки;
    //    при включении строго обрезается по радиусу скругления углов)
    if (scanlineEffect && h > 10) {
      uint16_t scanColor = dimColor565(curEyeColor, 7, 8);
      for (int16_t sy = y0 + 2; sy < y0 + h - 2; sy += 3) {
        int16_t inset = 1;
        int16_t dy = 0;
        if (sy < y0 + r) {
          dy = (y0 + r) - sy;
        } else if (sy >= y0 + h - r) {
          dy = sy - (y0 + h - r - 1);
        }
        if (dy > 0) {
          int32_t sq = (int32_t)r * r - (int32_t)dy * dy;
          int16_t dx = (sq > 0) ? (int16_t)sqrtf((float)sq) : 0;
          inset = r - dx + 1;
        }
        int16_t lineW = w - 2 * inset;
        if (lineW > 0) {
          canvas->drawFastHLine(x0 + inset, sy, lineW, scanColor);
        }
      }
    }

    // 3. Маска счастливой дуги-полумесяца снизу (^ ^)
    if (ep.happyArch > 0.02f) {
      int16_t archR = (int16_t)(w * 0.68f);
      // При увеличении happyArch маскирующий круг поднимается снизу вверх
      int16_t archCY = y0 + h + archR - (int16_t)(ep.happyArch * h * 0.68f);
      int16_t archCX = x0 + w / 2;
      canvas->fillCircle(archCX, archCY, archR, bgColor);
    }

    // 4. Наклонные маски верхнего и нижнего века (эмоции, прищур и моргание)
    // Для левого глаза dx=0 — внешний край, dx=w-1 — внутренний край (к переносице).
    // Для правого глаза dx=0 — внутренний край, dx=w-1 — внешний край.
    bool hasTopMask = (ep.topLid > 0.005f) || (fabsf(ep.topSlope) > 0.4f);
    bool hasBotMask = (ep.botLid > 0.005f) || (fabsf(ep.botSlope) > 0.4f);

    if (hasTopMask || hasBotMask) {
      float invW = (w > 1) ? (1.0f / (float)(w - 1)) : 0.0f;
      float baseTopCut = ep.topLid * (float)h;
      float baseBotCut = ep.botLid * (float)h;

      for (int16_t dx = 0; dx < w; dx++) {
        // tNorm: от -0.5 (внешний край) до +0.5 (внутренний край у переносицы)
        float tNorm = isLeftEye ? ((float)dx * invW - 0.5f) : (0.5f - (float)dx * invW);

        if (hasTopMask) {
          int16_t cutTop = (int16_t)(baseTopCut + tNorm * ep.topSlope + 0.5f);
          if (cutTop > 0) {
            if (cutTop > h + 2) cutTop = h + 2;
            canvas->drawFastVLine(x0 + dx, y0 - 1, cutTop + 1, bgColor);
          }
        }

        if (hasBotMask) {
          int16_t cutBot = (int16_t)(baseBotCut + tNorm * ep.botSlope + 0.5f);
          if (cutBot > 0) {
            if (cutBot > h + 2) cutBot = h + 2;
            canvas->drawFastVLine(x0 + dx, y0 + h - cutBot, cutBot + 1, bgColor);
          }
        }
      }
    }
  }

  // Отрисовка всплывающих символов Zzz в режиме сна (EYE_STATE_SLEEPY)
  void drawSleepZzzToCanvas(unsigned long now) {
    if (!canvas) return;
    float t = (float)(now - stateEnteredMs) * 0.001f;
    for (int i = 0; i < 3; i++) {
      float phase = fmodf(t * 0.65f + (float)i * 0.33f, 1.0f);
      int16_t zx = 224 + i * 15 + (int16_t)(sinf(t * 2.2f + i) * 3.0f);
      int16_t zy = 54 - (int16_t)(phase * 42.0f);
      int16_t sz = 7 + i * 3;
      uint16_t zCol = (phase < 0.75f) ? COLOR_EYE_CYAN : COLOR_EYE_SLEEPY;
      // Горизонтальные и диагональная линии буквы Z (толщиной 2px)
      canvas->drawFastHLine(zx, zy, sz, zCol);
      canvas->drawFastHLine(zx, zy + 1, sz, zCol);
      canvas->drawLine(zx + sz - 1, zy + 1, zx, zy + sz - 1, zCol);
      canvas->drawLine(zx + sz - 2, zy + 1, zx, zy + sz - 2, zCol);
      canvas->drawFastHLine(zx, zy + sz - 1, sz, zCol);
      canvas->drawFastHLine(zx, zy + sz, sz, zCol);
    }
  }

  // --------------------------------------------------------------------------
  // Пакетная отправка обновленной подобласти из GFXcanvas16 в дисплей по SPI
  // --------------------------------------------------------------------------
  void pushCanvasSubRect(Adafruit_SPITFT &tft, int16_t cx1, int16_t cy1, int16_t cx2, int16_t cy2) {
    cx1 = clampI16(cx1, 0, CANVAS_W - 1);
    cy1 = clampI16(cy1, 0, CANVAS_H - 1);
    cx2 = clampI16(cx2, 0, CANVAS_W - 1);
    cy2 = clampI16(cy2, 0, CANVAS_H - 1);

    int16_t rw = cx2 - cx1 + 1;
    int16_t rh = cy2 - cy1 + 1;
    if (rw <= 0 || rh <= 0) return;

    uint16_t *buf = canvas->getBuffer();
    int16_t screenX = CANVAS_X + cx1;
    int16_t screenY = CANVAS_Y + cy1;

    tft.startWrite();
    tft.setAddrWindow(screenX, screenY, rw, rh);
    for (int16_t row = cy1; row <= cy2; row++) {
      tft.writePixels(&buf[row * CANVAS_W + cx1], rw);
    }
    tft.endWrite();
  }

public:
  RobotEyes()
    : displayPtr(nullptr),
      canvas(nullptr),
      visible(false),
      needsFullClear(true),
      scanlineEffect(false),
      autoStateColors(true),
      baseWidth(72.0f),
      baseHeight(84.0f),
      baseRadius(18.0f),
      baseSpacing(34.0f),
      bgColor(0x0000),
      curEyeColor(COLOR_EYE_CYAN),
      targetEyeColor(COLOR_EYE_CYAN),
      curGazeX(0.0f),
      curGazeY(0.0f),
      targetGazeX(0.0f),
      targetGazeY(0.0f),
      manualGazeActive(false),
      manualGazeHoldUntil(0),
      currentState(EYE_STATE_IDLE),
      lastFrameMs(0),
      nextBlinkMs(3000),
      blinkStartMs(0),
      isBlinking(false),
      doubleBlinkPending(false),
      nextSaccadeMs(1500),
      stateEnteredMs(0),
      externalSpeechLevel(-1.0f),
      musicBass01(0.0f),
      musicHigh01(0.0f),
      musicActive(false),
      prevMinX(0),
      prevMinY(0),
      prevMaxX(CANVAS_W - 1),
      prevMaxY(CANVAS_H - 1) {
    resetTargetsToBase();
    curL = targetL;
    curR = targetR;
  }

  ~RobotEyes() {
    if (canvas) {
      delete canvas;
      canvas = nullptr;
    }
  }

  // --------------------------------------------------------------------------
  // Инициализация модуля (выделение RAM-буфера GFXcanvas16 и стартовых таймеров)
  // --------------------------------------------------------------------------
  void init(Adafruit_SPITFT *tft = nullptr) {
    if (tft) displayPtr = tft;
    if (!canvas) {
      canvas = new GFXcanvas16(CANVAS_W, CANVAS_H);
      if (canvas) {
        canvas->fillScreen(bgColor);
      }
    }
    unsigned long now = millis();
    lastFrameMs    = now;
    stateEnteredMs = now;
    scheduleNextBlink(now, 2500, 5000);
    nextSaccadeMs  = now + 1500;
  }

  // Перегрузка init по ссылке на дисплей
  void init(Adafruit_SPITFT &tft) {
    init(&tft);
  }

  // --------------------------------------------------------------------------
  // Настройка геометрии и визуального стиля глаз
  // --------------------------------------------------------------------------
  void setEyeGeometry(float width, float height, float cornerRadius, float spacing) {
    baseWidth   = width;
    baseHeight  = height;
    baseRadius  = cornerRadius;
    baseSpacing = spacing;
  }

  void setColors(uint16_t eyeColor565, uint16_t background565 = 0x0000) {
    targetEyeColor  = eyeColor565;
    curEyeColor     = eyeColor565;
    bgColor         = background565;
    autoStateColors = false;
    needsFullClear  = true;
  }

  void setAutoStateColors(bool enabled) {
    autoStateColors = enabled;
  }

  void setScanlineEffect(bool enabled) {
    scanlineEffect = enabled;
  }

  // Передача внешней амплитуды речи (0.0 .. 1.0) для синхронизации с реальным TTS
  void setSpeechLevel(float level01) {
    if (level01 < 0.0f) {
      externalSpeechLevel = -1.0f;
    } else {
      externalSpeechLevel = (level01 > 1.0f) ? 1.0f : level01;
    }
  }

  // Передача спектральной энергии музыки (бас + верхние частоты 0.0 .. 1.0) для танца глаз
  void setMusicEnergy(float bass01, float high01, bool active) {
    musicActive = active;
    if (!active) {
      musicBass01 = lerpVal(musicBass01, 0.0f, 0.35f);
      musicHigh01 = lerpVal(musicHigh01, 0.0f, 0.35f);
    } else {
      float b = (bass01 < 0.0f) ? 0.0f : ((bass01 > 1.0f) ? 1.0f : bass01);
      float h = (high01 < 0.0f) ? 0.0f : ((high01 > 1.0f) ? 1.0f : high01);
      musicBass01 = lerpVal(musicBass01, b, 0.55f);
      musicHigh01 = lerpVal(musicHigh01, h, 0.55f);
    }
  }

  // Принудительное одиночное моргание
  void triggerBlink() {
    isBlinking = true;
    blinkStartMs = millis();
    doubleBlinkPending = false;
  }

  // --------------------------------------------------------------------------
  // Управление состоянием автомата (FSM)
  // --------------------------------------------------------------------------
  void setEyeState(EyeState state) {
    if (currentState != state) {
      currentState   = state;
      stateEnteredMs = millis();
      // Короткое естественное моргание при смене эмоции/состояния
      triggerBlink();
    }
  }

#ifdef UI_THEME_H
  // Прямая совместимость с enum AssistantState из UITheme.h:
  // позволяет вызывать setEyeState(STATE_LISTENING / STATE_THINKING / STATE_SPEAKING / STATE_READY)
  void setEyeState(AssistantState st) {
    switch (st) {
      case STATE_READY:     setEyeState(EYE_STATE_IDLE);      break;
      case STATE_LISTENING: setEyeState(EYE_STATE_LISTENING); break;
      case STATE_THINKING:  setEyeState(EYE_STATE_THINKING);  break;
      case STATE_SPEAKING:  setEyeState(EYE_STATE_SPEAKING);  break;
      default:              setEyeState(EYE_STATE_IDLE);      break;
    }
  }
#endif

  EyeState getEyeState() const {
    return currentState;
  }

  // --------------------------------------------------------------------------
  // Управление точкой взгляда lookAt(x, y)
  // Поддерживает:
  // - Относительное смещение в пикселях (-40..+40, -30..+30)
  // - Экранные координаты (0..320, 0..240), если переданы координаты точки на экране
  // --------------------------------------------------------------------------
  void lookAt(int x, int y, uint32_t holdDurationMs = 3000) {
    float gx = (float)x;
    float gy = (float)y;

    // Если переданы абсолютные координаты экрана 320x240, переводим в смещение от центра (160, 120)
    if (x > 45 || x < -45 || y > 35 || y < -35) {
      gx = ((float)x - 160.0f) * 0.22f;
      gy = ((float)y - 120.0f) * 0.22f;
    }

    if (gx < -32.0f) gx = -32.0f;
    if (gx >  32.0f) gx =  32.0f;
    if (gy < -22.0f) gy = -22.0f;
    if (gy >  22.0f) gy =  22.0f;

    targetGazeX = gx;
    targetGazeY = gy;
    manualGazeActive = true;
    manualGazeHoldUntil = millis() + holdDurationMs;
  }

  // --------------------------------------------------------------------------
  // Мягкий показ / скрытие экрана глаз (show / hide)
  // --------------------------------------------------------------------------
  void show(Adafruit_SPITFT *tft = nullptr) {
    if (tft) displayPtr = tft;
    if (!canvas) init(displayPtr);

    visible = true;
    needsFullClear = true;

    // Эффект «пробуждения» при открытии экрана глаз: глаза плавно раскрываются из прищура
    curL.h = 6.0f;
    curR.h = 6.0f;
    curL.topLid = 0.45f;
    curL.botLid = 0.45f;
    curR.topLid = 0.45f;
    curR.botLid = 0.45f;

    if (displayPtr) {
      displayPtr->fillScreen(bgColor);
      update(*displayPtr, true);
    }
  }

  void show(Adafruit_SPITFT &tft) {
    show(&tft);
  }

  void hide() {
    visible = false;
  }

  bool isVisible() const {
    return visible;
  }

  // --------------------------------------------------------------------------
  // Неблокирующий цикл обновления и отрисовки (~40 FPS, интервал 25 мс)
  // --------------------------------------------------------------------------
  void update(Adafruit_SPITFT &tft, bool forceRender = false) {
    if (!visible) return;
    if (!canvas) init(&tft);
    if (!canvas) return;

    unsigned long now = millis();
    if (!forceRender && (now - lastFrameMs < 25)) {
      return; // Ограничение до ~40 FPS без блокировки loop()
    }
    lastFrameMs = now;

    // 1. Обновление логики состояний FSM и генератора морганий/саккад
    updateStateTargets(now);

    // 2. Шаг плавной физики (LERP)
    stepPhysics();

    // 3. Вычисление центров левого и правого глаза внутри canvas
    const float canvasCenterX = CANVAS_W * 0.5f;
    const float canvasCenterY = CANVAS_H * 0.5f;

    float leftCX  = canvasCenterX - (baseSpacing * 0.5f) - (curL.w * 0.5f) + curGazeX;
    float leftCY  = canvasCenterY + curGazeY + curL.yOffset;

    float rightCX = canvasCenterX + (baseSpacing * 0.5f) + (curR.w * 0.5f) + curGazeX;
    float rightCY = canvasCenterY + curGazeY + curR.yOffset;

    // 4. Вычисление ограничивающего прямоугольника (Bounding Box) текущего кадра
    int16_t lMinX = (int16_t)(leftCX  - curL.w * 0.5f) - 3;
    int16_t lMaxX = (int16_t)(leftCX  + curL.w * 0.5f) + 3;
    int16_t lMinY = (int16_t)(leftCY  - curL.h * 0.5f) - 3;
    int16_t lMaxY = (int16_t)(leftCY  + curL.h * 0.5f) + 3;

    int16_t rMinX = (int16_t)(rightCX - curR.w * 0.5f) - 3;
    int16_t rMaxX = (int16_t)(rightCX + curR.w * 0.5f) + 3;
    int16_t rMinY = (int16_t)(rightCY - curR.h * 0.5f) - 3;
    int16_t rMaxY = (int16_t)(rightCY + curR.h * 0.5f) + 3;

    int16_t currMinX = clampI16((lMinX < rMinX) ? lMinX : rMinX, 0, CANVAS_W - 1);
    int16_t currMaxX = clampI16((lMaxX > rMaxX) ? lMaxX : rMaxX, 0, CANVAS_W - 1);
    int16_t currMinY = clampI16((lMinY < rMinY) ? lMinY : rMinY, 0, CANVAS_H - 1);
    int16_t currMaxY = clampI16((lMaxY > rMaxY) ? lMaxY : rMaxY, 0, CANVAS_H - 1);

    if (currentState == EYE_STATE_SLEEPY) {
      currMinY = 0;
      currMaxX = CANVAS_W - 1;
    }

    // Объединение с прямоугольником прошлого кадра (Dirty Rect) для полного стирания следов
    int16_t dirtyMinX = (currMinX < prevMinX) ? currMinX : prevMinX;
    int16_t dirtyMaxX = (currMaxX > prevMaxX) ? currMaxX : prevMaxX;
    int16_t dirtyMinY = (currMinY < prevMinY) ? currMinY : prevMinY;
    int16_t dirtyMaxY = (currMaxY > prevMaxY) ? currMaxY : prevMaxY;

    if (needsFullClear) {
      canvas->fillScreen(bgColor);
      dirtyMinX = 0;
      dirtyMinY = 0;
      dirtyMaxX = CANVAS_W - 1;
      dirtyMaxY = CANVAS_H - 1;
      needsFullClear = false;
    } else {
      // Очищаем в RAM-буфере только грязную область
      canvas->fillRect(dirtyMinX, dirtyMinY,
                       dirtyMaxX - dirtyMinX + 1,
                       dirtyMaxY - dirtyMinY + 1,
                       bgColor);
    }

    // 5. Отрисовка обоих глаз в RAM-буфер (Double Buffering)
    drawSingleEyeToCanvas(leftCX,  leftCY,  curL, true);
    drawSingleEyeToCanvas(rightCX, rightCY, curR, false);

    if (currentState == EYE_STATE_SLEEPY) {
      drawSleepZzzToCanvas(now);
    }

    // 6. Мгновенная пересылка обновленного прямоугольника на экран без мерцания
    pushCanvasSubRect(tft, dirtyMinX, dirtyMinY, dirtyMaxX, dirtyMaxY);

    // Сохраняем границы для следующего кадра
    prevMinX = currMinX;
    prevMinY = currMinY;
    prevMaxX = currMaxX;
    prevMaxY = currMaxY;
  }

  // Перегрузка update() без аргументов (если дисплей был передан в init/show)
  void update() {
    if (displayPtr) {
      update(*displayPtr, false);
    }
  }
};

#endif // ROBOT_EYES_H

