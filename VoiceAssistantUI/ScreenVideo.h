#ifndef SCREEN_VIDEO_H
#define SCREEN_VIDEO_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "Ornament.h"
#include "UIComponents.h"

// ============================================================================
// Экран воспроизведения видео (Video Screen / YouTube)
// Разрешение: 320x240
// Компоновка:
// - Header: «БЕЙНЕ • YOUTUBE» + Wi-Fi + Время
// - Информационный бейдж источника и статус воспроизведения
// - Название видео (2 строки с умным переносом слов UTF-8 без разрыва кириллицы)
// - Название канала (автора)
// - Полоса прогресса воспроизведения (текущее время / общая длительность)
// - Кнопки управления плеером (Назад 10с, Play/Pause, Вперед 10с)
// - Казахский орнамент и пагинация
// ============================================================================

class ScreenVideo {
private:
  char videoTitle[160];
  char videoChannel[96];
  char mediaSource[40];
  bool isPlaying;
  uint32_t videoSeconds;
  uint32_t videoDuration;
  bool isConnected;

  bool infoDirty;
  bool statusDirty;
  bool progressDirty;

public:
  ScreenVideo()
    : isPlaying(false), videoSeconds(0), videoDuration(0), isConnected(false),
      infoDirty(true), statusDirty(true), progressDirty(true) {
    strncpy(videoTitle, "Бейне жүктелуде...", sizeof(videoTitle));
    strncpy(videoChannel, "YouTube", sizeof(videoChannel));
    strncpy(mediaSource, "YouTube", sizeof(mediaSource));
  }

  void setVideoData(const char *title, const char *channel, const char *source,
                    uint32_t pos, uint32_t dur, bool playing, bool connected) {
    if (title && strlen(title) > 0) {
      if (strncmp(videoTitle, title, sizeof(videoTitle) - 1) != 0) {
        strncpy(videoTitle, title, sizeof(videoTitle) - 1);
        videoTitle[sizeof(videoTitle) - 1] = '\0';
        infoDirty = true;
      }
    }
    if (channel && strlen(channel) > 0) {
      if (strncmp(videoChannel, channel, sizeof(videoChannel) - 1) != 0) {
        strncpy(videoChannel, channel, sizeof(videoChannel) - 1);
        videoChannel[sizeof(videoChannel) - 1] = '\0';
        infoDirty = true;
      }
    }
    if (source && strlen(source) > 0) {
      if (strncmp(mediaSource, source, sizeof(mediaSource) - 1) != 0) {
        strncpy(mediaSource, source, sizeof(mediaSource) - 1);
        mediaSource[sizeof(mediaSource) - 1] = '\0';
        statusDirty = true;
      }
    }

    if (isPlaying != playing || isConnected != connected) {
      isPlaying = playing;
      isConnected = connected;
      statusDirty = true;
      progressDirty = true;
    }

    if (videoDuration != dur) {
      videoDuration = dur;
      progressDirty = true;
    }

    int32_t diff = (int32_t)pos - (int32_t)videoSeconds;
    if (diff < 0) diff = -diff;

    // Обновляем секунды при смене видео, паузе или при перемотке (разница >= 2 сек)
    if (infoDirty || statusDirty || !isPlaying || diff >= 2 || videoSeconds == 0) {
      if (videoSeconds != pos) {
        videoSeconds = pos;
        progressDirty = true;
      }
    }
  }

  bool getPlaying() const { return isPlaying; }
  void togglePlay() {
    isPlaying = !isPlaying;
    statusDirty = true;
  }

  void seekLocal(int32_t deltaSec, Adafruit_GFX &gfx) {
    int32_t nextPos = (int32_t)videoSeconds + deltaSec;
    if (nextPos < 0) nextPos = 0;
    if (videoDuration > 0 && (uint32_t)nextPos > videoDuration) {
      nextPos = (int32_t)videoDuration;
    }
    videoSeconds = (uint32_t)nextPos;
    drawProgressBar(gfx);
  }

  // Отрисовка всего экрана
  void drawStatic(Adafruit_GFX &gfx, const char *timeHeaderStr, bool wifiOk) {
    gfx.fillScreen(COLOR_BG);

    // 1. Шапка
    drawHeader(gfx, "БЕЙНЕ • YOUTUBE", timeHeaderStr, wifiOk);

    // 2. Боковые тонкие казахские орнаменты
    drawSideBorders(gfx, COLOR_GOLD);
    drawCornerAccents(gfx, COLOR_GOLD_DARK);

    // 3. Статус подключения и воспроизведения (y=32..50)
    drawStatusBadge(gfx);

    // 4. Текстовый блок названия видео и канала (y=56..120)
    drawVideoInfo(gfx);

    // 5. Прогресс-бар видео (y=132..146)
    drawProgressBar(gfx);

    // 6. Кнопки управления видео (y=154..196)
    drawControls(gfx);

    // 7. Нижний орнамент и пагинация (индекс 2 из 4)
    drawBottomOrnamentBar(gfx, 214, COLOR_GOLD);
    drawPagination(gfx, 2, SCREEN_COUNT);

    infoDirty = false;
    statusDirty = false;
    progressDirty = false;
  }

  // Отрисовка бейджа статуса
  void drawStatusBadge(Adafruit_GFX &gfx) {
    const int16_t badgeY = 32;
    const int16_t badgeH = 18;
    const int16_t badgeW = 260;
    const int16_t badgeX = (SCREEN_WIDTH - badgeW) / 2;

    gfx.fillRect(badgeX, badgeY, badgeW, badgeH, COLOR_BG);

    if (!isConnected) {
      gfx.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 4, COLOR_CARD_BG);
      gfx.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 4, COLOR_CARD_BORDER);
      gfx.fillCircle(badgeX + 12, badgeY + 9, 3, COLOR_TEXT_MUTED);
      drawStringUTF8(gfx, badgeX + 22, badgeY + 5, "MacBook: БАЙЛАНЫС ЖОҚ (OFFLINE)", COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);
    } else if (isPlaying) {
      gfx.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 4, 0x0A24);
      gfx.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 4, COLOR_STATUS_READY);
      gfx.fillCircle(badgeX + 12, badgeY + 9, 3, COLOR_STATUS_READY);
      drawStringUTF8(gfx, badgeX + 22, badgeY + 5, "ОЙНАТУДА • PLAYING", COLOR_STATUS_READY, 0x0A24, 1);
    } else {
      gfx.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 4, 0x1920);
      gfx.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 4, COLOR_STATUS_THINK);
      gfx.fillCircle(badgeX + 12, badgeY + 9, 3, COLOR_STATUS_THINK);
      drawStringUTF8(gfx, badgeX + 22, badgeY + 5, "ТОҚТАТЫЛДЫ • PAUSED", COLOR_STATUS_THINK, 0x1920, 1);
    }
  }

  // Отрисовка названия видео и канала с безопасным переносом UTF-8
  void drawVideoInfo(Adafruit_GFX &gfx) {
    // Очищаем область текста между боковыми орнаментами
    gfx.fillRect(18, 54, 284, 68, COLOR_BG);

    char line1[96];
    char line2[96];
    // 22 символа при size=2 занимают 262 пикселя (идеально входит в 284px рабочей зоны)
    wrapUTF8TwoLines(videoTitle, line1, sizeof(line1), line2, sizeof(line2), 22);

    if (line2[0] != '\0') {
      drawStringUTF8(gfx, 0, 58, line1, COLOR_GOLD, COLOR_BG, 2, true);
      drawStringUTF8(gfx, 0, 78, line2, COLOR_GOLD, COLOR_BG, 2, true);
    } else {
      // Если заголовок короткий (в 1 строку) — центрируем по вертикали
      drawStringUTF8(gfx, 0, 68, line1, COLOR_GOLD, COLOR_BG, 2, true);
    }

    // Канал / Автор
    char truncChannel[64];
    truncateUTF8(videoChannel, truncChannel, sizeof(truncChannel), 30);
    char channelBuf[88];
    snprintf(channelBuf, sizeof(channelBuf), "Арна: %s", truncChannel);
    drawStringUTF8(gfx, 0, 105, channelBuf, COLOR_BEIGE, COLOR_BG, 1, true);
  }

  // Отрисовка полосы прогресса без остаточных следов бегунка
  void drawProgressBar(Adafruit_GFX &gfx) {
    const int16_t barX = 66;
    const int16_t barY = 136;
    const int16_t barW = 188;
    const int16_t barH = 5;

    // Временные метки (поддержка MM:SS и H:MM:SS)
    char curTime[12];
    formatTimeHMS(videoSeconds, curTime, sizeof(curTime));

    char totalTime[12];
    formatTimeHMS(videoDuration, totalTime, sizeof(totalTime));

    // Очистка и отрисовка времени слева и справа
    gfx.fillRect(16, barY - 3, 46, 12, COLOR_BG);
    drawStringUTF8(gfx, 18, barY - 2, curTime, COLOR_TEXT_MUTED, COLOR_BG, 1);

    gfx.fillRect(258, barY - 3, 46, 12, COLOR_BG);
    drawStringUTF8(gfx, 260, barY - 2, totalTime, COLOR_TEXT_MUTED, COLOR_BG, 1);

    // Полная очистка области полосы и круглого бегунка (радиус 4), чтобы не оставалось следов
    gfx.fillRect(barX - 5, barY - 3, barW + 10, 11, COLOR_BG);

    // Фоновая шкала
    gfx.fillRoundRect(barX, barY, barW, barH, 2, COLOR_CARD_BG);
    gfx.drawRoundRect(barX, barY, barW, barH, 2, COLOR_CARD_BORDER);

    // Заполненная часть
    int16_t fillW = 0;
    if (videoDuration > 0) {
      uint32_t clampedPos = (videoSeconds <= videoDuration) ? videoSeconds : videoDuration;
      fillW = (int16_t)((uint64_t)barW * clampedPos / videoDuration);
    }
    if (fillW > 0) {
      gfx.fillRoundRect(barX, barY, fillW, barH, 2, COLOR_GOLD);
    }

    // Бегунок
    int16_t knobX = barX + fillW;
    if (knobX > barX + barW) knobX = barX + barW;
    gfx.fillCircle(knobX, barY + 2, 4, COLOR_GOLD_LIGHT);
    gfx.drawCircle(knobX, barY + 2, 4, COLOR_GOLD);
  }

  // Кнопки управления видео
  void drawControls(Adafruit_GFX &gfx) {
    const int16_t cY = 175;

    // Кнопка Seek Back -10s (|<<)
    const int16_t prevX = 90;
    gfx.fillCircle(prevX + 10, cY, 15, COLOR_CARD_BG);
    gfx.drawCircle(prevX + 10, cY, 15, COLOR_CARD_BORDER);
    gfx.drawFastVLine(prevX + 3, cY - 5, 11, COLOR_GOLD);
    gfx.fillTriangle(prevX + 11, cY - 5, prevX + 11, cY + 5, prevX + 5, cY, COLOR_GOLD);
    gfx.fillTriangle(prevX + 17, cY - 5, prevX + 17, cY + 5, prevX + 11, cY, COLOR_GOLD);

    // Центральная Play/Pause
    gfx.fillCircle(160, cY, 20, COLOR_GOLD);
    gfx.drawCircle(160, cY, 22, COLOR_GOLD_LIGHT);

    if (isPlaying) {
      // Иконка Pause: две полоски
      gfx.fillRect(153, cY - 8, 4, 16, COLOR_BG);
      gfx.fillRect(163, cY - 8, 4, 16, COLOR_BG);
    } else {
      // Иконка Play: треугольник
      gfx.fillTriangle(154, cY - 9, 154, cY + 9, 171, cY, COLOR_BG);
    }

    // Кнопка Seek Fwd +10s (>>|)
    const int16_t nextX = 220;
    gfx.fillCircle(nextX + 10, cY, 15, COLOR_CARD_BG);
    gfx.drawCircle(nextX + 10, cY, 15, COLOR_CARD_BORDER);
    gfx.fillTriangle(nextX + 3, cY - 5, nextX + 3, cY + 5, nextX + 9, cY, COLOR_GOLD);
    gfx.fillTriangle(nextX + 9, cY - 5, nextX + 9, cY + 5, nextX + 15, cY, COLOR_GOLD);
    gfx.drawFastVLine(nextX + 17, cY - 5, 11, COLOR_GOLD);
  }

  // Локальное продвижение секунды (только если видео реально воспроизводится!)
  void advanceSecond(Adafruit_GFX &gfx) {
    if (isPlaying) {
      videoSeconds++;
      if (videoDuration > 0 && videoSeconds > videoDuration) {
        videoSeconds = videoDuration;
      }
      drawProgressBar(gfx);
    }
  }

  // Дифференциальное обновление данных без мерцания экрана
  void updateData(Adafruit_GFX &gfx) {
    if (statusDirty) {
      drawStatusBadge(gfx);
      drawControls(gfx);
      statusDirty = false;
    }
    if (infoDirty) {
      drawVideoInfo(gfx);
      infoDirty = false;
    }
    if (progressDirty) {
      drawProgressBar(gfx);
      progressDirty = false;
    }
  }
};

#endif // SCREEN_VIDEO_H
