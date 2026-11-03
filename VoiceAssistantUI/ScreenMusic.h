#ifndef SCREEN_MUSIC_H
#define SCREEN_MUSIC_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <math.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"
#include "UIComponents.h"

// ============================================================================
// Музыкальный экран (Music Screen: Яндекс Музыка / Apple Music / YouTube / Spotify)
// Компоновка 320x240:
// - Header: «МУЗЫКАЛЫҚ ПЛЕЕР» + Wi-Fi + Время (y=0..24)
// - Название трека и исполнитель с безопасной обрезкой UTF-8 (y=30..67)
// - Реактивный 16-полосный спектральный эквалайзер в золотистых тонах (y=74..119)
//   с быстрым подъёмом, плавным спадом и эффектом удержания/затухания пиков
// - Индикатор прогресса трека (полоса, таймер текущий и общий) (y=132..146)
// - Кнопки управления (Назад, Play/Pause в золотом круге, Вперед) (y=154..196)
// - Тонкий орнамент и пагинация (страница 2 из 4) (y=214..234)
// ============================================================================

#define EQ_BARS           16
#define EQ_MIN_HEIGHT     3
#define EQ_MAX_HEIGHT     40
#define EQ_BAR_WIDTH      10
#define EQ_BAR_GAP        4
#define EQ_TOTAL_WIDTH    (EQ_BARS * EQ_BAR_WIDTH + (EQ_BARS - 1) * EQ_BAR_GAP) // 220 px
#define EQ_START_X        ((SCREEN_WIDTH - EQ_TOTAL_WIDTH) / 2)                  // 50 px
#define EQ_BASE_Y         118
#define EQ_STREAM_TIMEOUT 2200 // мс до аварийного затухания при потере потока пакетов

class ScreenMusic {
private:
  // Состояние 16-полосного реактивного эквалайзера
  uint8_t targetBands[EQ_BARS];      // Входные уровни частотных диапазонов (0..100) от MacBook
  float   smoothHeights[EQ_BARS];    // Сглаженные текущие высоты столбцов (в пикселях)
  float   peakHeights[EQ_BARS];      // Позиции плавающих пиковых маркеров (в пикселях)
  uint8_t peakHoldFrames[EQ_BARS];   // Счётчик удержания пика на вершине
  uint8_t prevHeights[EQ_BARS];      // Предыдущие отрисованные высоты столбцов (для дифф. рендера)
  uint8_t prevPeakHeights[EQ_BARS];  // Предыдущие отрисованные пики
  bool    forceEqRedraw;             // Принудительная первичная отрисовка при открытии экрана
  bool    hasAudioSignal;            // Флаг активности аудиопотока
  unsigned long lastEqPacketMs;      // Время получения последнего пакета спектра

  // Медиаданные
  bool isPlaying;
  uint32_t trackSeconds;
  uint32_t trackDuration;
  char currentTitle[160];
  char currentArtist[96];
  char currentSource[40];
  bool isConnected;

  bool infoDirty;
  bool statusDirty;
  bool progressDirty;

  // Отрисовка многослойного золотого столбца эквалайзера (без очистки неизменной части)
  void drawGoldenBar(Adafruit_GFX &gfx, int16_t bx, int16_t baseY, uint8_t height) {
    if (height == 0) return;

    // 1. Нижний базовый ярус (тёмное благородное золото): 1..14 px
    uint8_t tier1 = (height > 14) ? 14 : height;
    gfx.fillRect(bx, baseY - tier1, EQ_BAR_WIDTH, tier1, COLOR_GOLD_DARK);

    // 2. Средний ярус (насыщенное янтарное золото): 15..27 px
    if (height > 14) {
      uint8_t tier2 = (height > 27) ? (27 - 14) : (height - 14);
      gfx.fillRect(bx, baseY - 14 - tier2, EQ_BAR_WIDTH, tier2, COLOR_GOLD_MID);
    }

    // 3. Верхний ярус (яркое фирменное золото): 28..40 px
    if (height > 27) {
      uint8_t tier3 = height - 27;
      gfx.fillRect(bx, baseY - height, EQ_BAR_WIDTH, tier3, COLOR_GOLD);
    }

    // 4. Светлая золотая кромка на вершине столбца
    uint16_t capColor = (height > EQ_MIN_HEIGHT + 2) ? COLOR_GOLD_LIGHT : COLOR_GOLD;
    gfx.drawFastHLine(bx, baseY - height, EQ_BAR_WIDTH, capColor);
  }

public:
  ScreenMusic()
    : forceEqRedraw(true), hasAudioSignal(false), lastEqPacketMs(0),
      isPlaying(false), trackSeconds(0), trackDuration(0), isConnected(false),
      infoDirty(true), statusDirty(true), progressDirty(true) {
    strncpy(currentTitle, "Ойнатылмай тұр", sizeof(currentTitle));
    strncpy(currentArtist, "MacBook Air • Күтуде", sizeof(currentArtist));
    strncpy(currentSource, "macOS", sizeof(currentSource));
    for (uint8_t i = 0; i < EQ_BARS; i++) {
      targetBands[i] = 0;
      smoothHeights[i] = (float)EQ_MIN_HEIGHT;
      peakHeights[i] = (float)EQ_MIN_HEIGHT;
      peakHoldFrames[i] = 0;
      prevHeights[i] = 0;
      prevPeakHeights[i] = 0;
    }
  }

  // Приём реальных уровней 16 частотных диапазонов (0..100) от MacBook
  void setEqualizerBands(const uint8_t *bands, uint8_t count, bool active, unsigned long packetMs) {
    if (!bands) return;
    bool anyPositive = false;
    for (uint8_t i = 0; i < EQ_BARS; i++) {
      uint8_t v = (i < count) ? bands[i] : 0;
      if (v > 100) v = 100;
      targetBands[i] = v;
      if (v > 0) anyPositive = true;
    }
    isConnected = true;
    hasAudioSignal = active || anyPositive;
    lastEqPacketMs = packetMs;
  }

  void setTrackData(const char *title, const char *artist, const char *source,
                    uint32_t pos, uint32_t dur, bool playing, bool connected) {
    if (title && strlen(title) > 0) {
      if (strncmp(currentTitle, title, sizeof(currentTitle) - 1) != 0) {
        strncpy(currentTitle, title, sizeof(currentTitle) - 1);
        currentTitle[sizeof(currentTitle) - 1] = '\0';
        infoDirty = true;
      }
    } else if (!playing) {
      if (strcmp(currentTitle, "Ойнатылмай тұр") != 0) {
        strncpy(currentTitle, "Ойнатылмай тұр", sizeof(currentTitle) - 1);
        infoDirty = true;
      }
    }

    if (artist && strlen(artist) > 0) {
      if (strncmp(currentArtist, artist, sizeof(currentArtist) - 1) != 0) {
        strncpy(currentArtist, artist, sizeof(currentArtist) - 1);
        currentArtist[sizeof(currentArtist) - 1] = '\0';
        infoDirty = true;
      }
    } else if (!playing) {
      if (strcmp(currentArtist, "MacBook Air • Күтуде") != 0) {
        strncpy(currentArtist, "MacBook Air • Күтуде", sizeof(currentArtist) - 1);
        infoDirty = true;
      }
    }

    if (source && strlen(source) > 0) {
      if (strncmp(currentSource, source, sizeof(currentSource) - 1) != 0) {
        strncpy(currentSource, source, sizeof(currentSource) - 1);
        currentSource[sizeof(currentSource) - 1] = '\0';
        infoDirty = true;
      }
    }

    if (isPlaying != playing || isConnected != connected) {
      isPlaying = playing;
      isConnected = connected;
      statusDirty = true;
      progressDirty = true;
      if (!connected) {
        for (uint8_t i = 0; i < EQ_BARS; i++) {
          targetBands[i] = 0;
        }
        hasAudioSignal = false;
      }
    }

    if (trackDuration != dur) {
      trackDuration = dur;
      progressDirty = true;
    }

    int32_t diff = (int32_t)pos - (int32_t)trackSeconds;
    if (diff < 0) diff = -diff;

    if (infoDirty || statusDirty || !isPlaying || diff >= 2 || trackSeconds == 0) {
      if (trackSeconds != pos) {
        trackSeconds = pos;
        progressDirty = true;
      }
    }
  }

  bool getPlaying() const { return isPlaying; }
  void togglePlay() {
    isPlaying = !isPlaying;
    statusDirty = true;
  }
  void setPlaying(bool play) {
    isPlaying = play;
    statusDirty = true;
  }

  void seekLocal(int32_t deltaSec, Adafruit_GFX &gfx) {
    int32_t nextPos = (int32_t)trackSeconds + deltaSec;
    if (nextPos < 0) nextPos = 0;
    if (trackDuration > 0 && (uint32_t)nextPos > trackDuration) {
      nextPos = (int32_t)trackDuration;
    }
    trackSeconds = (uint32_t)nextPos;
    drawProgressBar(gfx);
  }

  void drawStatic(Adafruit_GFX &gfx, const char *timeHeaderStr, bool wifiOk) {
    gfx.fillScreen(COLOR_BG);

    // 1. Шапка
    drawHeader(gfx, "МУЗЫКАЛЫҚ ПЛЕЕР", timeHeaderStr, wifiOk);

    // 2. Боковые тонкие казахские орнаменты
    drawSideBorders(gfx, COLOR_GOLD);
    drawCornerAccents(gfx, COLOR_GOLD_DARK);

    // 3. Информационный блок трека (y=30..67)
    drawTrackInfo(gfx);

    // 4. Опорная золотая линия области 16-полосного эквалайзера (y=74..119)
    gfx.drawFastHLine(EQ_START_X - 4, EQ_BASE_Y + 1, EQ_TOTAL_WIDTH + 8, COLOR_GOLD_DARK);
    gfx.drawPixel(EQ_START_X - 5, EQ_BASE_Y + 1, COLOR_GOLD);
    gfx.drawPixel(EQ_START_X + EQ_TOTAL_WIDTH + 4, EQ_BASE_Y + 1, COLOR_GOLD);

    // Сброс кэша отрисовки столбцов для мгновенного вывода без мерцания
    for (uint8_t i = 0; i < EQ_BARS; i++) {
      prevHeights[i] = 0;
      prevPeakHeights[i] = 0;
    }
    forceEqRedraw = true;
    updateEqualizer(gfx);

    // 5. Прогресс-бар трека (y=132..146)
    drawProgressBar(gfx);

    // 6. Кнопки управления плеером (y=154..196)
    drawControls(gfx);

    // 7. Нижний казахский орнамент и пагинация (индекс 1 из 4)
    drawBottomOrnamentBar(gfx, 214, COLOR_GOLD);
    drawPagination(gfx, 1, SCREEN_COUNT);

    infoDirty = false;
    statusDirty = false;
    progressDirty = false;
  }

  // Отрисовка текстовой информации трека с защитой UTF-8
  void drawTrackInfo(Adafruit_GFX &gfx) {
    gfx.fillRect(18, 30, 284, 38, COLOR_BG);

    char truncTitle[80];
    truncateUTF8(currentTitle, truncTitle, sizeof(truncTitle), 22);

    char artistLine[120];
    if (strlen(currentSource) > 0 && strcmp(currentSource, "macOS") != 0 &&
        strstr(currentArtist, currentSource) == nullptr) {
      snprintf(artistLine, sizeof(artistLine), "%s • %s", currentArtist, currentSource);
    } else {
      strncpy(artistLine, currentArtist, sizeof(artistLine) - 1);
      artistLine[sizeof(artistLine) - 1] = '\0';
    }

    char truncArtist[96];
    truncateUTF8(artistLine, truncArtist, sizeof(truncArtist), 38);

    drawStringUTF8(gfx, 0, 32, truncTitle, COLOR_GOLD, COLOR_BG, 2, true);
    drawStringUTF8(gfx, 0, 52, truncArtist, COLOR_BEIGE, COLOR_BG, 1, true);
  }

  // Отрисовка полосы прогресса трека без остаточных следов бегунка
  void drawProgressBar(Adafruit_GFX &gfx) {
    const int16_t barX = 66;
    const int16_t barY = 136;
    const int16_t barW = 188;
    const int16_t barH = 4;

    // Временные метки слева и справа
    char curTime[12];
    formatTimeHMS(trackSeconds, curTime, sizeof(curTime));

    char totalTime[12];
    formatTimeHMS(trackDuration, totalTime, sizeof(totalTime));

    gfx.fillRect(16, barY - 3, 46, 12, COLOR_BG);
    drawStringUTF8(gfx, 18, barY - 2, curTime, COLOR_TEXT_MUTED, COLOR_BG, 1);

    gfx.fillRect(258, barY - 3, 46, 12, COLOR_BG);
    drawStringUTF8(gfx, 260, barY - 2, totalTime, COLOR_TEXT_MUTED, COLOR_BG, 1);

    // Очистка полосы и кружка бегунка
    gfx.fillRect(barX - 5, barY - 3, barW + 10, 11, COLOR_BG);

    // Фоновая линия шкалы
    gfx.fillRoundRect(barX, barY, barW, barH, 2, COLOR_CARD_BG);
    gfx.drawRoundRect(barX, barY, barW, barH, 2, COLOR_CARD_BORDER);

    // Заполненная часть прогресса
    int16_t fillW = 0;
    if (trackDuration > 0) {
      uint32_t clampedPos = (trackSeconds <= trackDuration) ? trackSeconds : trackDuration;
      fillW = (int16_t)((uint64_t)barW * clampedPos / trackDuration);
    }
    if (fillW > 0) {
      gfx.fillRoundRect(barX, barY, fillW, barH, 2, COLOR_GOLD);
    }

    // Бегунок-точка на конце заполнения
    int16_t knobX = barX + fillW;
    if (knobX > barX + barW) knobX = barX + barW;
    gfx.fillCircle(knobX, barY + 2, 4, COLOR_GOLD_LIGHT);
    gfx.drawCircle(knobX, barY + 2, 4, COLOR_GOLD);
  }

  // Отрисовка стилизованных кнопок управления
  void drawControls(Adafruit_GFX &gfx) {
    const int16_t cY = 175;

    // Кнопка Prev: |<<
    const int16_t prevX = 90;
    gfx.fillCircle(prevX + 10, cY, 15, COLOR_CARD_BG);
    gfx.drawCircle(prevX + 10, cY, 15, COLOR_CARD_BORDER);
    gfx.drawFastVLine(prevX + 3, cY - 5, 11, COLOR_GOLD);
    gfx.fillTriangle(prevX + 11, cY - 5, prevX + 11, cY + 5, prevX + 5, cY, COLOR_GOLD);
    gfx.fillTriangle(prevX + 17, cY - 5, prevX + 17, cY + 5, prevX + 11, cY, COLOR_GOLD);

    // Центральная кнопка Play/Pause в золотом круге (cx=160, cy=175, r=20)
    gfx.fillCircle(160, cY, 20, COLOR_GOLD);
    gfx.drawCircle(160, cY, 22, COLOR_GOLD_LIGHT);

    if (isPlaying) {
      // Иконка Pause: две аккуратные вертикальные полосы
      gfx.fillRect(153, cY - 8, 4, 16, COLOR_BG);
      gfx.fillRect(163, cY - 8, 4, 16, COLOR_BG);
    } else {
      // Иконка Play: треугольник вправо
      gfx.fillTriangle(154, cY - 9, 154, cY + 9, 171, cY, COLOR_BG);
    }

    // Кнопка Next: >>|
    const int16_t nextX = 220;
    gfx.fillCircle(nextX + 10, cY, 15, COLOR_CARD_BG);
    gfx.drawCircle(nextX + 10, cY, 15, COLOR_CARD_BORDER);
    gfx.fillTriangle(nextX + 3, cY - 5, nextX + 3, cY + 5, nextX + 9, cY, COLOR_GOLD);
    gfx.fillTriangle(nextX + 9, cY - 5, nextX + 9, cY + 5, nextX + 15, cY, COLOR_GOLD);
    gfx.drawFastVLine(nextX + 17, cY - 5, 11, COLOR_GOLD);
  }

  void updatePlayButton(Adafruit_GFX &gfx) {
    drawControls(gfx);
  }

  // ==========================================================================
  // Реактивное обновление 16-полосного эквалайзера (без мерцания и фейковой анимации)
  // ==========================================================================
  void updateEqualizer(Adafruit_GFX &gfx) {
    unsigned long now = millis();
    bool streamTimedOut = (lastEqPacketMs == 0) || ((now - lastEqPacketMs) > EQ_STREAM_TIMEOUT);
    bool shouldDecayToMin = (!isConnected) || streamTimedOut;

    for (uint8_t i = 0; i < EQ_BARS; i++) {
      uint8_t bandLevel = shouldDecayToMin ? 0 : targetBands[i];

      // Вычисляем целевую высоту из реального уровня частотного диапазона (0..100)
      float targetH = (float)EQ_MIN_HEIGHT;
      if (bandLevel > 0) {
        targetH = (float)EQ_MIN_HEIGHT + ((float)bandLevel * (float)(EQ_MAX_HEIGHT - EQ_MIN_HEIGHT)) / 100.0f;
      }

      // Асимметричное сглаживание: быстрый подъём (атака) и плавное опускание (спад)
      if (targetH >= smoothHeights[i]) {
        smoothHeights[i] += (targetH - smoothHeights[i]) * 0.65f;
      } else {
        float drop = (smoothHeights[i] - targetH) * 0.24f;
        if (drop < 0.45f) drop = 0.45f;
        smoothHeights[i] -= drop;
        if (smoothHeights[i] < targetH) {
          smoothHeights[i] = targetH;
        }
      }

      if (smoothHeights[i] < (float)EQ_MIN_HEIGHT) smoothHeights[i] = (float)EQ_MIN_HEIGHT;
      if (smoothHeights[i] > (float)EQ_MAX_HEIGHT) smoothHeights[i] = (float)EQ_MAX_HEIGHT;

      uint8_t curH = (uint8_t)roundf(smoothHeights[i]);
      if (curH < EQ_MIN_HEIGHT) curH = EQ_MIN_HEIGHT;
      if (curH > EQ_MAX_HEIGHT) curH = EQ_MAX_HEIGHT;

      // Логика удержания и плавного затухания пиков (Peak Hold & Decay)
      if ((float)curH >= peakHeights[i]) {
        peakHeights[i] = (float)curH;
        peakHoldFrames[i] = 7; // Удерживаем пик ~210 мс
      } else if (peakHoldFrames[i] > 0) {
        peakHoldFrames[i]--;
      } else {
        peakHeights[i] -= 0.70f;
        if (peakHeights[i] < (float)curH) {
          peakHeights[i] = (float)curH;
        }
      }

      uint8_t curPeakH = (uint8_t)roundf(peakHeights[i]);
      if (curPeakH < curH) curPeakH = curH;
      if (curPeakH > EQ_MAX_HEIGHT) curPeakH = EQ_MAX_HEIGHT;

      // Пропускаем SPI-обращение, если ни высота столбца, ни пик не изменились
      if (!forceEqRedraw && curH == prevHeights[i] && curPeakH == prevPeakHeights[i]) {
        continue;
      }

      int16_t bx = EQ_START_X + i * (EQ_BAR_WIDTH + EQ_BAR_GAP);
      uint8_t oldH = prevHeights[i];
      uint8_t oldPeakH = prevPeakHeights[i];

      // 1. Стираем старый маркер пика, если он был выше столбца и сместился
      if (oldPeakH > oldH + 1 && oldPeakH != curPeakH) {
        int16_t oldPeakY = EQ_BASE_Y - oldPeakH - 2;
        if (oldPeakY >= 70) {
          gfx.fillRect(bx, oldPeakY, EQ_BAR_WIDTH, 2, COLOR_BG);
        }
      }

      // 2. Если столбец опустился — стираем ТОЛЬКО освободившуюся верхнюю часть
      if (curH < oldH) {
        gfx.fillRect(bx, EQ_BASE_Y - oldH, EQ_BAR_WIDTH, oldH - curH, COLOR_BG);
      }

      // 3. Отрисовываем столбец в золотистых оттенках
      drawGoldenBar(gfx, bx, EQ_BASE_Y, curH);

      // 4. Отрисовываем плавающий маркер пика (если он отделен от вершины столбца)
      if (curPeakH > curH + 1 && curPeakH > EQ_MIN_HEIGHT + 2) {
        int16_t peakY = EQ_BASE_Y - curPeakH - 2;
        if (peakY >= 70) {
          gfx.drawFastHLine(bx, peakY, EQ_BAR_WIDTH, COLOR_GOLD_LIGHT);
          gfx.drawFastHLine(bx, peakY + 1, EQ_BAR_WIDTH, COLOR_GOLD);
        }
      }

      prevHeights[i] = curH;
      prevPeakHeights[i] = curPeakH;
    }

    forceEqRedraw = false;
  }

  void advanceTrackSecond(Adafruit_GFX &gfx) {
    if (isPlaying) {
      trackSeconds++;
      if (trackDuration > 0 && trackSeconds > trackDuration) {
        trackSeconds = trackDuration;
      }
      drawProgressBar(gfx);
    }
  }

  void updateData(Adafruit_GFX &gfx) {
    if (infoDirty) {
      drawTrackInfo(gfx);
      infoDirty = false;
    }
    if (progressDirty) {
      drawProgressBar(gfx);
      progressDirty = false;
    }
    if (statusDirty) {
      drawControls(gfx);
      statusDirty = false;
    }
  }
};

#endif // SCREEN_MUSIC_H
