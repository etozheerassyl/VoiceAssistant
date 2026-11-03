#ifndef OVERLAY_PHRASE_H
#define OVERLAY_PHRASE_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "UITheme.h"
#include "CyrillicFont.h"
#include "RandomPhrases.h"

// ============================================================================
// Всплывающий компонент фраз и ответов голосового помощника (Phrase Overlay)
// Безопасный вывод UTF-8 текста с автопереносом на 2 строки и защитой границ экрана.
// ============================================================================

class OverlayPhrase {
private:
  bool visible;
  bool isVoiceReplyMode;
  unsigned long hideTimestamp;
  unsigned long nextTriggerMs;
  char currentHeader[128];
  char currentPhrase[160];
  uint8_t phraseIndex;

public:
  OverlayPhrase()
    : visible(false),
      isVoiceReplyMode(false),
      hideTimestamp(0),
      nextTriggerMs(0),
      phraseIndex(0) {
    currentHeader[0] = '\0';
    currentPhrase[0] = '\0';
  }

  void init(unsigned long now) {
    nextTriggerMs = 0; // Автоматический вывод случайных сообщений выключен
  }

  void scheduleNext(unsigned long now, uint32_t minMs = 35000, uint32_t maxMs = 75000) {
    nextTriggerMs = 0; // Отключено
  }

  bool isVisible() const { return visible; }

  const char* getCurrentPhrase() const { return currentPhrase; }

  void dismiss() {
    visible = false;
    isVoiceReplyMode = false;
    nextTriggerMs = 0;
  }

  // Показ короткого ответа голосового помощника поверх любого экрана
  void showVoiceReply(const char *heard, const char *reply, unsigned long now, uint32_t durationMs = 6500) {
    isVoiceReplyMode = true;
    if (heard && strlen(heard) > 0) {
      // Сначала безопасно обрезаем вопрос по числу символов UTF-8 (до 34 символов),
      // чтобы закрывающая кавычка «» никогда не обрезалась и не выходила за края плашки
      char shortHeard[96];
      truncateUTF8(heard, shortHeard, sizeof(shortHeard), 34);
      snprintf(currentHeader, sizeof(currentHeader), "ВЫ: «%s»", shortHeard);
    } else {
      strncpy(currentHeader, "AI КОМАНДА:", sizeof(currentHeader) - 1);
      currentHeader[sizeof(currentHeader) - 1] = '\0';
    }

    if (reply && strlen(reply) > 0) {
      strncpy(currentPhrase, reply, sizeof(currentPhrase) - 1);
      currentPhrase[sizeof(currentPhrase) - 1] = '\0';
    } else {
      strncpy(currentPhrase, "Выполнено!", sizeof(currentPhrase) - 1);
    }

    visible = true;
    hideTimestamp = now + durationMs;
    nextTriggerMs = 0;
  }

  // Ручной показ системной фразы (только по команде из Serial 't')
  void triggerRandom(unsigned long now, uint32_t durationMs = 4500) {
    if (RANDOM_PHRASE_COUNT == 0) return;

    isVoiceReplyMode = false;
    strncpy(currentHeader, "AI ASSISTANT:", sizeof(currentHeader) - 1);
    currentHeader[sizeof(currentHeader) - 1] = '\0';

    uint8_t idx = random(0, RANDOM_PHRASE_COUNT);
    if (RANDOM_PHRASE_COUNT > 1 && idx == phraseIndex) {
      idx = (idx + 1) % RANDOM_PHRASE_COUNT;
    }
    phraseIndex = idx;
    strncpy(currentPhrase, RANDOM_PHRASES[phraseIndex], sizeof(currentPhrase) - 1);
    currentPhrase[sizeof(currentPhrase) - 1] = '\0';

    visible = true;
    hideTimestamp = now + durationMs;
    nextTriggerMs = 0;
  }

  // Проверка таймеров в loop(): скрывает баннер ответа после окончания времени показа
  bool loopUpdate(unsigned long now, bool &justShown) {
    justShown = false;

    if (visible && now >= hideTimestamp) {
      visible = false;
      isVoiceReplyMode = false;
      nextTriggerMs = 0;
      return true;
    }

    return false;
  }

  // Отрисовка фразы на дисплее
  // eyesActive: true, если сейчас открыт полноэкранный режим RobotEyes
  void draw(Adafruit_GFX &gfx, bool eyesActive) {
    if (!visible) return;

    if (eyesActive) {
      // ----------------------------------------------------------------------
      // В режиме RobotEyes: выводим плашку в нижней части экрана (y = 204..234),
      // со строгой обрезкой строки, чтобы символы никогда не выходили за края!
      // ----------------------------------------------------------------------
      const int16_t boxW = 284;
      const int16_t boxH = 30;
      const int16_t boxX = (320 - boxW) / 2; // 18
      const int16_t boxY = 205;

      // Сначала очищаем всю нижнюю полосу от края до края (0..320)
      gfx.fillRect(0, 203, 320, 37, 0x0000);

      gfx.fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BG);
      gfx.drawRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_GOLD);
      gfx.drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 5, COLOR_GOLD_DARK);

      char truncEyes[96];
      truncateUTF8(currentPhrase, truncEyes, sizeof(truncEyes), 42);
      size_t charCount = countUTF8Chars(truncEyes);
      uint8_t fSize = (!isVoiceReplyMode && charCount <= 14) ? 2 : 1;
      int16_t textW = measureStringUTF8(truncEyes, fSize);
      int16_t textX = (320 - textW) / 2;
      if (textX < boxX + 8) textX = boxX + 8;
      int16_t textY = boxY + (fSize == 2 ? 7 : 11);

      drawStringUTF8(gfx, textX, textY, truncEyes, COLOR_TEXT_WHITE, COLOR_CARD_BG, fSize);
    } else {
      // ----------------------------------------------------------------------
      // В основном интерфейсе (Home / Music / Video):
      // Всплывающая карточка (x=18..302, y=156..208) с поддержкой 2 строк ответа
      // ----------------------------------------------------------------------
      const int16_t boxW = 284;
      const int16_t boxH = 52;
      const int16_t boxX = 18;
      const int16_t boxY = 156;

      gfx.fillRoundRect(boxX, boxY, boxW, boxH, 7, COLOR_CARD_BG);
      gfx.drawRoundRect(boxX, boxY, boxW, boxH, 7, COLOR_GOLD);
      gfx.drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 6, COLOR_GOLD_DARK);

      // 1-я строка: вопрос пользователя (ВЫ: «...») — строго внутри карточки (макс 42 символа = 252 px)
      char truncHead[104];
      const char *hdr = (strlen(currentHeader) > 0) ? currentHeader : "AI ASSISTANT:";
      truncateUTF8(hdr, truncHead, sizeof(truncHead), 42);
      drawStringUTF8(gfx, boxX + 10, boxY + 6, truncHead, isVoiceReplyMode ? COLOR_STATUS_LISTEN : COLOR_GOLD, COLOR_CARD_BG, 1);

      // 2-я и 3-я строки: ответ помощника (автоперенос по словам на 2 строки до 42 символов каждая)
      char line1[104];
      char line2[104];
      wrapUTF8TwoLines(currentPhrase, line1, sizeof(line1), line2, sizeof(line2), 42);

      uint16_t replyColor = isVoiceReplyMode ? COLOR_GOLD_LIGHT : COLOR_TEXT_WHITE;
      if (line2[0] != '\0') {
        drawStringUTF8(gfx, boxX + 10, boxY + 21, line1, replyColor, COLOR_CARD_BG, 1);
        drawStringUTF8(gfx, boxX + 10, boxY + 34, line2, replyColor, COLOR_CARD_BG, 1);
      } else {
        size_t charCount = countUTF8Chars(line1);
        uint8_t fSize = (!isVoiceReplyMode && charCount <= 14) ? 2 : 1;
        int16_t textW = measureStringUTF8(line1, fSize);
        int16_t textX = (320 - textW) / 2;
        if (textX < boxX + 10) textX = boxX + 10;
        int16_t textY = boxY + (fSize == 2 ? 22 : 27);
        drawStringUTF8(gfx, textX, textY, line1, replyColor, COLOR_CARD_BG, fSize);
      }
    }
  }

  // Полная очистка нижней области в режиме глаз от края до края (0..320)
  void clearEyesArea(Adafruit_GFX &gfx) {
    gfx.fillRect(0, 202, 320, 38, 0x0000);
  }
};

#endif // OVERLAY_PHRASE_H
