#ifndef SCREEN_HOME_H
#define SCREEN_HOME_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"
#include "UIComponents.h"

// ============================================================================
// Главный экран (Home Screen)
// Компоновка 320x240:
// - Header: «AI КӨМЕКШІ» + Wi-Fi + Время
// - Крупные цифровые часы и дата по центру
// - Карточка погоды слева (Алматы, +18°C, иконка солнца)
// - Карточка статуса справа (Система готова / Жүйе дайын)
// - Казахский орнамент «Қошқар мүйіз» и нижний бордюр
// - Индикатор страницы
// ============================================================================

class ScreenHome {
private:
  char dateText[48] = "СӘРСЕНБІ, 28 ҚАЗАН";
  int8_t tempC = 18;
  char weatherDesc[32] = "Ашық";
  uint16_t usdKzt = 495;
  uint8_t macBattery = 100;
  bool macCharging = false;
  int32_t timerRemainingSec = 0;

public:
  void setDate(const char *d) {
    if (d && strlen(d) > 0) {
      strncpy(dateText, d, sizeof(dateText) - 1);
      dateText[sizeof(dateText) - 1] = '\0';
    }
  }

  void setLiveTelemetry(int8_t tC, const char *desc, uint16_t usd, uint8_t batt, bool chg) {
    tempC = tC;
    if (desc && strlen(desc) > 0) {
      strncpy(weatherDesc, desc, sizeof(weatherDesc) - 1);
      weatherDesc[sizeof(weatherDesc) - 1] = '\0';
    }
    if (usd > 0) usdKzt = usd;
    macBattery = (batt <= 100) ? batt : 100;
    macCharging = chg;
  }

  void setTimerRemaining(int32_t sec) {
    timerRemainingSec = (sec > 0) ? sec : 0;
  }

  void drawCards(Adafruit_GFX &gfx, bool macConnected) {
    const int16_t cardWY = 102;

    // 4. Карточка погоды и курса валют (слева, x=20, y=102, w=134, h=64)
    gfx.fillRoundRect(20, cardWY, 134, 64, 6, COLOR_CARD_BG);
    gfx.drawRoundRect(20, cardWY, 134, 64, 6, COLOR_CARD_BORDER);
    gfx.fillRoundRect(20, cardWY + 4, 3, 56, 1, COLOR_GOLD);

    // Иконка солнца
    drawSunIcon(gfx, 44, cardWY + 22, COLOR_SUN_GOLD, COLOR_GOLD);

    // Живая температура в Алматы
    char tempBuf[16];
    if (tempC > 0) {
      snprintf(tempBuf, sizeof(tempBuf), "+%d°C", (int)tempC);
    } else {
      snprintf(tempBuf, sizeof(tempBuf), "%d°C", (int)tempC);
    }
    drawStringUTF8(gfx, 66, cardWY + 10, tempBuf, COLOR_TEXT_WHITE, COLOR_CARD_BG, 2);

    // Город + описание погоды
    char cityLine[48];
    snprintf(cityLine, sizeof(cityLine), "Алматы • %s", weatherDesc);
    drawStringUTF8(gfx, 28, cardWY + 35, cityLine, COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);

    // Актуальный курс доллара USD/KZT
    char rateLine[32];
    snprintf(rateLine, sizeof(rateLine), "1 USD = %u KZT", (unsigned int)usdKzt);
    drawStringUTF8(gfx, 28, cardWY + 49, rateLine, COLOR_GOLD, COLOR_CARD_BG, 1);

    // 5. Карточка статуса MacBook, батареи и таймера (справа, x=166, y=102, w=134, h=64)
    gfx.fillRoundRect(166, cardWY, 134, 64, 6, COLOR_CARD_BG);
    gfx.drawRoundRect(166, cardWY, 134, 64, 6, COLOR_CARD_BORDER);

    uint16_t stColor = macConnected ? COLOR_STATUS_READY : COLOR_STATUS_THINK;
    gfx.fillRoundRect(166, cardWY + 4, 3, 56, 1, stColor);

    // Светодиод статуса
    gfx.fillCircle(182, cardWY + 16, 4, stColor);
    gfx.drawCircle(182, cardWY + 16, 6, macConnected ? 0x14E4 : 0x31A0);

    // Подпись карточки
    drawStringUTF8(gfx, 194, cardWY + 12, "MacBook Air", COLOR_GOLD, COLOR_CARD_BG, 1);

    if (timerRemainingSec > 0) {
      uint32_t tmMin = (uint32_t)timerRemainingSec / 60;
      uint32_t tmSec = (uint32_t)timerRemainingSec % 60;
      char tmrStr[32];
      snprintf(tmrStr, sizeof(tmrStr), "ТАЙМЕР %02lu:%02lu", (unsigned long)tmMin, (unsigned long)tmSec);
      drawStringUTF8(gfx, 176, cardWY + 30, tmrStr, COLOR_SUN_GOLD, COLOR_CARD_BG, 1);
    } else {
      drawStringUTF8(gfx, 176, cardWY + 30, macConnected ? "ҚОСЫЛДЫ" : "БАЙЛАНЫС ЖОҚ", COLOR_TEXT_WHITE, COLOR_CARD_BG, 1);
    }

    char battLine[40];
    if (macConnected) {
      snprintf(battLine, sizeof(battLine), "Бат: %u%% • %s", (unsigned int)macBattery, macCharging ? "Қуаттау" : "Онлайн");
    } else {
      snprintf(battLine, sizeof(battLine), "Офлайн • Күтуде");
    }
    drawStringUTF8(gfx, 176, cardWY + 48, battLine, COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);
  }

  void drawStatic(Adafruit_GFX &gfx, const char *timeHeaderStr, bool wifiOk, bool macConnected = false) {
    gfx.fillScreen(COLOR_BG);

    // 1. Верхняя панель
    drawHeader(gfx, "AI КӨМЕКШІ", timeHeaderStr, wifiOk);

    // 2. Боковые тонкие орнаменты и угловые акценты
    drawSideBorders(gfx, COLOR_GOLD);
    drawCornerAccents(gfx, COLOR_GOLD_DARK);

    // 3. Дата и день недели (y=84)
    drawStringUTF8(gfx, 0, 84, dateText, COLOR_BEIGE, COLOR_BG, 1, true);

    // 4 & 5. Карточки живой погоды/курса и статуса MacBook/таймера
    drawCards(gfx, macConnected);

    // 6. Орнамент «Қошқар мүйіз» по центру под карточками (y=182)
    drawKoshkarMuyiz(gfx, 160, 184, COLOR_GOLD);
    gfx.drawFastHLine(40, 184, 80, COLOR_GOLD_DARK);
    gfx.drawFastHLine(200, 184, 80, COLOR_GOLD_DARK);

    // 7. Нижний бордюр и пагинация (активна 1-я страница = 0)
    drawBottomOrnamentBar(gfx, 214, COLOR_GOLD);
    drawPagination(gfx, 0, SCREEN_COUNT);
  }

  // Обновление больших цифровых часов по центру (без перерисовки экрана)
  void updateClock(Adafruit_GFX &gfx, uint8_t hours, uint8_t minutes, uint8_t seconds) {
    char timeStr[12];
    snprintf(timeStr, sizeof(timeStr), "%02u:%02u", hours, minutes);

    char secStr[4];
    snprintf(secStr, sizeof(secStr), ":%02u", seconds);

    // Отрисовка HH:MM крупным шрифтом (size=4, высота 32px, ширина 5*24-4 = 116px)
    // и секунд шрифтом (size=2)
    const int16_t clockY = 40;
    const int16_t clockX = 84;

    drawStringUTF8(gfx, clockX, clockY, timeStr, COLOR_TEXT_WHITE, COLOR_BG, 4);
    drawStringUTF8(gfx, clockX + 116, clockY + 12, secStr, COLOR_GOLD, COLOR_BG, 2);
  }
};

#endif // SCREEN_HOME_H

