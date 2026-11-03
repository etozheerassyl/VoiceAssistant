#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "CyrillicFont.h"

// -------------------------------------------------------------
// Распиновка подключения дисплея ILI9341 к ESP32-S3
// -------------------------------------------------------------
#define TFT_CS   10
#define TFT_DC   14
#define TFT_RST  9
#define TFT_MOSI 11
#define TFT_SCK  12

// Создание объекта дисплея с аппаратным SPI
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// -------------------------------------------------------------
// Цветовая палитра интерфейса (16-битный RGB565)
// -------------------------------------------------------------
#define COLOR_BG          0x0000 // Глубокий черный фон для максимального контраста
#define COLOR_HEADER_BG   0x0842 // Темно-серый фон плашки заголовка
#define COLOR_CARD_BG     0x0863 // Темный фон информационных карточек
#define COLOR_CARD_BORDER 0x2125 // Сдержанная серая рамка
#define COLOR_CYAN        0x07FF // Яркий бирюзовый акцент
#define COLOR_CYAN_DIM    0x02EC // Приглушенный бирюзовый для контура
#define COLOR_GREEN       0x07E0 // Изумрудно-зеленый (статус «Готов»)
#define COLOR_AMBER       0xFDA0 // Янтарный (индикатор распознавания)
#define COLOR_TEXT_WHITE  0xFFFF // Белый для основного текста
#define COLOR_TEXT_MUTED  0x7BEF // Приглушенный серый для подписей и подсказок

// Переменные для динамического обновления (без мерцания)
unsigned long lastUptimeUpdate = 0;
unsigned long lastPulseUpdate = 0;
uint8_t pulsePhase = 0;

// Отрисовка статичного интерфейса экрана
void drawStaticUI() {
  tft.fillScreen(COLOR_BG);

  // 1. Верхняя панель (Заголовок)
  tft.fillRect(0, 0, 240, 42, COLOR_HEADER_BG);
  tft.drawFastHLine(0, 42, 240, COLOR_CYAN);
  drawStringUTF8(tft, 0, 8, "ГОЛОСОВОЙ ПОМОЩНИК", COLOR_TEXT_WHITE, COLOR_HEADER_BG, 1, true);
  drawStringUTF8(tft, 0, 24, "ESP32-S3 AI ASSISTANT", COLOR_CYAN, COLOR_HEADER_BG, 1, true);

  // 2. Центральный индикатор состояния
  const int16_t cx = 120;
  const int16_t cy = 96;

  // Концентрические кольца индикатора
  tft.drawCircle(cx, cy, 38, COLOR_CYAN_DIM);
  tft.drawCircle(cx, cy, 37, COLOR_CYAN_DIM);
  tft.drawCircle(cx, cy, 33, COLOR_CYAN);
  tft.drawCircle(cx, cy, 32, COLOR_CYAN);
  tft.fillCircle(cx, cy, 22, 0x0208); // Темное ядро
  tft.fillCircle(cx, cy, 14, COLOR_GREEN); // Активное зеленое ядро готовности

  // Подпись состояния под индикатором
  drawStringUTF8(tft, 0, 142, "ГОТОВ К РАБОТЕ", COLOR_GREEN, COLOR_BG, 1, true);
  drawStringUTF8(tft, 0, 154, "Ожидание команды...", COLOR_TEXT_MUTED, COLOR_BG, 1, true);

  // 3. Карточка «Вы услышали»
  const int16_t card1Y = 170;
  tft.fillRoundRect(8, card1Y, 224, 52, 4, COLOR_CARD_BG);
  tft.drawRoundRect(8, card1Y, 224, 52, 4, COLOR_CARD_BORDER);
  tft.fillRect(8, card1Y + 3, 3, 46, COLOR_AMBER); // Акцентная полоса слева

  drawStringUTF8(tft, 16, card1Y + 6, "ВЫ УСЛЫШАЛИ:", COLOR_AMBER, COLOR_CARD_BG, 1);
  drawStringUTF8(tft, 16, card1Y + 22, "«Привет, помощник»", COLOR_TEXT_WHITE, COLOR_CARD_BG, 1);
  drawStringUTF8(tft, 16, card1Y + 36, "Фраза активации распознана", COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);

  // 4. Карточка «Ответ помощника»
  const int16_t card2Y = 228;
  tft.fillRoundRect(8, card2Y, 224, 52, 4, COLOR_CARD_BG);
  tft.drawRoundRect(8, card2Y, 224, 52, 4, COLOR_CARD_BORDER);
  tft.fillRect(8, card2Y + 3, 3, 46, COLOR_GREEN); // Акцентная полоса слева

  drawStringUTF8(tft, 16, card2Y + 6, "ОТВЕТ ПОМОЩНИКА:", COLOR_GREEN, COLOR_CARD_BG, 1);
  drawStringUTF8(tft, 16, card2Y + 22, "«Я на связи. Чем помочь?»", COLOR_TEXT_WHITE, COLOR_CARD_BG, 1);
  drawStringUTF8(tft, 16, card2Y + 36, "Синтез речи готов к ответу", COLOR_TEXT_MUTED, COLOR_CARD_BG, 1);

  // 5. Нижняя панель (Футер)
  tft.drawFastHLine(8, 292, 224, COLOR_CARD_BORDER);

  // Индикатор системы
  tft.fillCircle(14, 305, 3, COLOR_GREEN);
  drawStringUTF8(tft, 22, 302, "ESP32-S3", COLOR_TEXT_MUTED, COLOR_BG, 1);

  // Статичная подпись таймера
  drawStringUTF8(tft, 114, 302, "UPTIME:", COLOR_TEXT_MUTED, COLOR_BG, 1);
}

// Обновление таймера времени работы без перерисовки всего экрана
void updateDynamicElements() {
  unsigned long totalSeconds = millis() / 1000;
  unsigned int hours = totalSeconds / 3600;
  unsigned int minutes = (totalSeconds % 3600) / 60;
  unsigned int seconds = totalSeconds % 60;

  char timeStr[10];
  snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u", hours, minutes, seconds);

  // Отрисовка поверх старого текста с закрашиванием фона (без мерцания)
  drawStringUTF8(tft, 166, 302, timeStr, COLOR_CYAN, COLOR_BG, 1);
}

// Легкая пульсация центрального кольца (индикация активной системы)
void updateStatusPulse() {
  const int16_t cx = 120;
  const int16_t cy = 96;

  // Чередуем легкую подсветку внешней границы кольца каждые 500 мс
  uint16_t pulseColor = (pulsePhase % 2 == 0) ? COLOR_CYAN : COLOR_CYAN_DIM;
  tft.drawCircle(cx, cy, 38, pulseColor);
  pulsePhase++;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[SYSTEM] Initializing Voice Assistant Display...");

  // Инициализация аппаратной шины SPI (SCK=12, MISO=-1, MOSI=11, SS=10)
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  // Инициализация драйвера дисплея на частоте 10 МГц
  tft.begin(10000000);

  // Портретная ориентация (240x320)
  tft.setRotation(0);

  // Отрисовка базового экрана
  drawStaticUI();
  updateDynamicElements();

  Serial.println("[SYSTEM] UI successfully initialized!");
}

void loop() {
  unsigned long now = millis();

  // Обновление таймера uptime каждую секунду
  if (now - lastUptimeUpdate >= 1000) {
    lastUptimeUpdate = now;
    updateDynamicElements();
  }

  // Мягкая анимация кольца каждые 500 мс
  if (now - lastPulseUpdate >= 500) {
    lastPulseUpdate = now;
    updateStatusPulse();
  }

  delay(20);
}

