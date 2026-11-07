#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

// ============================================================================
// Конфигурация локальной сети Wi-Fi и подключения к MacBook Air M1
// ============================================================================

// 1. Учетные данные вашей Wi-Fi сети (роутер)
// ВНИМАНИЕ: Замените на имя и пароль вашей локальной сети Wi-Fi (2.4 ГГц):
#define WIFI_SSID         "TP-Link_1825"
#define WIFI_PASSWORD     "75242930"

// 2. Сетевой адрес MacBook в локальной сети Wi-Fi
// Узнать IP MacBook можно в Терминале командой: ipconfig getifaddr en0
#define SERVER_HOST       "192.168.0.106"
#define SERVER_PORT       8765

// 3. Токен авторизации (должен совпадать с AUTH_TOKEN в server.py)
#define AUTH_TOKEN        "esp32_mac_secret_key"

// 4. Таймауты и интервалы опроса (в миллисекундах)
#define WIFI_RETRY_INTERVAL_MS   5000
#define SERVER_RETRY_INTERVAL_MS 3000
#define CLIENT_BUFFER_SIZE       1024

#endif // NETWORK_CONFIG_H

