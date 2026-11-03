#ifndef OVERLAY_VOLUME_H
#define OVERLAY_VOLUME_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "UIComponents.h"

// ============================================================================
// Всплывающий компонент регулировки громкости (Volume Overlay)
// Отображается поверх активного экрана при изменении громкости (+/-)
// и автоматически плавно скрывается через 2.5 секунды
// ============================================================================

class OverlayVolume {
private:
  uint8_t volumePercent;
  bool visible;
  unsigned long hideTimestamp;
  uint8_t prevDrawnVol;

public:
  OverlayVolume() : volumePercent(70), visible(false), hideTimestamp(0), prevDrawnVol(70) {}

  uint8_t getVolume() const { return volumePercent; }
  bool isVisible() const { return visible; }

  // Установка точного значения громкости (из сети / с MacBook)
  void setVolume(uint8_t vol) {
    volumePercent = (vol > 100) ? 100 : vol;
    visible = true;
    hideTimestamp = millis() + 2500;
  }

  // Изменение громкости на заданную дельту
  void changeVolume(int8_t delta) {
    int16_t newVol = (int16_t)volumePercent + delta;
    if (newVol < 0) newVol = 0;
    if (newVol > 100) newVol = 100;

    volumePercent = (uint8_t)newVol;
    visible = true;
    hideTimestamp = millis() + 2500; // Скрывать через 2.5 секунды
  }

  // Принудительное скрытие
  void dismiss() {
    visible = false;
  }

  // Проверка необходимости скрытия по таймеру
  bool checkTimeout() {
    if (visible && millis() >= hideTimestamp) {
      visible = false;
      return true; // Сигнал о необходимости перерисовки экрана
    }
    return false;
  }

  // Отрисовка оверлея
  void draw(Adafruit_GFX &gfx) {
    if (!visible) return;

    const int16_t boxW = 252;
    const int16_t boxH = 74;
    const int16_t boxX = (SCREEN_WIDTH - boxW) / 2; // 34
    const int16_t boxY = (SCREEN_HEIGHT - boxH) / 2; // 83

    // 1. Модальное окно (темно-графитовый фон и двойная золотая рамка)
    gfx.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    gfx.drawRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_GOLD);
    gfx.drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, COLOR_GOLD_DARK);

    // 2. Иконка динамика слева
    drawSpeakerIcon(gfx, boxX + 16, boxY + 28, COLOR_GOLD, volumePercent);

    // 3. Заголовок
    drawStringUTF8(gfx, boxX + 52, boxY + 14, "ДЫБЫС ДЕҢГЕЙІ • VOLUME", COLOR_GOLD, COLOR_CARD_BG, 1);

    // 4. Горизонтальная шкала громкости
    const int16_t barX = boxX + 52;
    const int16_t barY = boxY + 34;
    const int16_t barW = 138;
    const int16_t barH = 10;

    // Фоновая направляющая шкалы
    gfx.fillRoundRect(barX, barY, barW, barH, 4, 0x18C3);
    gfx.drawRoundRect(barX, barY, barW, barH, 4, COLOR_CARD_BORDER);

    // Заполненная часть
    int16_t fillW = (int32_t)barW * volumePercent / 100;
    if (fillW > 0) {
      gfx.fillRoundRect(barX, barY, fillW, barH, 4, COLOR_GOLD);
      // Верхний блик шкалы
      gfx.drawFastHLine(barX + 2, barY + 2, fillW - 4, COLOR_GOLD_LIGHT);
    }

    // 5. Процент громкости справа
    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%3u%%", volumePercent);
    // Очищаем область текста и рисуем число
    gfx.fillRect(boxX + 198, boxY + 32, 42, 16, COLOR_CARD_BG);
    drawStringUTF8(gfx, boxX + 200, boxY + 34, pctStr, COLOR_TEXT_WHITE, COLOR_CARD_BG, 1);

    prevDrawnVol = volumePercent;
  }
};

#endif // OVERLAY_VOLUME_H
