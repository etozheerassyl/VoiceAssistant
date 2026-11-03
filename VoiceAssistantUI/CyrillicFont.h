#ifndef CYRILLIC_FONT_H
#define CYRILLIC_FONT_H

#include <Arduino.h>
#include <Adafruit_GFX.h>

// ============================================================================
// 5x7 растровый шрифт с поддержкой ASCII, русского и казахского алфавитов
// Поддерживаемые буквы:
// - Все буквы кириллицы (А-Я, а-я, Ё, ё)
// - Казахские буквы: Қ, қ, Ө, ө, Ә, ә, Ғ, ғ, Ұ, ұ, Ү, ү, І, і, Ң, ң, Һ, һ
// - Символы: «, », °, •, —, ноты и базовый ASCII
// ============================================================================

static const uint8_t PROGMEM font_ru_upper[33][5] = {
  {0x7E, 0x11, 0x11, 0x11, 0x7E}, // А (0x0410)
  {0x7F, 0x49, 0x49, 0x49, 0x31}, // Б (0x0411)
  {0x7F, 0x49, 0x49, 0x49, 0x36}, // В (0x0412)
  {0x7F, 0x01, 0x01, 0x01, 0x01}, // Г (0x0413)
  {0x60, 0x3F, 0x21, 0x3F, 0x60}, // Д (0x0414)
  {0x7F, 0x49, 0x49, 0x49, 0x41}, // Е (0x0415)
  {0x77, 0x08, 0x7F, 0x08, 0x77}, // Ж (0x0416)
  {0x41, 0x49, 0x49, 0x49, 0x36}, // З (0x0417)
  {0x7F, 0x20, 0x10, 0x08, 0x7F}, // И (0x0418)
  {0x7F, 0x24, 0x12, 0x08, 0x7F}, // Й (0x0419)
  {0x7F, 0x08, 0x14, 0x22, 0x41}, // К (0x041A)
  {0x60, 0x1E, 0x01, 0x01, 0x7F}, // Л (0x041B)
  {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // М (0x041C)
  {0x7F, 0x08, 0x08, 0x08, 0x7F}, // Н (0x041D)
  {0x3E, 0x41, 0x41, 0x41, 0x3E}, // О (0x041E)
  {0x7F, 0x01, 0x01, 0x01, 0x7F}, // П (0x041F)
  {0x7F, 0x09, 0x09, 0x09, 0x06}, // Р (0x0420)
  {0x3E, 0x41, 0x41, 0x41, 0x22}, // С (0x0421)
  {0x01, 0x01, 0x7F, 0x01, 0x01}, // Т (0x0422)
  {0x03, 0x04, 0x78, 0x04, 0x03}, // У (0x0423)
  {0x1C, 0x22, 0x7F, 0x22, 0x1C}, // Ф (0x0424)
  {0x63, 0x14, 0x08, 0x14, 0x63}, // Х (0x0425)
  {0x7F, 0x40, 0x40, 0x7F, 0xE0}, // Ц (0x0426)
  {0x0F, 0x08, 0x08, 0x08, 0x7F}, // Ч (0x0427)
  {0x7F, 0x40, 0x7F, 0x40, 0x7F}, // Ш (0x0428)
  {0x7F, 0x40, 0x7F, 0x40, 0xFF}, // Щ (0x0429)
  {0x05, 0x05, 0x7F, 0x49, 0x36}, // Ъ (0x042A)
  {0x7F, 0x49, 0x36, 0x00, 0x7F}, // Ы (0x042B)
  {0x7F, 0x49, 0x49, 0x49, 0x36}, // Ь (0x042C)
  {0x22, 0x49, 0x49, 0x49, 0x3E}, // Э (0x042D)
  {0x7F, 0x08, 0x3E, 0x41, 0x3E}, // Ю (0x042E)
  {0x46, 0x29, 0x19, 0x09, 0x7F}, // Я (0x042F)
  {0x7F, 0x49, 0x49, 0x49, 0x41}  // Ё (0x0401)
};

static const uint8_t PROGMEM font_ru_lower[33][5] = {
  {0x20, 0x54, 0x54, 0x54, 0x78}, // а (0x0430)
  {0x3F, 0x48, 0x48, 0x48, 0x30}, // б (0x0431)
  {0x7C, 0x54, 0x54, 0x54, 0x28}, // в (0x0432)
  {0x7C, 0x04, 0x04, 0x04, 0x04}, // г (0x0433)
  {0x60, 0x3C, 0x24, 0x3C, 0x60}, // д (0x0434)
  {0x38, 0x54, 0x54, 0x54, 0x18}, // е (0x0435)
  {0x6C, 0x10, 0x7C, 0x10, 0x6C}, // ж (0x0436)
  {0x44, 0x54, 0x54, 0x54, 0x28}, // з (0x0437)
  {0x7C, 0x20, 0x10, 0x08, 0x7C}, // и (0x0438)
  {0x7C, 0x24, 0x12, 0x08, 0x7C}, // й (0x0439)
  {0x7C, 0x10, 0x28, 0x44, 0x00}, // к (0x043A)
  {0x60, 0x1C, 0x04, 0x04, 0x7C}, // л (0x043B)
  {0x7C, 0x08, 0x10, 0x08, 0x7C}, // м (0x043C)
  {0x7C, 0x10, 0x10, 0x10, 0x7C}, // н (0x043D)
  {0x38, 0x44, 0x44, 0x44, 0x38}, // о (0x043E)
  {0x7C, 0x04, 0x04, 0x04, 0x7C}, // п (0x043F)
  {0x7C, 0x14, 0x14, 0x14, 0x08}, // р (0x0440)
  {0x38, 0x44, 0x44, 0x48, 0x20}, // с (0x0441)
  {0x04, 0x04, 0x7C, 0x04, 0x04}, // т (0x0442)
  {0x0C, 0x50, 0x50, 0x50, 0x3C}, // у (0x0443)
  {0x18, 0x24, 0x7E, 0x24, 0x18}, // ф (0x0444)
  {0x44, 0x28, 0x10, 0x28, 0x44}, // х (0x0445)
  {0x7C, 0x40, 0x40, 0x7C, 0xC0}, // ц (0x0446)
  {0x0C, 0x10, 0x10, 0x10, 0x7C}, // ч (0x0447)
  {0x7C, 0x40, 0x7C, 0x40, 0x7C}, // ш (0x0448)
  {0x7C, 0x40, 0x7C, 0x40, 0xFC}, // щ (0x0449)
  {0x08, 0x08, 0x7C, 0x54, 0x28}, // ъ (0x044A)
  {0x7C, 0x50, 0x28, 0x00, 0x7C}, // ы (0x044B)
  {0x7C, 0x50, 0x50, 0x50, 0x20}, // ь (0x044C)
  {0x28, 0x54, 0x54, 0x54, 0x38}, // э (0x044D)
  {0x7C, 0x10, 0x38, 0x44, 0x38}, // ю (0x044E)
  {0x48, 0x34, 0x1C, 0x14, 0x7C}, // я (0x044F)
  {0x38, 0x54, 0xD4, 0x54, 0x18}  // ё (0x0451)
};

// ============================================================================
// Казахские специфические буквы
// ============================================================================
static const uint8_t PROGMEM glyph_KZ_Q_upper[5] = {0x7F, 0x08, 0x14, 0x22, 0xC1}; // Қ (0x049A)
static const uint8_t PROGMEM glyph_KZ_q_lower[5] = {0x7C, 0x10, 0x28, 0x44, 0xC0}; // қ (0x049B)

static const uint8_t PROGMEM glyph_KZ_O_upper[5] = {0x3E, 0x49, 0x49, 0x49, 0x3E}; // Ө (0x04E8)
static const uint8_t PROGMEM glyph_KZ_o_lower[5] = {0x38, 0x54, 0x54, 0x54, 0x38}; // ө (0x04E9)

static const uint8_t PROGMEM glyph_KZ_A_upper[5] = {0x3E, 0x49, 0x49, 0x49, 0x22}; // Ә (0x04D8)
static const uint8_t PROGMEM glyph_KZ_a_lower[5] = {0x38, 0x54, 0x54, 0x54, 0x28}; // ә (0x04D9)

static const uint8_t PROGMEM glyph_KZ_G_upper[5] = {0x7F, 0x09, 0x09, 0x09, 0x01}; // Ғ (0x0492)
static const uint8_t PROGMEM glyph_KZ_g_lower[5] = {0x7C, 0x14, 0x14, 0x14, 0x04}; // ғ (0x0493)

static const uint8_t PROGMEM glyph_KZ_U_upper[5] = {0x0B, 0x0C, 0x78, 0x0C, 0x0B}; // Ұ (0x04B0)
static const uint8_t PROGMEM glyph_KZ_u_lower[5] = {0x1C, 0x50, 0x70, 0x50, 0x3C}; // ұ (0x04B1)

static const uint8_t PROGMEM glyph_KZ_Y_upper[5] = {0x03, 0x04, 0x78, 0x04, 0x03}; // Ү (0x04AE)
static const uint8_t PROGMEM glyph_KZ_y_lower[5] = {0x06, 0x08, 0x70, 0x08, 0x06}; // ү (0x04AF)

static const uint8_t PROGMEM glyph_KZ_I_upper[5] = {0x00, 0x41, 0x7F, 0x41, 0x00}; // І (0x0406)
static const uint8_t PROGMEM glyph_KZ_i_lower[5] = {0x00, 0x00, 0x5D, 0x00, 0x00}; // і (0x0456)

static const uint8_t PROGMEM glyph_KZ_N_upper[5] = {0x7F, 0x08, 0x08, 0x08, 0xFF}; // Ң (0x04A2)
static const uint8_t PROGMEM glyph_KZ_n_lower[5] = {0x7C, 0x10, 0x10, 0x10, 0xFC}; // ң (0x04A3)

static const uint8_t PROGMEM glyph_KZ_H_upper[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F}; // Һ (0x04BA)
static const uint8_t PROGMEM glyph_KZ_h_lower[5] = {0x7C, 0x08, 0x04, 0x04, 0x78}; // һ (0x04BB)

// Знаки препинания и символы
static const uint8_t PROGMEM glyph_DEGREE[5]     = {0x00, 0x06, 0x09, 0x09, 0x06}; // ° (0x00B0)
static const uint8_t PROGMEM glyph_QUOTE_L[5]    = {0x14, 0x28, 0x50, 0x28, 0x14}; // « (0x00AB)
static const uint8_t PROGMEM glyph_QUOTE_R[5]    = {0x50, 0x28, 0x14, 0x28, 0x50}; // » (0x00BB)
static const uint8_t PROGMEM glyph_NOTE[5]       = {0x04, 0x0E, 0x05, 0x7F, 0x00}; // Нота ♫
static const uint8_t PROGMEM glyph_BULLET[5]     = {0x00, 0x18, 0x3C, 0x18, 0x00}; // • (0x2022)

// ============================================================================
// Отрисовка отдельного символа
// ============================================================================
inline void drawGlyph5x7(Adafruit_GFX &gfx, int16_t x, int16_t y, uint16_t unicode, uint16_t color, uint16_t bg, uint8_t size) {
  // Нормализация типографских символов (тире, кавычки, пробелы) в поддерживаемые глифы
  if (unicode == 0x2013 || unicode == 0x2014 || unicode == 0x2012 || unicode == 0x2011 || unicode == 0x2212) {
    unicode = '-';
  } else if (unicode == 0x201C || unicode == 0x201D || unicode == 0x201E || unicode == 0x201F) {
    unicode = '"';
  } else if (unicode == 0x2018 || unicode == 0x2019 || unicode == 0x02BC) {
    unicode = '\'';
  } else if (unicode == 0x00A0) {
    unicode = ' ';
  } else if (unicode == 0x2116) {
    unicode = '#';
  } else if (unicode == 0x2026) {
    unicode = '.';
  }

  // Стандартные символы ASCII (буквы, цифры, базовая пунктуация)
  if (unicode < 128) {
    gfx.drawChar(x, y, (unsigned char)unicode, color, bg, size);
    return;
  }

  const uint8_t *bitmap = nullptr;

  // 1. Русский алфавит
  if (unicode >= 0x0410 && unicode <= 0x042F) {
    bitmap = font_ru_upper[unicode - 0x0410];
  } else if (unicode == 0x0401) { // Ё
    bitmap = font_ru_upper[32];
  } else if (unicode >= 0x0430 && unicode <= 0x044F) {
    bitmap = font_ru_lower[unicode - 0x0430];
  } else if (unicode == 0x0451) { // ё
    bitmap = font_ru_lower[32];
  }
  // 2. Казахские буквы
  else if (unicode == 0x049A) bitmap = glyph_KZ_Q_upper; // Қ
  else if (unicode == 0x049B) bitmap = glyph_KZ_q_lower; // қ
  else if (unicode == 0x04E8) bitmap = glyph_KZ_O_upper; // Ө
  else if (unicode == 0x04E9) bitmap = glyph_KZ_o_lower; // ө
  else if (unicode == 0x04D8) bitmap = glyph_KZ_A_upper; // Ә
  else if (unicode == 0x04D9) bitmap = glyph_KZ_a_lower; // ә
  else if (unicode == 0x0492) bitmap = glyph_KZ_G_upper; // Ғ
  else if (unicode == 0x0493) bitmap = glyph_KZ_g_lower; // ғ
  else if (unicode == 0x04B0) bitmap = glyph_KZ_U_upper; // Ұ
  else if (unicode == 0x04B1) bitmap = glyph_KZ_u_lower; // ұ
  else if (unicode == 0x04AE) bitmap = glyph_KZ_Y_upper; // Ү
  else if (unicode == 0x04AF) bitmap = glyph_KZ_y_lower; // ү
  else if (unicode == 0x0406) bitmap = glyph_KZ_I_upper; // І
  else if (unicode == 0x0456) bitmap = glyph_KZ_i_lower; // і
  else if (unicode == 0x04A2) bitmap = glyph_KZ_N_upper; // Ң
  else if (unicode == 0x04A3) bitmap = glyph_KZ_n_lower; // ң
  else if (unicode == 0x04BA) bitmap = glyph_KZ_H_upper; // Һ
  else if (unicode == 0x04BB) bitmap = glyph_KZ_h_lower; // һ
  // 3. Дополнительные символы
  else if (unicode == 0x00B0) bitmap = glyph_DEGREE;     // °
  else if (unicode == 0x00AB) bitmap = glyph_QUOTE_L;    // «
  else if (unicode == 0x00BB) bitmap = glyph_QUOTE_R;    // »
  else if (unicode == 0x2022 || unicode == 0x00B7) bitmap = glyph_BULLET; // •
  else if (unicode == 0x266A || unicode == 0x266B) bitmap = glyph_NOTE; // ♫

  if (bitmap) {
    for (int8_t i = 0; i < 5; i++) {
      uint8_t line = pgm_read_byte(&bitmap[i]);
      for (int8_t j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) {
          if (size == 1) gfx.drawPixel(x + i, y + j, color);
          else gfx.fillRect(x + i * size, y + j * size, size, size, color);
        } else if (bg != color) {
          if (size == 1) gfx.drawPixel(x + i, y + j, bg);
          else gfx.fillRect(x + i * size, y + j * size, size, size, bg);
        }
      }
    }
    // Межсимвольный зазор шириной 1 пиксель * size
    if (bg != color) {
      if (size == 1) {
        for (int8_t j = 0; j < 8; j++) gfx.drawPixel(x + 5, y + j, bg);
      } else {
        gfx.fillRect(x + 5 * size, y, size, 8 * size, bg);
      }
    }
  } else {
    // Вместо '?' рисуем аккуратный пробел, чтобы незнакомые эмодзи не портили заголовок
    gfx.drawChar(x, y, ' ', color, bg, size);
  }
}

// ============================================================================
// Утилиты работы с UTF-8 без разрыва двухбайтовых кириллических символов
// ============================================================================
inline const char* nextUTF8Char(const char *p) {
  if (!p || !*p) return p;
  const uint8_t c = (uint8_t)*p;
  if ((c & 0x80) == 0) return p + 1;
  if ((c & 0xE0) == 0xC0) return p + (p[1] ? 2 : 1);
  if ((c & 0xF0) == 0xE0) {
    if (!p[1]) return p + 1;
    if (!p[2]) return p + 2;
    return p + 3;
  }
  if ((c & 0xF8) == 0xF0) {
    for (int i = 1; i <= 3; i++) {
      if (!p[i]) return p + i;
    }
    return p + 4;
  }
  return p + 1;
}

inline size_t countUTF8Chars(const char *str) {
  if (!str) return 0;
  size_t count = 0;
  const char *p = str;
  while (*p) {
    p = nextUTF8Char(p);
    count++;
  }
  return count;
}

// Безопасная обрезка UTF-8 строки по числу символов (а не байтов!)
inline void truncateUTF8(const char *src, char *dst, size_t dstSize, size_t maxChars) {
  if (!dst || dstSize == 0) return;
  dst[0] = '\0';
  if (!src || !*src || maxChars == 0) return;

  size_t totalChars = countUTF8Chars(src);
  if (totalChars <= maxChars) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
    return;
  }

  size_t keepChars = (maxChars > 2) ? (maxChars - 2) : maxChars;
  const char *p = src;
  size_t charsCopied = 0;
  while (*p && charsCopied < keepChars) {
    const char *next = nextUTF8Char(p);
    size_t charBytes = (size_t)(next - p);
    if ((size_t)(p - src) + charBytes + 3 >= dstSize) break;
    p = next;
    charsCopied++;
  }

  size_t byteLen = (size_t)(p - src);
  memcpy(dst, src, byteLen);
  if (maxChars > 2 && byteLen + 2 < dstSize) {
    dst[byteLen++] = '.';
    dst[byteLen++] = '.';
  }
  dst[byteLen] = '\0';
}

// Умное разбиение заголовка UTF-8 на 2 строки по словам без разрыва кириллицы
inline void wrapUTF8TwoLines(const char *src, char *line1, size_t l1Size,
                             char *line2, size_t l2Size, size_t maxCharsPerLine) {
  if (line1 && l1Size > 0) line1[0] = '\0';
  if (line2 && l2Size > 0) line2[0] = '\0';
  if (!src || !*src) return;

  // Пропускаем начальные пробелы
  while (*src == ' ') src++;

  size_t totalChars = countUTF8Chars(src);
  if (totalChars <= maxCharsPerLine) {
    strncpy(line1, src, l1Size - 1);
    line1[l1Size - 1] = '\0';
    return;
  }

  // Ищем границу первой строки (предпочтительно по пробелу)
  const char *p = src;
  const char *lastSpace = nullptr;
  size_t charIdx = 0;

  while (*p && charIdx < maxCharsPerLine) {
    if (*p == ' ' && charIdx >= maxCharsPerLine / 2) {
      lastSpace = p;
    }
    p = nextUTF8Char(p);
    charIdx++;
  }

  const char *splitPos = p;
  if (*p && *p != ' ' && lastSpace != nullptr) {
    splitPos = lastSpace;
  }

  size_t l1Bytes = (size_t)(splitPos - src);
  if (l1Bytes >= l1Size) l1Bytes = l1Size - 1;
  memcpy(line1, src, l1Bytes);
  line1[l1Bytes] = '\0';

  // Переходим ко второй строке, пропуская пробелы
  const char *rest = splitPos;
  while (*rest == ' ') rest++;
  if (*rest) {
    truncateUTF8(rest, line2, l2Size, maxCharsPerLine);
  }
}

// Форматирование времени (MM:SS или H:MM:SS для длинных видео)
inline void formatTimeHMS(uint32_t sec, char *buf, size_t bufSize) {
  if (sec >= 3600) {
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    snprintf(buf, bufSize, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
  } else {
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, bufSize, "%02u:%02u", (unsigned)m, (unsigned)s);
  }
}

// ============================================================================
// Измерение ширины UTF-8 строки в пикселях
// ============================================================================
inline int16_t measureStringUTF8(const char *str, uint8_t size) {
  int16_t w = 0;
  const uint8_t *p = (const uint8_t*)str;
  while (*p) {
    if ((*p & 0x80) == 0) {
      p++;
    } else if ((*p & 0xE0) == 0xC0) {
      p += (p[1] ? 2 : 1);
    } else if ((*p & 0xF0) == 0xE0) {
      p += (p[1] && p[2]) ? 3 : 1;
    } else if ((*p & 0xF8) == 0xF0) {
      p += 4;
    } else {
      p++;
    }
    w += 6 * size;
  }
  return (w > 0) ? (w - size) : 0;
}

// ============================================================================
// Отрисовка UTF-8 строки с поддержкой русского, казахского и латиницы
// ============================================================================
inline void drawStringUTF8(Adafruit_GFX &gfx, int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size, bool center = false) {
  if (center) {
    int16_t textWidth = measureStringUTF8(str, size);
    x = (gfx.width() - textWidth) / 2;
    if (x < 18) x = 18;
  }

  int16_t curX = x;
  const uint8_t *p = (const uint8_t*)str;

  while (*p) {
    uint16_t u = 0;
    if ((*p & 0x80) == 0) {
      u = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      if (!*(p + 1)) break;
      u = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0) {
      if (!*(p + 1) || !*(p + 2)) break;
      u = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0) {
      p += 4;
      u = ' ';
    } else {
      p++;
      continue;
    }

    if (u == 0x0A) { // символ перевода строки '\n'
      curX = x;
      y += 8 * size + 2;
      continue;
    }

    // Жёсткая защита границ экрана (x = 14..305), чтобы текст никогда не выходил за плашки и орнамент
    if (curX + 6 * size > 305) {
      break;
    }
    if (curX >= 14) {
      drawGlyph5x7(gfx, curX, y, u, color, bg, size);
    }
    curX += 6 * size;
  }
}

#endif // CYRILLIC_FONT_H
