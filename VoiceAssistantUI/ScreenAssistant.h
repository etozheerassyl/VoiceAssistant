#ifndef SCREEN_ASSISTANT_H
#define SCREEN_ASSISTANT_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <math.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"
#include "UIComponents.h"

// ============================================================================
// Экран голосового помощника (Voice Assistant Screen)
// Состояния:
// 1. STATE_READY     - Готов к работе (плавное дыхание изумрудного ядра)
// 2. STATE_LISTENING - Слушаю (бирюзовые расходящиеся звуковые волны)
// 3. STATE_THINKING  - Обработка (янтарные орбитальные частицы)
// 4. STATE_SPEAKING  - Произношу ответ (золотые лучи звуковой волны)
// ============================================================================

class ScreenAssistant {
private:
  AssistantState currentState;
  uint8_t animPhase;
  int16_t prevWaveR[3];

  // Телеметрия микрофона INMP441
  float   micLevel01;
  int32_t micRms16;
  float   micDbfs;
  bool    micOk;
  bool    micZeroWarn;
  bool    micVoiceActive;
  int16_t prevDrawnBarW;
  int32_t prevDrawnDbInt;

  // Последняя распознанная голосовая команда и ответ помощника
  char    lastHeardLine[96];
  char    lastReplyLine[96];
  bool    hasCustomDialog;

public:
  ScreenAssistant()
    : currentState(STATE_READY),
      animPhase(0),
      micLevel01(0.0f),
      micRms16(0),
      micDbfs(-96.0f),
      micOk(false),
      micZeroWarn(false),
      micVoiceActive(false),
      prevDrawnBarW(-1),
      prevDrawnDbInt(-999),
      hasCustomDialog(false) {
    prevWaveR[0] = 0;
    prevWaveR[1] = 0;
    prevWaveR[2] = 0;
    lastHeardLine[0] = '\0';
    lastReplyLine[0] = '\0';
  }

  void setVoiceDialog(const char *heard, const char *reply) {
    if (heard && strlen(heard) > 0) {
      char shortHeard[88];
      truncateUTF8(heard, shortHeard, sizeof(shortHeard), 32);
      snprintf(lastHeardLine, sizeof(lastHeardLine), "Вы: «%s»", shortHeard);
    } else {
      strncpy(lastHeardLine, "Вы: «...»", sizeof(lastHeardLine) - 1);
    }

    if (reply && strlen(reply) > 0) {
      truncateUTF8(reply, lastReplyLine, sizeof(lastReplyLine), 32);
    } else {
      strncpy(lastReplyLine, "Готово", sizeof(lastReplyLine) - 1);
    }
    hasCustomDialog = true;
  }

  void setMicTelemetry(float level01, int32_t rms16, float dbfs, bool ok, bool zeroWarn, bool voiceActive) {
    micLevel01     = level01;
    micRms16       = rms16;
    micDbfs        = dbfs;
    micOk          = ok;
    micZeroWarn    = zeroWarn;
    micVoiceActive = voiceActive;
  }

  AssistantState getState() const { return currentState; }

  void setState(AssistantState st) {
    currentState = st;
    animPhase = 0;
    prevDrawnBarW = -1;
    prevDrawnDbInt = -999;
  }

  void nextState() {
    uint8_t next = ((uint8_t)currentState + 1) % 4;
    currentState = (AssistantState)next;
    animPhase = 0;
    prevDrawnBarW = -1;
    prevDrawnDbInt = -999;
  }

  void drawStatic(Adafruit_GFX &gfx, const char *timeHeaderStr, bool wifiOk) {
    gfx.fillScreen(COLOR_BG);

    // 1. Шапка
    drawHeader(gfx, "AI ДАУЫС КӨМЕКШІ", timeHeaderStr, wifiOk);

    // 2. Боковые тонкие орнаменты и углы
    drawSideBorders(gfx, COLOR_GOLD);
    drawCornerAccents(gfx, COLOR_GOLD_DARK);

    // 3. Статичная подложка центрального элемента (cx=160, cy=95)
    drawCenterStatic(gfx);

    // 4. Карточка текстового состояния
    drawStateCard(gfx);

    // 5. Нижний орнамент и пагинация (индекс 3 = 4-я страница)
    drawBottomOrnamentBar(gfx, 214, COLOR_GOLD);
    drawPagination(gfx, 3, SCREEN_COUNT);
  }

  // Статичная основа центрального графического ядра
  void drawCenterStatic(Adafruit_GFX &gfx) {
    const int16_t cx = 160;
    const int16_t cy = 92;

    // Внешняя сдержанная орбита
    gfx.drawCircle(cx, cy, 48, COLOR_CARD_BORDER);
    gfx.drawCircle(cx, cy, 40, COLOR_CARD_BG);

    // Декоративные микро-риски по осям
    gfx.drawFastHLine(cx - 52, cy, 3, COLOR_GOLD_DARK);
    gfx.drawFastHLine(cx + 49, cy, 3, COLOR_GOLD_DARK);
    gfx.drawFastVLine(cx, cy - 52, 3, COLOR_GOLD_DARK);
    gfx.drawFastVLine(cx, cy + 49, 3, COLOR_GOLD_DARK);
  }

  // Отрисовка информационной карточки в зависимости от состояния
  void drawStateCard(Adafruit_GFX &gfx) {
    const int16_t cardY = 146;
    const int16_t cardW = 280;
    const int16_t cardH = 58;
    const int16_t cardX = 20;

    // Очистка и отрисовка плашки
    gfx.fillRoundRect(cardX, cardY, cardW, cardH, 6, COLOR_CARD_BG);
    gfx.drawRoundRect(cardX, cardY, cardW, cardH, 6, COLOR_CARD_BORDER);

    uint16_t accentColor = COLOR_GOLD;
    const char *stateTitle = "";
    const char *dialogLine1 = "";
    const char *dialogLine2 = "";

    switch (currentState) {
      case STATE_READY:
        accentColor = COLOR_STATUS_READY;
        stateTitle = "ЖҮЙЕ ДАЙЫН • ГОТОВ";
        if (hasCustomDialog) {
          dialogLine1 = lastHeardLine;
          dialogLine2 = lastReplyLine;
        } else {
          dialogLine1 = "«Сәлем, көмекші» деп айтыңыз";
          dialogLine2 = "ESP32-S3 AI дауыстық жүйесі белсенді";
        }
        break;

      case STATE_LISTENING:
        accentColor = COLOR_STATUS_LISTEN;
        stateTitle = "ТЫҢДАП ТҰРМЫН... • СЛУШАЮ";
        dialogLine1 = "Айта беріңіз • Говорите команду...";
        dialogLine2 = "Дауыстық пәрмен жазылуда...";
        break;

      case STATE_THINKING:
        accentColor = COLOR_STATUS_THINK;
        stateTitle = "ОЙЛАНУДА... • ОБРАБОТКА";
        if (hasCustomDialog) {
          dialogLine1 = lastHeardLine;
          dialogLine2 = lastReplyLine;
        } else {
          dialogLine1 = "Сұраныс талдануда...";
          dialogLine2 = "MacBook AI ищет ответ...";
        }
        break;

      case STATE_SPEAKING:
        accentColor = COLOR_STATUS_SPEAK;
        stateTitle = "ЖАУАП БЕРУДЕ • ОТВЕТ";
        if (hasCustomDialog) {
          dialogLine1 = lastHeardLine;
          dialogLine2 = lastReplyLine;
        } else {
          dialogLine1 = "«Алматыда бүгін +18°C, ашық күн»";
          dialogLine2 = "Дыбыстық синтез белсенді";
        }
        break;
    }

    // Левая акцентная полоска карточки
    gfx.fillRoundRect(cardX, cardY + 4, 3, cardH - 8, 1, accentColor);

    // Заголовок статуса
    drawStringUTF8(gfx, cardX + 14, cardY + 8, stateTitle, accentColor, COLOR_CARD_BG, 1);

    // Строки диалога
    drawStringUTF8(gfx, cardX + 14, cardY + 24, dialogLine1, COLOR_TEXT_WHITE, COLOR_CARD_BG, 1);
    uint16_t line2Color = (currentState == STATE_SPEAKING && hasCustomDialog) ? COLOR_GOLD_LIGHT : COLOR_TEXT_MUTED;
    drawStringUTF8(gfx, cardX + 14, cardY + 40, dialogLine2, line2Color, COLOR_CARD_BG, 1);

    // Принудительная отрисовка шкалы уровня микрофона INMP441 в карточке
    prevDrawnBarW = -1;
    prevDrawnDbInt = -999;
    drawMicLevelBar(gfx, true);
  }

  // Дифференциальная отрисовка VU-метра микрофона INMP441 в правом верхнем углу карточки
  void drawMicLevelBar(Adafruit_GFX &gfx, bool forceRedraw = false) {
    const int16_t barX = 216;
    const int16_t barY = 154;
    const int16_t barW = 76;
    const int16_t barH = 8;

    int16_t fillW = (int16_t)(micLevel01 * (float)(barW - 2) + 0.5f);
    if (fillW < 0) fillW = 0;
    if (fillW > barW - 2) fillW = barW - 2;

    int32_t dbInt = (int32_t)roundf(micDbfs);
    if (dbInt < -60) dbInt = -60;
    if (dbInt > 0)   dbInt = 0;

    if (forceRedraw) {
      gfx.drawRect(barX, barY, barW, barH, COLOR_CARD_BORDER);
    }

    if (forceRedraw || fillW != prevDrawnBarW) {
      uint16_t fillColor = micVoiceActive ? COLOR_GOLD : COLOR_STATUS_LISTEN;
      if (!micOk || micZeroWarn) {
        fillColor = COLOR_STATUS_THINK;
      }
      if (fillW > 0) {
        gfx.fillRect(barX + 1, barY + 1, fillW, barH - 2, fillColor);
      }
      if (fillW < barW - 2) {
        gfx.fillRect(barX + 1 + fillW, barY + 1, (barW - 2) - fillW, barH - 2, COLOR_CARD_BG);
      }
      prevDrawnBarW = fillW;
    }

    // Обновляем компактную текстовую подпись dBFS справа на третьей строке карточки
    if (forceRedraw || abs(dbInt - prevDrawnDbInt) >= 2) {
      char dbBuf[16];
      if (!micOk) {
        snprintf(dbBuf, sizeof(dbBuf), "MIC: ERR");
      } else if (micZeroWarn) {
        snprintf(dbBuf, sizeof(dbBuf), "MIC: 0 SD");
      } else {
        snprintf(dbBuf, sizeof(dbBuf), "%3d dBFS", (int)dbInt);
      }
      gfx.fillRect(240, 185, 54, 10, COLOR_CARD_BG);
      drawStringUTF8(gfx, 242, 186, dbBuf, micVoiceActive ? COLOR_GOLD : COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);
      prevDrawnDbInt = dbInt;
    }
  }

  // Обновление анимации центрального элемента (вызывается ~ каждые 60-80 мс)
  void updateAnimation(Adafruit_GFX &gfx) {
    const int16_t cx = 160;
    const int16_t cy = 92;
    animPhase++;

    // Дифференциально обновляем индикатор уровня микрофона в карточке
    drawMicLevelBar(gfx, false);

    switch (currentState) {
      // ----------------------------------------------------------------------
      // 1. READY: Плавное дыхание кольца + отклик на уровень микрофона
      // ----------------------------------------------------------------------
      case STATE_READY: {
        uint8_t breath = (animPhase % 30);
        int16_t micBoost = (int16_t)(micLevel01 * 8.0f);
        int16_t pulseR = 26 + (breath < 15 ? breath : (30 - breath)) / 3 + micBoost;
        if (pulseR > 35) pulseR = 35;

        // Очистка внутренней области
        gfx.fillCircle(cx, cy, 36, COLOR_BG);

        // Изумрудное центральное ядро
        int16_t coreR = 14 + (int16_t)(micLevel01 * 4.0f);
        gfx.fillCircle(cx, cy, coreR, COLOR_STATUS_READY);
        gfx.drawCircle(cx, cy, coreR + 1, COLOR_GOLD_LIGHT);

        // Пульсирующий внешний обод
        gfx.drawCircle(cx, cy, pulseR, micVoiceActive ? COLOR_GOLD : COLOR_STATUS_READY);
        gfx.drawCircle(cx, cy, 33, COLOR_GOLD_DARK);
        break;
      }

      // ----------------------------------------------------------------------
      // 2. LISTENING: Расширяющиеся звуковые волны, модулируемые уровнем речи
      // ----------------------------------------------------------------------
      case STATE_LISTENING: {
        // Очищаем область анимационных волн
        gfx.fillCircle(cx, cy, 38, COLOR_BG);

        // Центральный индикатор микрофона (радиус дышит от реального голоса)
        int16_t coreR = 11 + (int16_t)(micLevel01 * 6.0f);
        gfx.fillCircle(cx, cy, coreR, COLOR_STATUS_LISTEN);
        gfx.drawCircle(cx, cy, coreR + 1, COLOR_TEXT_WHITE);

        // Расширяющиеся кольца волны
        uint8_t step = animPhase % 18;
        int16_t r1 = 16 + step;
        int16_t r2 = 16 + ((step + 6) % 18);
        int16_t r3 = 16 + ((step + 12) % 18);

        if (r1 <= 35) gfx.drawCircle(cx, cy, r1, COLOR_STATUS_LISTEN);
        if (r2 <= 35) gfx.drawCircle(cx, cy, r2, micVoiceActive ? COLOR_GOLD : 0x14E4);
        if (r3 <= 35) gfx.drawCircle(cx, cy, r3, COLOR_GOLD_DARK);
        break;
      }

      // ----------------------------------------------------------------------
      // 3. THINKING: Вращающиеся по орбите янтарные частицы
      // ----------------------------------------------------------------------
      case STATE_THINKING: {
        gfx.fillCircle(cx, cy, 36, COLOR_BG);

        // Центральное янтарное ядро
        gfx.fillCircle(cx, cy, 10, COLOR_STATUS_THINK);
        gfx.drawCircle(cx, cy, 11, COLOR_GOLD_LIGHT);

        // Орбита
        gfx.drawCircle(cx, cy, 26, COLOR_CARD_BORDER);

        // 3 вращающиеся частицы (сдвиг фаз 120 градусов)
        float angle1 = (float)(animPhase * 16) * 0.017453f;
        float angle2 = angle1 + 2.0944f;
        float angle3 = angle1 + 4.1887f;

        int16_t p1x = cx + (int16_t)(26.0f * cosf(angle1));
        int16_t p1y = cy + (int16_t)(26.0f * sinf(angle1));
        gfx.fillCircle(p1x, p1y, 4, COLOR_STATUS_THINK);

        int16_t p2x = cx + (int16_t)(26.0f * cosf(angle2));
        int16_t p2y = cy + (int16_t)(26.0f * sinf(angle2));
        gfx.fillCircle(p2x, p2y, 3, COLOR_GOLD);

        int16_t p3x = cx + (int16_t)(26.0f * cosf(angle3));
        int16_t p3y = cy + (int16_t)(26.0f * sinf(angle3));
        gfx.fillCircle(p3x, p3y, 2, COLOR_BEIGE);
        break;
      }

      // ----------------------------------------------------------------------
      // 4. SPEAKING: Звуковая форма голосового ответа (Золотые лучи)
      // ----------------------------------------------------------------------
      case STATE_SPEAKING: {
        gfx.fillCircle(cx, cy, 38, COLOR_BG);

        // Центральный золотистый диск
        gfx.fillCircle(cx, cy, 14, COLOR_GOLD);
        gfx.drawCircle(cx, cy, 16, COLOR_GOLD_LIGHT);

        // Динамические голосовые лучи (8 радиальных отрезков)
        float basePhase = (float)animPhase * 0.25f;
        for (uint8_t i = 0; i < 8; i++) {
          float ang = (float)i * 0.785398f; // 45 градусов
          float rayLen = 20.0f + 14.0f * sinf(basePhase + (float)i * 0.8f);
          if (rayLen < 18.0f) rayLen = 18.0f;

          int16_t x1 = cx + (int16_t)(17.0f * cosf(ang));
          int16_t y1 = cy + (int16_t)(17.0f * sinf(ang));
          int16_t x2 = cx + (int16_t)(rayLen * cosf(ang));
          int16_t y2 = cy + (int16_t)(rayLen * sinf(ang));

          gfx.drawLine(x1, y1, x2, y2, (i % 2 == 0) ? COLOR_GOLD : COLOR_BEIGE);
        }
        break;
      }
    }
  }
};

#endif // SCREEN_ASSISTANT_H

