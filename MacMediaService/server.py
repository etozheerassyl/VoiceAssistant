#!/usr/bin/env python3
"""
MacMediaService - macOS Media, Volume, Time and Real-Time Audio Equalizer Gateway
for ESP32-S3 Voice Assistant (320x240 TFT).
Runs on MacBook Air M1 (Apple Silicon).
No third-party pip dependencies required.
"""

import sys
import os
import re
import time
import datetime
import json
import socket
import threading
import subprocess
import urllib.request
import logging
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

from audio_processor import (
    AudioEqualizerService,
    PCMSpectrumAnalyzer,
    sanitize_equalizer_bands,
    DEFAULT_BAND_COUNT,
)
from voice_assistant import recognize_pcm16, process_voice_command, is_tts_speaking_or_tail, set_event_broadcaster

# ============================================================================
# Конфигурация сервиса
# ============================================================================
TCP_PORT = 8765               # Порт для подключения ESP32-S3
HTTP_PORT = 8766              # Порт для YouTube / Yandex Music Userscript
AUTH_TOKEN = "esp32_mac_secret_key"  # Простой локальный токен аутентификации
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(BASE_DIR, "service.log")
MEDIA_HELPER_SWIFT = os.path.join(BASE_DIR, "media_helper.swift")

EQ_BAND_COUNT = DEFAULT_BAND_COUNT   # 16 логарифмических полос
EQ_TARGET_FPS = 30                   # Целевая частота обновления спектра (30 Гц)
EQ_MIN_SEND_INTERVAL = 0.032         # Минимальный интервал отправки (ограничение частоты ~30 Гц)
EQ_IDLE_KEEPALIVE_INTERVAL = 1.5     # Интервал отправки нулевых полос в полной тишине

# Настройка логирования
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler(LOG_FILE, encoding="utf-8")
    ]
)
logger = logging.getLogger("MacMediaService")

# ============================================================================
# Глобальное состояние
# ============================================================================
state_lock = threading.Lock()
socket_send_lock = threading.Lock()

current_state = {
    "type": "media_state",
    "connected": True,
    "title": "",
    "artist": "",
    "source": "macOS",
    "state": "stopped",
    "position": 0,
    "duration": 0,
    "volume": 50,
    "time": {
        "hours": 0,
        "minutes": 0,
        "seconds": 0
    },
    "date": "СӘРСЕНБІ, 28 ҚАЗАН",
    "weather": {
        "temp": 18,
        "desc_kz": "Ашық күн",
        "desc_ru": "ясно",
        "usd_kzt": 495,
        "mac_battery": 100,
        "mac_charging": False
    }
}

current_eq_state = {
    "type": "equalizer",
    "bands": [0] * EQ_BAND_COUNT,
    "state": "stopped"
}

# Данные из браузерной интеграции (YouTube / Яндекс Музыка Userscript)
browser_data = {
    "active": False,
    "last_seen": 0,
    "title": "",
    "artist": "",
    "source": "Browser",
    "position": 0,
    "duration": 0,
    "state": "stopped"
}

# Очередь команд для браузера
browser_pending_commands = []
connected_esp_clients = []
running = True
audio_service = None

# ============================================================================
# Форматирование точного времени и даты (Казахская локализация)
# ============================================================================
DAYS_KZ = ["ДҮЙСЕНБІ", "СЕЙСЕНБІ", "СӘРСЕНБІ", "БЕЙСЕНБІ", "ЖҰМА", "СЕНБІ", "ЖЕКСЕНБІ"]
MONTHS_KZ = ["ҚАҢТАР", "АҚПАН", "НАУРЫЗ", "СӘУІР", "МАМЫР", "МАУСЫМ", "ШІЛДЕ", "ТАМЫЗ", "ҚЫРКҮЙЕК", "ҚАЗАН", "ҚАРАША", "ЖЕЛТОҚСАН"]

def clean_title_text(raw_title):
    if not raw_title:
        return ""
    # Убираем префикс уведомлений вида "(4) " или "(12) " в начале заголовка YouTube
    cleaned = re.sub(r"^\(\d+\)\s*", "", str(raw_title)).strip()
    return cleaned

def get_current_time_payload():
    now = datetime.datetime.now()
    date_str = f"{DAYS_KZ[now.weekday()]}, {now.day} {MONTHS_KZ[now.month - 1]}"
    time_dict = {
        "hours": now.hour,
        "minutes": now.minute,
        "seconds": now.second
    }
    return time_dict, date_str

# ============================================================================
# Живая погода (Алматы, Open-Meteo), курс USD/KZT и заряд батареи MacBook
# ============================================================================
def _decode_wmo_weather(code: int):
    if code == 0:
        return "Ашық", "ясно и солнечно"
    elif code in (1, 2):
        return "Ала бұлт", "переменная облачность"
    elif code == 3:
        return "Бұлтты", "пасмурно"
    elif code in (45, 48):
        return "Тұман", "туман"
    elif code in (51, 53, 55, 61, 63, 65, 80, 81, 82):
        return "Жаңбыр", "дождь"
    elif code in (71, 73, 75, 77, 85, 86):
        return "Қар", "снег"
    elif code in (95, 96, 99):
        return "Найзағай", "гроза"
    return "Ашық", "комфортная погода"

def fetch_mac_battery_info():
    """Возвращает (процент_заряда, заряжается_ли) ноутбука MacBook."""
    try:
        out = subprocess.check_output(["/usr/bin/pmset", "-g", "batt"], text=True, timeout=1.5)
        m = re.search(r"(\d+)%", out)
        pct = int(m.group(1)) if m else 100
        charging = ("AC Power" in out) or ("charging" in out.lower() and "discharging" not in out.lower())
        return pct, charging
    except Exception:
        return 100, False

def fetch_live_weather_and_rates():
    """Запрашивает реальную погоду в Алматы и актуальный курс доллара к тенге (USD/KZT)."""
    temp_c = None
    desc_kz = None
    desc_ru = None
    usd_kzt = None

    # 1. Open-Meteo (Алматы: 43.2389, 76.8897)
    try:
        w_url = "https://api.open-meteo.com/v1/forecast?latitude=43.2389&longitude=76.8897&current_weather=true"
        req = urllib.request.Request(w_url, headers={"User-Agent": "ESP32VoiceAssistant/2.5"})
        with urllib.request.urlopen(req, timeout=3.5) as resp:
            w_data = json.loads(resp.read().decode("utf-8"))
            cw = w_data.get("current_weather", {})
            if "temperature" in cw:
                temp_c = int(round(float(cw["temperature"])))
                w_code = int(cw.get("weathercode", 0))
                desc_kz, desc_ru = _decode_wmo_weather(w_code)
    except Exception as e:
        logger.debug(f"[WEATHER] Не удалось обновить погоду Open-Meteo: {e}")

    # 2. Официальный курс USD -> KZT Национального Банка РК (nationalbank.kz) + резерв ExchangeRate API
    try:
        nbk_url = "https://nationalbank.kz/rss/rates_all.xml"
        req = urllib.request.Request(nbk_url, headers={"User-Agent": "ESP32VoiceAssistant/2.5"})
        with urllib.request.urlopen(req, timeout=3.5) as resp:
            xml_str = resp.read().decode("utf-8", errors="ignore")
            m_usd = re.search(r"<title>USD</title>.*?<description>([\d.]+)</description>", xml_str, re.S)
            if m_usd:
                usd_kzt = int(round(float(m_usd.group(1))))
    except Exception as e:
        logger.debug(f"[CURRENCY] Ошибка НБ РК ({e}), пробую резервный API...")

    if usd_kzt is None:
        try:
            r_url = "https://open.er-api.com/v6/latest/USD"
            req = urllib.request.Request(r_url, headers={"User-Agent": "ESP32VoiceAssistant/2.5"})
            with urllib.request.urlopen(req, timeout=3.5) as resp:
                r_data = json.loads(resp.read().decode("utf-8"))
                rates = r_data.get("rates", {})
                if "KZT" in rates:
                    usd_kzt = int(round(float(rates["KZT"])))
        except Exception as e:
            logger.debug(f"[CURRENCY] Не удалось обновить курс USD/KZT: {e}")

    batt_pct, batt_chg = fetch_mac_battery_info()

    changed = False
    with state_lock:
        w_obj = dict(current_state.get("weather", {}))
        if temp_c is not None and w_obj.get("temp") != temp_c:
            w_obj["temp"] = temp_c
            changed = True
        if desc_kz is not None and w_obj.get("desc_kz") != desc_kz:
            w_obj["desc_kz"] = desc_kz
            w_obj["desc_ru"] = desc_ru
            changed = True
        if usd_kzt is not None and w_obj.get("usd_kzt") != usd_kzt:
            w_obj["usd_kzt"] = usd_kzt
            changed = True
        if w_obj.get("mac_battery") != batt_pct or w_obj.get("mac_charging") != batt_chg:
            w_obj["mac_battery"] = batt_pct
            w_obj["mac_charging"] = batt_chg
            changed = True
        current_state["weather"] = w_obj

    if changed:
        logger.info(
            f"[LIVE TELEMETRY] Алматы: {w_obj.get('temp')}°C ({w_obj.get('desc_kz')}) | "
            f"Курс USD: {w_obj.get('usd_kzt')} ₸ | Батарея Mac: {batt_pct}% ({'Зарядка' if batt_chg else 'От батареи'})"
        )
        broadcast_state(force=True)

def weather_and_sys_loop():
    """Фоновый цикл обновления погоды, курса валют (раз в 10 мин) и батареи MacBook (раз в 30 сек)."""
    last_full_fetch = 0.0
    while running:
        now = time.time()
        if now - last_full_fetch >= 600.0:
            last_full_fetch = now
            fetch_live_weather_and_rates()
        else:
            batt_pct, batt_chg = fetch_mac_battery_info()
            changed = False
            with state_lock:
                w_obj = dict(current_state.get("weather", {}))
                if w_obj.get("mac_battery") != batt_pct or w_obj.get("mac_charging") != batt_chg:
                    w_obj["mac_battery"] = batt_pct
                    w_obj["mac_charging"] = batt_chg
                    current_state["weather"] = w_obj
                    changed = True
            if changed:
                broadcast_state(force=True)
        time.sleep(30.0)

# ============================================================================
# Потокобезопасная отправка данных клиентам ESP32
# ============================================================================
def send_line_to_client(client_sock, payload_dict):
    """Атомарно отправляет одну JSON-строку конкретному клиенту ESP32."""
    raw = (json.dumps(payload_dict, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    with socket_send_lock:
        client_sock.sendall(raw)

def broadcast_raw_bytes(data_bytes):
    """Рассылает подготовленный JSON-пакет всем подключенным клиентам ESP32 без блокировки аудио."""
    with state_lock:
        clients_snapshot = list(connected_esp_clients)
    if not clients_snapshot:
        return

    dead_clients = []
    with socket_send_lock:
        for client in clients_snapshot:
            try:
                client.sendall(data_bytes)
            except Exception as e:
                logger.warning(f"Ошибка отправки клиенту ESP32: {e}")
                dead_clients.append(client)

    if dead_clients:
        with state_lock:
            for dead in dead_clients:
                if dead in connected_esp_clients:
                    connected_esp_clients.remove(dead)

# ============================================================================
# Трансляция данных эквалайзера (16 частотных диапазонов)
# ============================================================================
last_eq_sent_time = 0.0
eq_zero_streak = 0

def on_equalizer_update(bands, audio_active):
    """
    Callback от модуля обработки аудио (вызывается в фоновом потоке до 30 раз/с).
    Формирует и отправляет сообщение {"type": "equalizer", "bands": [...], "state": ...}.
    """
    global last_eq_sent_time, eq_zero_streak
    now = time.time()

    clean_bands = sanitize_equalizer_bands(bands, EQ_BAND_COUNT)
    has_energy = audio_active or any(b > 0 for b in clean_bands)

    with state_lock:
        media_st = current_state.get("state", "stopped")
        if has_energy:
            eq_state_str = "playing"
        else:
            eq_state_str = media_st if media_st in ("paused", "stopped") else "paused"

        current_eq_state["bands"] = clean_bands
        current_eq_state["state"] = eq_state_str
        has_clients = len(connected_esp_clients) > 0

    if not has_clients:
        return

    # Ограничение частоты отправки (Rate Limiting)
    elapsed = now - last_eq_sent_time
    if elapsed < EQ_MIN_SEND_INTERVAL:
        return

    if not has_energy:
        eq_zero_streak += 1
        # Отправляем первые 4 нулевых кадра подряд для гарантированного затухания на ESP32,
        # затем переходим в экономичный режим ожидания (раз в 1.5 сек)
        if eq_zero_streak > 4 and elapsed < EQ_IDLE_KEEPALIVE_INTERVAL:
            return
    else:
        eq_zero_streak = 0

    msg_obj = {
        "type": "equalizer",
        "bands": clean_bands,
        "state": eq_state_str
    }
    data_bytes = (json.dumps(msg_obj, separators=(",", ":")) + "\n").encode("utf-8")
    last_eq_sent_time = now
    broadcast_raw_bytes(data_bytes)

# ============================================================================
# Постоянный демон Swift MediaHelper (Zero-Latency IPC)
# ============================================================================
helper_lock = threading.Lock()
helper_proc = None

def get_helper_proc():
    global helper_proc
    if helper_proc is not None and helper_proc.poll() is None:
        return helper_proc
    try:
        logger.info("Запуск постоянного процесса Swift MediaHelper (--daemon)...")
        helper_proc = subprocess.Popen(
            ["/usr/bin/swift", MEDIA_HELPER_SWIFT, "--daemon"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )
        return helper_proc
    except Exception as e:
        logger.error(f"Не удалось запустить Swift MediaHelper: {e}")
        helper_proc = None
        return None

def query_media_helper(command_line):
    """Отправляет команду в постоянный Swift-процесс и возвращает словарь JSON."""
    global helper_proc
    with helper_lock:
        proc = get_helper_proc()
        if not proc:
            return None
        try:
            proc.stdin.write(command_line + "\n")
            proc.stdin.flush()
            resp_line = proc.stdout.readline()
            if not resp_line:
                logger.warning("Swift MediaHelper закрыл поток stdout, перезапуск...")
                try:
                    proc.kill()
                except Exception:
                    pass
                helper_proc = None
                return None
            return json.loads(resp_line.strip())
        except Exception as e:
            logger.error(f"Ошибка обмена с Swift MediaHelper ({command_line}): {e}")
            try:
                proc.kill()
            except Exception:
                pass
            helper_proc = None
            return None

# ============================================================================
# Определение источника в браузере (YouTube vs Яндекс Музыка)
# ============================================================================
last_browser_scan = 0
cached_browser_info = {
    "youtube_titles": [],
    "has_youtube": False,
    "has_yandex": False
}

BROWSER_NAMES = {"google chrome", "safari", "arc", "brave browser", "opera", "microsoft edge", "firefox", "yandex"}

def scan_browser_tabs_info():
    """Проверяет открытые вкладки Safari / Chrome для точной классификации источника (YouTube или Яндекс Музыка)."""
    global last_browser_scan, cached_browser_info
    now = time.time()
    if now - last_browser_scan < 1.5:
        return cached_browser_info
    last_browser_scan = now

    osa_script = """
    tell application "System Events"
        set isSafariRunning to (exists (processes where name is "Safari"))
        set isChromeRunning to (exists (processes where name is "Google Chrome"))
    end tell

    set res to ""
    if isSafariRunning then
        tell application "Safari"
            repeat with w in windows
                repeat with t in tabs of w
                    set u to (URL of t as string)
                    set n to (name of t as string)
                    if u contains "music.yandex" or u contains "youtube.com/watch" or u contains "youtube.com/shorts" or u contains "music.youtube.com" then
                        set res to res & u & "<TAB>" & n & "<NL>"
                    end if
                end repeat
            end repeat
        end tell
    end if

    if isChromeRunning then
        tell application "Google Chrome"
            repeat with w in windows
                repeat with t in tabs of w
                    set u to (URL of t as string)
                    set n to (title of t as string)
                    if u contains "music.yandex" or u contains "youtube.com/watch" or u contains "youtube.com/shorts" or u contains "music.youtube.com" then
                        set res to res & u & "<TAB>" & n & "<NL>"
                    end if
                end repeat
            end repeat
        end tell
    end if

    return res
    """
    yt_titles = []
    has_yt = False
    has_ya = False
    try:
        proc = subprocess.run(["osascript", "-e", osa_script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=0.8)
        if proc.returncode == 0 and proc.stdout.strip():
            for line in proc.stdout.strip().split("<NL>"):
                line = line.strip()
                if "<TAB>" in line:
                    url, name = line.split("<TAB>", 1)
                    url = url.strip()
                    name = clean_title_text(name.strip())
                    if "music.yandex" in url:
                        has_ya = True
                    elif "youtube.com" in url:
                        has_yt = True
                        clean_yt = name.replace(" - YouTube Music", "").replace(" - YouTube", "").replace("- YouTube", "").strip()
                        clean_yt = clean_title_text(clean_yt)
                        if clean_yt:
                            yt_titles.append(clean_yt)
    except Exception:
        pass

    cached_browser_info = {
        "youtube_titles": yt_titles,
        "has_youtube": has_yt,
        "has_yandex": has_ya
    }
    return cached_browser_info

def classify_media_source(sys_state):
    """Определяет точный источник ('YouTube', 'Яндекс Музыка' и т.д.) для системного NowPlaying."""
    raw_source = sys_state.get("source", "macOS") or "macOS"
    bundle_id = (sys_state.get("bundle_id", "") or "").lower()
    title = clean_title_text(sys_state.get("title", ""))
    album = (sys_state.get("album", "") or "").strip()

    if "yandex" in raw_source.lower() or "yandex" in bundle_id:
        return "Яндекс Музыка"
    if "youtube" in raw_source.lower():
        return "YouTube"

    is_browser = (raw_source.lower() in BROWSER_NAMES) or any(b in bundle_id for b in ["chrome", "safari", "firefox", "arc", "brave", "opera", "edge"])
    if is_browser and title:
        tab_info = scan_browser_tabs_info()
        t_low = title.lower()
        for yt_t in tab_info["youtube_titles"]:
            yt_low = yt_t.lower()
            if t_low == yt_low or (len(t_low) >= 5 and (t_low in yt_low or yt_low in t_low)):
                return "YouTube"

        # В Яндекс Музыке поле album почти всегда заполнено (название альбома/сингла), а в YouTube оно пустое
        if album and tab_info["has_yandex"]:
            return "Яндекс Музыка"
        if tab_info["has_yandex"] and not tab_info["has_youtube"]:
            return "Яндекс Музыка"
        if tab_info["has_youtube"] and not tab_info["has_yandex"] and not album:
            return "YouTube"
        if album:
            return "Яндекс Музыка"

    return raw_source

# ============================================================================
# Системные вызовы управления
# ============================================================================
def fetch_system_media_state():
    """Получает системное состояние NowPlaying и громкости из macOS."""
    res = query_media_helper("GET")
    if res and isinstance(res, dict):
        res["title"] = clean_title_text(res.get("title", ""))
        if res.get("title"):
            res["source"] = classify_media_source(res)
        return res
    return None

def set_mac_volume(percent):
    """Устанавливает системную громкость macOS через CoreAudio."""
    percent = max(0, min(100, int(percent)))
    res = query_media_helper(f"SET_VOLUME {percent}")
    if res and res.get("success"):
        with state_lock:
            current_state["volume"] = percent
        broadcast_state(force=True)
        return True, percent
    return False, percent

def execute_media_command(command, val=None):
    """Выполняет команду воспроизведения через MediaRemote (и дублирует в Browser Companion при необходимости)."""
    cmd_str = f"CMD {command}"
    if val is not None:
        cmd_str += f" {val}"

    res = query_media_helper(cmd_str)
    if res and res.get("success"):
        logger.info(f"[ROUTER] Команда {command} (val={val}) выполнена через MediaRemote")
        # Если команда вернула новую целевую позицию (например, при перемотке), сразу обновляем и рассылаем
        if "position" in res:
            with state_lock:
                current_state["position"] = int(res["position"])
        else:
            # Даем 120 мс плееру обновить статус (например, Play/Pause или Next Track)
            time.sleep(0.12)
        sys_st = fetch_system_media_state()
        if sys_st:
            with state_lock:
                current_state["volume"] = sys_st.get("volume", current_state["volume"])
                if sys_st.get("title"):
                    current_state["title"] = sys_st.get("title", "")
                    current_state["artist"] = sys_st.get("artist", "")
                    current_state["source"] = sys_st.get("source", current_state["source"])
                    current_state["state"] = sys_st.get("state", current_state["state"])
                    current_state["position"] = int(sys_st.get("position", current_state["position"]))
                    current_state["duration"] = int(sys_st.get("duration", current_state["duration"]))
        broadcast_state(force=True)
        return True, "system_media_remote"

    # Резерв: отправка в очередь Browser Companion Userscript
    is_browser_active = False
    with state_lock:
        if browser_data["active"] and (time.time() - browser_data["last_seen"] < 4.0):
            is_browser_active = True

    if is_browser_active and command in ["PLAY_PAUSE", "NEXT_TRACK", "PREVIOUS_TRACK", "SEEK_FORWARD", "SEEK_BACKWARD", "SEEK_TO"]:
        logger.info(f"[ROUTER] Перенаправление команды {command} в Browser Companion")
        cmd_obj = {"command": command}
        if val is not None:
            cmd_obj["value"] = val
        with state_lock:
            browser_pending_commands.append(cmd_obj)
        return True, "routed_to_browser"

    logger.warning(f"[ROUTER] Команда {command} не удалась")
    return False, "unsupported_or_failed"

# ============================================================================
# Рассылка клиентам ESP32 (с защитой от джиттера прогресс-бара)
# ============================================================================
last_sent_state = {}
last_sent_time = 0.0
last_minute = -1

def broadcast_state(force=False):
    """Рассылает актуальное состояние всем подключенным клиентам ESP32."""
    global last_sent_state, last_sent_time, last_minute
    now = time.time()

    with state_lock:
        t_dict, d_str = get_current_time_payload()
        current_state["time"] = t_dict
        current_state["date"] = d_str
        st_copy = dict(current_state)
        st_copy["connected"] = True

    # Проверяем смену минуты (для гарантированного обновления часов в шапке)
    cur_min = st_copy["time"]["minutes"]
    if cur_min != last_minute:
        last_minute = cur_min
        force = True

    if not force and last_sent_state:
        # 1. Проверяем изменение метаданных, статуса воспроизведения, длительности или громкости
        meta_cur = (
            st_copy.get("title"),
            st_copy.get("artist"),
            st_copy.get("state"),
            st_copy.get("volume"),
            st_copy.get("duration"),
            st_copy.get("source"),
            st_copy.get("weather"),
        )
        meta_last = (
            last_sent_state.get("title"),
            last_sent_state.get("artist"),
            last_sent_state.get("state"),
            last_sent_state.get("volume"),
            last_sent_state.get("duration"),
            last_sent_state.get("source"),
            last_sent_state.get("weather"),
        )

        if meta_cur == meta_last:
            # Метаданные и статус не менялись — проверяем, был ли скачок позиции (перемотка на Mac)
            pos_cur = int(st_copy.get("position", 0))
            pos_last = int(last_sent_state.get("position", 0))
            elapsed_wall = max(0.0, now - last_sent_time)

            if st_copy.get("state") == "playing":
                expected_pos = pos_last + elapsed_wall
                pos_Jump = abs(pos_cur - expected_pos) >= 2.2
                periodic_sync = elapsed_wall >= 5.0
                if not pos_Jump and not periodic_sync:
                    return
            else:
                # На паузе или в стопе рассылаем только если позиция реально изменилась (ручная перемотка)
                if pos_cur == pos_last:
                    return

    msg = json.dumps(st_copy, ensure_ascii=False) + "\n"
    data = msg.encode("utf-8")
    last_sent_state = dict(st_copy)
    last_sent_time = now
    broadcast_raw_bytes(data)

# ============================================================================
# HTTP Сервер для Browser Companion Userscript
# ============================================================================
class BrowserBridgeHandler(BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_POST(self):
        if self.path == "/api/browser_update":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                data = json.loads(body.decode("utf-8"))
                with state_lock:
                    browser_data["active"] = True
                    browser_data["last_seen"] = time.time()
                    browser_data["title"] = clean_title_text(data.get("title", ""))
                    browser_data["artist"] = data.get("artist", "")
                    browser_data["source"] = data.get("source", "YouTube")
                    browser_data["position"] = int(data.get("position", 0))
                    browser_data["duration"] = int(data.get("duration", 0))
                    browser_data["state"] = data.get("state", "paused")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(b'{"status":"ok"}')
                return
            except Exception as e:
                logger.error(f"Ошибка парсинга browser_update: {e}")
                self.send_response(400)
                self.end_headers()
                return

        elif self.path == "/api/text":
            # Локальная отладка: текстовая команда без микрофона (только с самого MacBook)
            if self.client_address[0] not in ("127.0.0.1", "::1"):
                self.send_response(403)
                self.end_headers()
                return
            length = int(self.headers.get("Content-Length", 0))
            try:
                data = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
                text = str(data.get("text", "")).strip()
                with state_lock:
                    st_snapshot = dict(current_state)

                def _send_thinking_to_esp(thinking_payload: dict):
                    broadcast_raw_bytes((json.dumps(thinking_payload, ensure_ascii=False) + "\n").encode("utf-8"))

                result = process_voice_command(
                    text, st_snapshot, set_mac_volume, execute_media_command,
                    on_thinking_fn=_send_thinking_to_esp,
                    bypass_wake=True,
                )
                payload_str = json.dumps(result, ensure_ascii=False)
                if result.get("status") != "ignored":
                    broadcast_raw_bytes((payload_str + "\n").encode("utf-8"))
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.end_headers()
                self.wfile.write(payload_str.encode("utf-8"))
                return
            except Exception as e:
                logger.error(f"Ошибка обработки /api/text: {e}")
                self.send_response(500)
                self.end_headers()
                return

        elif self.path == "/api/voice":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length > 0 else b""
            duration_sec = len(body) / (8000.0 * 2.0)

            # Защита от самовозбуждения (эха от динамиков MacBook во время синтеза речи TTS)
            if is_tts_speaking_or_tail():
                logger.info(f"[VOICE RX] Игнорировано эхо динамиков MacBook ({duration_sec:.2f} сек)")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.end_headers()
                self.wfile.write(b'{"type":"voice_result","status":"echo_ignored"}')
                return

            logger.info(f"[VOICE RX] Получено аудио с микрофона ESP32-S3: {len(body)} байт ({duration_sec:.2f} сек, 8000 Гц)")
            try:
                text = recognize_pcm16(body, sample_rate=8000)
                with state_lock:
                    st_snapshot = dict(current_state)

                def _send_thinking_to_esp(thinking_payload: dict):
                    raw_line = (json.dumps(thinking_payload, ensure_ascii=False) + "\n").encode("utf-8")
                    broadcast_raw_bytes(raw_line)

                result = process_voice_command(
                    text,
                    st_snapshot,
                    set_mac_volume,
                    execute_media_command,
                    on_thinking_fn=_send_thinking_to_esp,
                    bypass_wake=False,
                )
                payload_str = json.dumps(result, ensure_ascii=False)
                # Рассылаем результат на ESP32 только если команда принята (не ignored)
                if result.get("status") != "ignored":
                    broadcast_raw_bytes((payload_str + "\n").encode("utf-8"))

                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(payload_str.encode("utf-8"))
                return
            except Exception as e:
                logger.error(f"Ошибка обработки /api/voice: {e}")
                self.send_response(500)
                self.end_headers()
                return

        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        global browser_pending_commands
        if self.path == "/api/browser_commands":
            with state_lock:
                cmds = list(browser_pending_commands)
                browser_pending_commands.clear()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(cmds).encode("utf-8"))
            return

        if self.path == "/api/status":
            with state_lock:
                st = dict(current_state)
                st["equalizer"] = dict(current_eq_state)
            if audio_service is not None:
                st["audio_capture"] = audio_service.get_snapshot()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(st, ensure_ascii=False).encode("utf-8"))
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, format, *args):
        pass

def run_http_bridge():
    server = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), BrowserBridgeHandler)
    server.daemon_threads = True
    logger.info(f"HTTP Browser & Voice Bridge запущен на http://0.0.0.0:{HTTP_PORT}")
    server.serve_forever()

# ============================================================================
# Фоновый цикл опроса медиасостояния (350 мс)
# ============================================================================
def poll_loop():
    logger.info("Фоновый цикл опроса медиасостояния запущен.")
    while running:
        try:
            now = time.time()
            browser_is_live = False
            with state_lock:
                if browser_data["active"] and (now - browser_data["last_seen"] < 3.5):
                    browser_is_live = True

            # Получаем системное состояние через постоянный демон Swift MediaHelper
            sys_state = fetch_system_media_state()

            with state_lock:
                if sys_state:
                    current_state["volume"] = sys_state.get("volume", current_state["volume"])

                # 1. Основной источник: системный MediaRemote (MRNowPlayingRequest)
                # Он работает нативно для YouTube, Яндекс Музыки, Apple Music, Spotify в Safari и Chrome
                if sys_state and sys_state.get("title"):
                    current_state["title"] = sys_state.get("title", "")
                    current_state["artist"] = sys_state.get("artist", "")
                    current_state["source"] = sys_state.get("source", "macOS")
                    current_state["state"] = sys_state.get("state", "stopped")
                    current_state["position"] = int(sys_state.get("position", 0))
                    current_state["duration"] = int(sys_state.get("duration", 0))

                # 2. Дополнение из Userscript, если системный источник пуст
                elif browser_is_live and browser_data.get("title"):
                    current_state["title"] = browser_data["title"]
                    current_state["artist"] = browser_data["artist"]
                    current_state["source"] = browser_data["source"]
                    current_state["state"] = browser_data["state"]
                    current_state["position"] = int(browser_data["position"])
                    current_state["duration"] = int(browser_data["duration"])

                # 3. Воспроизведение не обнаружено / остановлено
                else:
                    current_state["title"] = ""
                    current_state["artist"] = ""
                    current_state["source"] = "macOS"
                    current_state["state"] = "stopped"
                    current_state["position"] = 0
                    current_state["duration"] = 0

            broadcast_state()

        except Exception as e:
            logger.error(f"Исключение в poll_loop: {e}")

        time.sleep(0.35)

# ============================================================================
# Обработка команд от ESP32
# ============================================================================
def handle_esp_command(cmd_obj, client_sock):
    """Обрабатывает входящее JSON-сообщение от ESP32."""
    msg_type = cmd_obj.get("type")

    if msg_type == "auth":
        token = cmd_obj.get("token", "")
        if token == AUTH_TOKEN:
            logger.info("ESP32 успешно авторизован.")
            resp = {"type": "response", "command": "auth", "success": True}
        else:
            logger.warning(f"ESP32 передал неверный токен авторизации: {token}")
            resp = {"type": "response", "command": "auth", "success": False, "error": "Invalid auth token"}
        send_line_to_client(client_sock, resp)
        return

    if msg_type != "command":
        logger.warning(f"Получено неизвестное сообщение: {cmd_obj}")
        return

    cmd = cmd_obj.get("command", "")
    val = cmd_obj.get("value")
    logger.info(f"[COMMAND FROM ESP32] {cmd} (значение: {val})")

    resp = {"type": "response", "command": cmd, "success": False}

    if cmd == "GET_STATUS":
        with state_lock:
            t_dict, d_str = get_current_time_payload()
            current_state["time"] = t_dict
            current_state["date"] = d_str
            st = dict(current_state)
            st["connected"] = True
            eq_st = dict(current_eq_state)
        send_line_to_client(client_sock, st)
        send_line_to_client(client_sock, eq_st)
        return

    elif cmd == "SET_VOLUME":
        if val is not None:
            ok, cur_vol = set_mac_volume(val)
            resp["success"] = ok
            resp["volume"] = cur_vol
        else:
            resp["error"] = "Missing value"

    elif cmd == "VOLUME_UP":
        step = cmd_obj.get("step", 5)
        with state_lock:
            cur = current_state["volume"]
        ok, cur_vol = set_mac_volume(cur + step)
        resp["success"] = ok
        resp["volume"] = cur_vol

    elif cmd == "VOLUME_DOWN":
        step = cmd_obj.get("step", 5)
        with state_lock:
            cur = current_state["volume"]
        ok, cur_vol = set_mac_volume(cur - step)
        resp["success"] = ok
        resp["volume"] = cur_vol

    elif cmd == "PLAY_PAUSE":
        ok, detail = execute_media_command("PLAY_PAUSE")
        resp["success"] = ok
        resp["detail"] = detail

    elif cmd == "NEXT_TRACK":
        ok, detail = execute_media_command("NEXT_TRACK")
        resp["success"] = ok
        resp["detail"] = detail

    elif cmd == "PREVIOUS_TRACK":
        ok, detail = execute_media_command("PREVIOUS_TRACK")
        resp["success"] = ok
        resp["detail"] = detail

    elif cmd == "SEEK_FORWARD":
        seconds = cmd_obj.get("seconds", val if val is not None else 10)
        ok, detail = execute_media_command("SEEK_FORWARD", seconds)
        resp["success"] = ok
        resp["detail"] = detail

    elif cmd == "SEEK_BACKWARD":
        seconds = cmd_obj.get("seconds", val if val is not None else 10)
        ok, detail = execute_media_command("SEEK_BACKWARD", seconds)
        resp["success"] = ok
        resp["detail"] = detail

    elif cmd == "SEEK_TO":
        pos = cmd_obj.get("position", val)
        if pos is not None:
            ok, detail = execute_media_command("SEEK_TO", pos)
            resp["success"] = ok
            resp["detail"] = detail
        else:
            resp["error"] = "Missing position"
    else:
        resp["error"] = f"Unknown command: {cmd}"

    try:
        send_line_to_client(client_sock, resp)
    except Exception as e:
        logger.warning(f"Не удалось отправить ответ клиенту: {e}")

# ============================================================================
# TCP Сервер для ESP32
# ============================================================================
def client_reader_thread(client_sock, client_addr):
    """Поток чтения сообщений от конкретного подключенного клиента ESP32."""
    logger.info(f"Клиент ESP32 подключился: {client_addr}")
    try:
        client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except Exception:
        pass

    with state_lock:
        connected_esp_clients.append(client_sock)
        t_dict, d_str = get_current_time_payload()
        current_state["time"] = t_dict
        current_state["date"] = d_str
        init_st = dict(current_state)
        init_st["connected"] = True
        init_eq = dict(current_eq_state)

    try:
        send_line_to_client(client_sock, init_st)
        send_line_to_client(client_sock, init_eq)
    except Exception as e:
        logger.warning(f"Ошибка первичной отправки клиенту: {e}")

    buffer = ""
    client_sock.settimeout(1.0)

    while running:
        try:
            chunk = client_sock.recv(2048)
            if not chunk:
                break
            buffer += chunk.decode("utf-8", errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if line:
                    try:
                        cmd_obj = json.loads(line)
                        handle_esp_command(cmd_obj, client_sock)
                    except json.JSONDecodeError:
                        logger.warning(f"Некорректный JSON от ESP32: {line}")
        except socket.timeout:
            continue
        except Exception as e:
            logger.info(f"Соединение с клиентом {client_addr} разорвано: {e}")
            break

    with state_lock:
        if client_sock in connected_esp_clients:
            connected_esp_clients.remove(client_sock)
    try:
        client_sock.close()
    except Exception:
        pass
    logger.info(f"Клиент ESP32 отключен: {client_addr}")

def run_tcp_server():
    """Главный поток TCP-сервера."""
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server_sock.bind(("0.0.0.0", TCP_PORT))
        server_sock.listen(5)
        logger.info(f"TCP Сервер для ESP32 запущен на 0.0.0.0:{TCP_PORT}")
    except Exception as e:
        logger.error(f"Не удалось привязать TCP порт {TCP_PORT}: {e}")
        return

    server_sock.settimeout(1.0)
    while running:
        try:
            client_sock, client_addr = server_sock.accept()
            t = threading.Thread(target=client_reader_thread, args=(client_sock, client_addr), daemon=True)
            t.start()
        except socket.timeout:
            continue
        except Exception as e:
            if running:
                logger.error(f"Ошибка в accept TCP: {e}")
            break

    server_sock.close()

# ============================================================================
# Главная функция запуска
# ============================================================================
def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

def main():
    global running, audio_service
    print("=" * 60)
    print("   MacMediaService - Gateway для ESP32-S3 Voice Assistant")
    print(f"   Локальный IP MacBook в сети: {get_local_ip()}")
    print(f"   Порт TCP для ESP32:          {TCP_PORT}")
    print(f"   Порт HTTP для Companion Script: {HTTP_PORT}")
    print(f"   Аудиоэквалайзер:             {EQ_BAND_COUNT} полос ({EQ_TARGET_FPS} FPS, ScreenCaptureKit)")
    print("=" * 60)

    # Запуск фонового сервиса анализа системного аудио (ScreenCaptureKit + FFT)
    audio_service = AudioEqualizerService(
        base_dir=BASE_DIR,
        on_equalizer_frame=on_equalizer_update,
        band_count=EQ_BAND_COUNT,
        target_fps=EQ_TARGET_FPS,
    )
    audio_service.start()

    # Асинхронные события от голосового модуля (окончание таймера и т.п.) -> всем ESP32
    set_event_broadcaster(
        lambda payload: broadcast_raw_bytes((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
    )

    t_poll = threading.Thread(target=poll_loop, daemon=True)
    t_poll.start()

    t_weather = threading.Thread(target=weather_and_sys_loop, daemon=True)
    t_weather.start()

    t_http = threading.Thread(target=run_http_bridge, daemon=True)
    t_http.start()

    t_tcp = threading.Thread(target=run_tcp_server, daemon=True)
    t_tcp.start()

    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        logger.info("Завершение работы MacMediaService по сигналу пользователя...")
        running = False
        if audio_service is not None:
            audio_service.stop()
        if helper_proc is not None:
            try:
                helper_proc.kill()
            except Exception:
                pass
        time.sleep(0.3)
        sys.exit(0)

if __name__ == "__main__":
    main()
