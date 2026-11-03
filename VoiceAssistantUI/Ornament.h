#ifndef ORNAMENT_H
#define ORNAMENT_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"

// ============================================================================
// Модуль векторных казахских орнаментов
// Реализует аутентичные традиционные элементы:
// - «Қошқар мүйіз» (бараньи рога) - центральный и парный мотив
// - «Тұмарша» (амулет / треугольный геометрический элемент)
// - «Ирек» (плавная орнаментальная волна)
// - Тонкие боковые и угловые обрамления
// ============================================================================

// Отрисовка симметричного мотива «Қошқар мүйіз» (размах ~40x16 пикселей)
inline void drawKoshkarMuyiz(Adafruit_GFX &gfx, int16_t cx, int16_t cy, uint16_t color) {
  // Центральный узел - ромбовидный тюльпан/бутон
  gfx.drawPixel(cx, cy - 2, color);
  gfx.drawFastHLine(cx - 1, cy - 1, 3, color);
  gfx.drawFastHLine(cx - 2, cy, 5, color);
  gfx.drawFastHLine(cx - 1, cy + 1, 3, color);
  gfx.drawPixel(cx, cy + 2, color);

  // Центральный нижний ствол
  gfx.drawFastVLine(cx, cy + 3, 4, color);

  // Левое крыло орнамента (завиток)
  gfx.drawPixel(cx - 3, cy, color);
  gfx.drawPixel(cx - 4, cy - 1, color);
  gfx.drawPixel(cx - 5, cy - 2, color);
  gfx.drawFastHLine(cx - 8, cy - 3, 3, color);
  gfx.drawPixel(cx - 9, cy - 2, color);
  gfx.drawPixel(cx - 10, cy - 1, color);
  gfx.drawFastVLine(cx - 11, cy, 3, color);
  gfx.drawPixel(cx - 10, cy + 3, color);
  gfx.drawPixel(cx - 9, cy + 4, color);
  gfx.drawFastHLine(cx - 8, cy + 5, 3, color);
  gfx.drawPixel(cx - 5, cy + 4, color);
  gfx.drawPixel(cx - 4, cy + 3, color);
  // Внутренняя спираль левого рога
  gfx.drawPixel(cx - 8, cy + 1, color);
  gfx.drawPixel(cx - 7, cy + 2, color);

  // Правое крыло орнамента (зеркальный завиток)
  gfx.drawPixel(cx + 3, cy, color);
  gfx.drawPixel(cx + 4, cy - 1, color);
  gfx.drawPixel(cx + 5, cy - 2, color);
  gfx.drawFastHLine(cx + 6, cy - 3, 3, color);
  gfx.drawPixel(cx + 9, cy - 2, color);
  gfx.drawPixel(cx + 10, cy - 1, color);
  gfx.drawFastVLine(cx + 11, cy, 3, color);
  gfx.drawPixel(cx + 10, cy + 3, color);
  gfx.drawPixel(cx + 9, cy + 4, color);
  gfx.drawFastHLine(cx + 6, cy + 5, 3, color);
  gfx.drawPixel(cx + 5, cy + 4, color);
  gfx.drawPixel(cx + 4, cy + 3, color);
  // Внутренняя спираль правого рога
  gfx.drawPixel(cx + 8, cy + 1, color);
  gfx.drawPixel(cx + 7, cy + 2, color);
}

// Отрисовка тонкой горизонтальной разделительной полосы с казахским узором
inline void drawOrnamentalDivider(Adafruit_GFX &gfx, int16_t y, uint16_t colorLine, uint16_t colorAccent) {
  // Тонкие золотистые линии
  gfx.drawFastHLine(20, y, 110, colorLine);
  gfx.drawFastHLine(190, y, 110, colorLine);

  // Центральный узел-ромб с лепестками (x=150..170)
  const int16_t cx = 160;
  gfx.drawPixel(cx, y - 3, colorAccent);
  gfx.drawFastHLine(cx - 2, y - 2, 5, colorAccent);
  gfx.drawFastHLine(cx - 4, y - 1, 9, colorAccent);
  gfx.drawFastHLine(cx - 5, y, 11, colorAccent);
  gfx.drawFastHLine(cx - 4, y + 1, 9, colorAccent);
  gfx.drawFastHLine(cx - 2, y + 2, 5, colorAccent);
  gfx.drawPixel(cx, y + 3, colorAccent);

  // Маленькие декоративные точки-спутники
  gfx.drawPixel(cx - 8, y, colorLine);
  gfx.drawPixel(cx + 8, y, colorLine);
  gfx.drawPixel(cx - 12, y, colorAccent);
  gfx.drawPixel(cx + 12, y, colorAccent);
}

// Отрисовка тонкого казахского бордюра в нижней части экрана
inline void drawBottomOrnamentBar(Adafruit_GFX &gfx, int16_t y, uint16_t color) {
  // Непрерывная тонкая направляющая
  gfx.drawFastHLine(16, y, 288, COLOR_GOLD_DARK);

  // Периодический мотив «ирек» / ромбовидные элементы по бокам
  for (int16_t x = 24; x <= 110; x += 16) {
    gfx.drawPixel(x, y - 2, color);
    gfx.drawPixel(x - 1, y - 1, color);
    gfx.drawPixel(x + 1, y - 1, color);
    gfx.drawPixel(x, y, color);
    gfx.drawPixel(x - 1, y + 1, color);
    gfx.drawPixel(x + 1, y + 1, color);
    gfx.drawPixel(x, y + 2, color);
  }

  for (int16_t x = 210; x <= 296; x += 16) {
    gfx.drawPixel(x, y - 2, color);
    gfx.drawPixel(x - 1, y - 1, color);
    gfx.drawPixel(x + 1, y - 1, color);
    gfx.drawPixel(x, y, color);
    gfx.drawPixel(x - 1, y + 1, color);
    gfx.drawPixel(x + 1, y + 1, color);
    gfx.drawPixel(x, y + 2, color);
  }
}

// Отрисовка тонких вертикальных боковых орнаментов
inline void drawSideBorders(Adafruit_GFX &gfx, uint16_t color) {
  // Левая боковая сторона (x=4)
  gfx.drawFastVLine(4, 34, 180, COLOR_GOLD_DARK);
  gfx.drawPixel(4, 40, color);
  gfx.drawPixel(3, 41, color);
  gfx.drawPixel(5, 41, color);
  gfx.drawPixel(4, 42, color);

  gfx.drawPixel(4, 120, color);
  gfx.drawPixel(3, 121, color);
  gfx.drawPixel(5, 121, color);
  gfx.drawPixel(4, 122, color);

  gfx.drawPixel(4, 200, color);
  gfx.drawPixel(3, 201, color);
  gfx.drawPixel(5, 201, color);
  gfx.drawPixel(4, 202, color);

  // Правая боковая сторона (x=315)
  gfx.drawFastVLine(315, 34, 180, COLOR_GOLD_DARK);
  gfx.drawPixel(315, 40, color);
  gfx.drawPixel(314, 41, color);
  gfx.drawPixel(316, 41, color);
  gfx.drawPixel(315, 42, color);

  gfx.drawPixel(315, 120, color);
  gfx.drawPixel(314, 121, color);
  gfx.drawPixel(316, 121, color);
  gfx.drawPixel(315, 122, color);

  gfx.drawPixel(315, 200, color);
  gfx.drawPixel(314, 201, color);
  gfx.drawPixel(316, 201, color);
  gfx.drawPixel(315, 202, color);
}

// Отрисовка углового золотого акцента в углах экрана
inline void drawCornerAccents(Adafruit_GFX &gfx, uint16_t color) {
  const int16_t L = 8;
  // Верхний левый угол
  gfx.drawFastHLine(6, 6, L, color);
  gfx.drawFastVLine(6, 6, L, color);

  // Верхний правый угол
  gfx.drawFastHLine(320 - 6 - L, 6, L, color);
  gfx.drawFastVLine(320 - 7, 6, L, color);

  // Нижний левый угол
  gfx.drawFastHLine(6, 240 - 7, L, color);
  gfx.drawFastVLine(6, 240 - 6 - L, L, color);

  // Нижний правый угол
  gfx.drawFastHLine(320 - 6 - L, 240 - 7, L, color);
  gfx.drawFastVLine(320 - 7, 240 - 6 - L, L, color);
}

#endif // ORNAMENT_H

