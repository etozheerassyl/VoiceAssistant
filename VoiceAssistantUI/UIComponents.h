#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"

// ============================================================================
// Векторная иконка Wi-Fi
// ============================================================================
inline void drawWifiIcon(Adafruit_GFX &gfx, int16_t x, int16_t y, uint16_t color) {
  // Точка-основание
  gfx.fillCircle(x + 7, y + 10, 1, color);

  // Нижняя дуга
  gfx.drawPixel(x + 5, y + 8, color);
  gfx.drawFastHLine(x + 6, y + 7, 3, color);
  gfx.drawPixel(x + 9, y + 8, color);

  // Средняя дуга
  gfx.drawPixel(x + 3, y + 5, color);
  gfx.drawPixel(x + 4, y + 4, color);
  gfx.drawFastHLine(x + 5, y + 3, 5, color);
  gfx.drawPixel(x + 10, y + 4, color);
  gfx.drawPixel(x + 11, y + 5, color);

  // Верхняя большая дуга
  gfx.drawPixel(x + 1, y + 2, color);
  gfx.drawPixel(x + 2, y + 1, color);
  gfx.drawFastHLine(x + 3, y, 9, color);
  gfx.drawPixel(x + 12, y + 1, color);
  gfx.drawPixel(x + 13, y + 2, color);
}

// ============================================================================
// Векторная иконка погоды: Солнце с лучами
// ============================================================================
inline void drawSunIcon(Adafruit_GFX &gfx, int16_t cx, int16_t cy, uint16_t colorSun, uint16_t colorRay) {
  // Центральный солнечный диск
  gfx.fillCircle(cx, cy, 6, colorSun);

  // 8 лучей
  gfx.drawFastVLine(cx, cy - 11, 3, colorRay); // Верх
  gfx.drawFastVLine(cx, cy + 9, 3, colorRay);  // Низ
  gfx.drawFastHLine(cx - 11, cy, 3, colorRay); // Лево
  gfx.drawFastHLine(cx + 9, cy, 3, colorRay);  // Право

  // Диагональные лучи
  gfx.drawLine(cx - 7, cy - 7, cx - 5, cy - 5, colorRay);
  gfx.drawLine(cx + 5, cy - 5, cx + 7, cy - 7, colorRay);
  gfx.drawLine(cx - 7, cy + 7, cx - 5, cy + 5, colorRay);
  gfx.drawLine(cx + 5, cy + 5, cx + 7, cy + 7, colorRay);
}

// ============================================================================
// Векторная иконка динамика (для оверлея громкости)
// ============================================================================
inline void drawSpeakerIcon(Adafruit_GFX &gfx, int16_t x, int16_t y, uint16_t color, uint8_t volumePercent) {
  // Задний прямоугольный блок магнита
  gfx.fillRect(x, y + 4, 4, 8, color);

  // Конус диффузора
  gfx.drawLine(x + 4, y + 4, x + 10, y, color);
  gfx.drawLine(x + 4, y + 11, x + 10, y + 15, color);
  gfx.drawFastVLine(x + 10, y, 16, color);
  for (int8_t i = 0; i <= 6; i++) {
    gfx.drawLine(x + 4, y + 4 + i, x + 10, y + i * 2, color);
  }

  // Звуковые волны в зависимости от громкости
  if (volumePercent > 0) {
    // 1-я малая волна
    gfx.drawPixel(x + 13, y + 5, color);
    gfx.drawFastVLine(x + 14, y + 6, 4, color);
    gfx.drawPixel(x + 13, y + 10, color);
  }
  if (volumePercent > 35) {
    // 2-я средняя волна
    gfx.drawPixel(x + 17, y + 3, color);
    gfx.drawFastVLine(x + 18, y + 4, 8, color);
    gfx.drawPixel(x + 17, y + 12, color);
  }
  if (volumePercent > 70) {
    // 3-я большая волна
    gfx.drawPixel(x + 21, y + 1, color);
    gfx.drawFastVLine(x + 22, y + 2, 12, color);
    gfx.drawPixel(x + 21, y + 14, color);
  }
}

// ============================================================================
// Верхняя панель (Header): Заголовок / Название, Wi-Fi и Часы
// ============================================================================
inline void drawHeader(Adafruit_GFX &gfx, const char *title, const char *timeStr, bool wifiOk) {
  // Фон шапки
  gfx.fillRect(0, 0, SCREEN_WIDTH, 24, COLOR_BG);

  // Декоративный золотистый ромбик слева
  gfx.drawPixel(10, 11, COLOR_GOLD);
  gfx.drawFastHLine(9, 12, 3, COLOR_GOLD);
  gfx.drawPixel(10, 13, COLOR_GOLD);

  // Название устройства / текущего раздела
  drawStringUTF8(gfx, 16, 8, title, COLOR_GOLD, COLOR_BG, 1);

  // Иконка Wi-Fi
  drawWifiIcon(gfx, 246, 6, wifiOk ? COLOR_GOLD : COLOR_TEXT_MUTED);

  // Время в шапке
  drawStringUTF8(gfx, 268, 8, timeStr, COLOR_BEIGE, COLOR_BG, 1);

  // Нижняя золотая линия-разделитель шапки
  gfx.drawFastHLine(10, 24, 300, COLOR_GOLD_DARK);
  gfx.drawFastHLine(80, 24, 160, COLOR_GOLD);
  gfx.drawPixel(160, 23, COLOR_GOLD_LIGHT);
  gfx.drawPixel(160, 25, COLOR_GOLD_LIGHT);
}

// Обновление только часов в статус-баре шапки (без перерисовки всей шапки)
inline void updateHeaderTime(Adafruit_GFX &gfx, const char *timeStr) {
  drawStringUTF8(gfx, 268, 8, timeStr, COLOR_BEIGE, COLOR_BG, 1);
}

// ============================================================================
// Компактный индикатор микрофона INMP441 и мини шкала уровня в шапке (x=200..238)
// ============================================================================
inline void drawHeaderMicStatus(Adafruit_GFX &gfx, float level01, bool voiceActive, bool micOk, bool zeroWarn) {
  const int16_t mx = 200;
  const int16_t my = 6;

  uint16_t micColor = COLOR_TEXT_MUTED;
  if (micOk && !zeroWarn) {
    micColor = voiceActive ? COLOR_STATUS_LISTEN : COLOR_STATUS_READY;
  } else if (zeroWarn) {
    micColor = COLOR_STATUS_THINK; // Янтарный при предупреждении о нулевом сигнале SD
  }

  // Мини-иконка капсюля микрофона (x=200..206, y=6..17)
  gfx.fillRect(mx, my, 8, 13, COLOR_BG);
  gfx.fillRoundRect(mx + 2, my, 3, 7, 1, micColor);
  gfx.drawFastVLine(mx, my + 4, 3, micColor);
  gfx.drawFastVLine(mx + 6, my + 4, 3, micColor);
  gfx.drawFastHLine(mx + 1, my + 8, 5, micColor);
  gfx.drawFastVLine(mx + 3, my + 9, 3, micColor);
  gfx.drawFastHLine(mx + 1, my + 11, 5, micColor);

  // Горизонтальная мини-шкала уровня (x=210..236, ширина 26 px, высота 6 px)
  const int16_t barX = mx + 10;
  const int16_t barY = my + 3;
  const int16_t barW = 26;
  const int16_t barH = 6;

  gfx.drawRect(barX, barY, barW, barH, COLOR_CARD_BORDER);

  int16_t fillW = (int16_t)(level01 * (float)(barW - 2) + 0.5f);
  if (fillW < 0) fillW = 0;
  if (fillW > barW - 2) fillW = barW - 2;

  if (fillW > 0) {
    uint16_t barColor = voiceActive ? COLOR_GOLD : COLOR_STATUS_READY;
    gfx.fillRect(barX + 1, barY + 1, fillW, barH - 2, barColor);
  }
  if (fillW < barW - 2) {
    gfx.fillRect(barX + 1 + fillW, barY + 1, (barW - 2) - fillW, barH - 2, COLOR_BG);
  }
}

// ============================================================================
// Индикатор текущей страницы (Pagination Dots / Pills)
// ============================================================================
inline void drawPagination(Adafruit_GFX &gfx, uint8_t activeIndex, uint8_t total) {
  const int16_t cy = 228;
  const int16_t spacing = 18;
  const int16_t startX = 160 - ((total - 1) * spacing) / 2;

  // Очистка полосы пагинации
  gfx.fillRect(115, 222, 90, 14, COLOR_BG);

  for (uint8_t i = 0; i < total; i++) {
    int16_t px = startX + i * spacing;
    if (i == activeIndex) {
      // Активная страница: вытянутая стильная пилюля
      gfx.fillRoundRect(px - 7, cy - 2, 14, 5, 2, COLOR_GOLD);
      gfx.drawRoundRect(px - 7, cy - 2, 14, 5, 2, COLOR_GOLD_LIGHT);
    } else {
      // Неактивная страница: компактный круг
      gfx.fillCircle(px, cy, 2, COLOR_CARD_BORDER);
      gfx.drawCircle(px, cy, 2, COLOR_TEXT_MUTED);
    }
  }
}

#endif // UI_COMPONENTS_H

