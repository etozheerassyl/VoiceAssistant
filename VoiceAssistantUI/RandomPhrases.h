#ifndef RANDOM_PHRASES_H
#define RANDOM_PHRASES_H

#include <Arduino.h>

// ============================================================================
// Список случайных фраз ассистента для вывода на экран.
// Сюда можно легко дописывать любые новые фразы на русском, казахском или латинице!
// Все фразы корректно отображаются благодаря встроенному шрифту CyrillicFont.
// ============================================================================
static const char* const RANDOM_PHRASES[] = {
  "AI Көмекші дайын",
  "Жүйе жұмыс істеп тұр",
  "ESP32-S3 Voice Assistant"
};

static const size_t RANDOM_PHRASE_COUNT = sizeof(RANDOM_PHRASES) / sizeof(RANDOM_PHRASES[0]);

#endif // RANDOM_PHRASES_H

