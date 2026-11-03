#!/usr/bin/env python3
"""
Модуль распознавания речи (STT), локальной языковой модели (Ollama qwen2.5:3b)
с доступом в интернет (DuckDuckGo + Wikipedia RU), выполнения голосовых команд
и нейросетевого синтеза речи (gTTS + afplay с кэшированием) через динамики MacBook
для компактного голосового помощника на ESP32-S3.
Включает защиту от акустической обратной связи (эха динамиков MacBook в микрофон INMP441).
"""

import os
import re
import html
import json
import time
import array
import hashlib
import urllib.parse
import urllib.request
import datetime
import tempfile
import threading
import subprocess
import logging
from concurrent.futures import ThreadPoolExecutor
import speech_recognition as sr
import requests
from gtts import gTTS

logger = logging.getLogger("MacMediaService.Voice")

# ============================================================================
# Конфигурация AI-бэкенда (Локальный MacBook vs Удалённый мощный ПК с GPU 16GB)
# ============================================================================
AI_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ai_config.json")


def load_ai_config() -> dict:
    defaults = {
        "remote_ai_host": os.getenv("REMOTE_AI_HOST", "").strip(),
        "voice_gender": os.getenv("VOICE_GENDER", "male").strip().lower(),
        "remote_ollama_port": int(os.getenv("REMOTE_OLLAMA_PORT", "11434")),
        "remote_ollama_model": os.getenv("REMOTE_OLLAMA_MODEL", "qwen2.5:7b"),
        "remote_tts_port": int(os.getenv("REMOTE_TTS_PORT", "8001")),
        "fallback_local_ollama": True,
        "local_ollama_url": "http://127.0.0.1:11434/api/chat",
        "local_ollama_model": "qwen2.5:3b",
        "remote_check_timeout_sec": 0.8,
    }
    if os.path.exists(AI_CONFIG_PATH):
        try:
            with open(AI_CONFIG_PATH, "r", encoding="utf-8") as f:
                cfg = json.load(f)
                for k, v in cfg.items():
                    if not k.startswith("_") and k in defaults:
                        defaults[k] = v
        except Exception as e:
            logger.warning(f"[AI CONFIG] Ошибка чтения ai_config.json: {e}")
    # Переменная окружения имеет наивысший приоритет
    if os.getenv("REMOTE_AI_HOST"):
        defaults["remote_ai_host"] = os.getenv("REMOTE_AI_HOST", "").strip()
    return defaults


_ai_config = load_ai_config()
_remote_ollama_status = ("unknown", 0.0)  # ("online"|"offline"|"unknown", timestamp)
_remote_tts_status = ("unknown", 0.0)

OLLAMA_URL = _ai_config.get("local_ollama_url", "http://127.0.0.1:11434/api/chat")
OLLAMA_MODEL = _ai_config.get("local_ollama_model", "qwen2.5:3b")
TTS_CACHE_DIR = os.path.join(tempfile.gettempdir(), "esp32_tts_cache")
os.makedirs(TTS_CACHE_DIR, exist_ok=True)

_recognizer = sr.Recognizer()
_tts_lock = threading.Lock()
_tts_proc = None
_tts_pending = False   # Идёт синтез речи (ещё не началось воспроизведение) — микрофон тоже считается «эхом»
_tts_mute_until = 0.0  # Timestamp, до которого микрофонные записи считаются эхом динамиков MacBook
_chat_history = []     # История последних реплик диалога для контекста LLM
_web_cache = {}        # Кэш поисковых запросов: query -> (timestamp, snippets)
_event_broadcast_fn = None  # Callback сервера для асинхронных событий на ESP32 (окончание таймера и т.п.)

# ============================================================================
# Профили голосов (Мужской / Женский) — согласованы для казахского и русского
# ============================================================================
VOICE_PROFILES = {
    "male": {
        "kk": "kk-KZ-DauletNeural",    # Естественный казахский мужской нейроголос (Дәулет)
        "ru": "ru-RU-DmitryNeural",    # Естественный русский мужской нейроголос (Дмитрий)
        "omni": "male",                # Локальный/удалённый OmniVoice: мужской голос Доса
        "name_ru": "Мужской (Дмитрий / Дәулет)",
        "name_kz": "Ер адам дауысы (Дәулет)",
    },
    "female": {
        "kk": "kk-KZ-AigulNeural",     # Естественный казахский женский нейроголос (Айгүл)
        "ru": "ru-RU-SvetlanaNeural",  # Естественный русский женский нейроголос (Светлана)
        "omni": "female",              # Локальный/удалённый OmniVoice: женский голос
        "name_ru": "Женский (Светлана / Айгүл)",
        "name_kz": "Әйел дауысы (Айгүл)",
    },
}

EDGE_RATE = "+0%"
LOCAL_TTS_API_URL = "http://127.0.0.1:8001/synthesize"
LOCAL_TTS_HEALTH_URL = "http://127.0.0.1:8001/health"
OMNIVOICE_SYNTH_TIMEOUT = 25.0  # Запас времени на нейросетевую генерацию на GPU (OmniVoice)
OMNIVOICE_RETRY_SEC = 300       # После сбоя/неготовности OmniVoice не опрашивается 5 минут
_omnivoice_state = ("unknown", 0.0)  # (статус, timestamp последней проверки)


def get_voice_gender() -> str:
    """Возвращает текущий пол голоса ('male' или 'female')."""
    g = (_ai_config.get("voice_gender") or "male").strip().lower()
    return "female" if g in ("female", "woman", "girl", "әйел", "қыз", "женский") else "male"


def set_voice_gender(gender: str) -> str:
    """Переключает пол голоса ('male' или 'female') и сохраняет выбор в ai_config.json."""
    global _ai_config
    norm = "female" if gender.strip().lower() in ("female", "woman", "girl", "әйел", "қыз", "женский") else "male"
    _ai_config["voice_gender"] = norm
    try:
        cfg_path = AI_CONFIG_PATH
        existing = {}
        if os.path.exists(cfg_path):
            with open(cfg_path, "r", encoding="utf-8") as f:
                existing = json.load(f)
        existing["voice_gender"] = norm
        with open(cfg_path, "w", encoding="utf-8") as f:
            json.dump(existing, f, ensure_ascii=False, indent=2)
        logger.info(f"[VOICE CONFIG] Пол голоса переключён на: {norm.upper()} ({VOICE_PROFILES[norm]['name_ru']})")
    except Exception as e:
        logger.warning(f"[VOICE CONFIG] Не удалось сохранить voice_gender: {e}")
    return norm


def get_ollama_endpoint() -> tuple:
    """
    Возвращает (url, model, is_remote).
    Если remote_ai_host задан в ai_config.json и доступен по сети — направляет тяжёлые
    вычисления на ПК с GPU 16GB.
    Если удалённый ПК недоступен (выключен, спит, нет сети) — мгновенно переключается
    на локальную Ollama MacBook (Fallback).
    """
    global _remote_ollama_status
    cfg = _ai_config
    remote_host = (cfg.get("remote_ai_host") or "").strip()
    local_url = cfg.get("local_ollama_url", "http://127.0.0.1:11434/api/chat")
    local_model = cfg.get("local_ollama_model", "qwen2.5:3b")

    if not remote_host:
        return local_url, local_model, False

    remote_port = cfg.get("remote_ollama_port", 11434)
    remote_url = f"http://{remote_host}:{remote_port}/api/chat"
    remote_model = cfg.get("remote_ollama_model", "qwen2.5:3b")
    now = time.time()
    st, ts = _remote_ollama_status

    if st == "online" and (now - ts < 25.0):
        return remote_url, remote_model, True
    if st == "offline" and (now - ts < 20.0):
        return local_url, local_model, False

    # Быстрая проверка здоровья удалённого Ollama (/api/version)
    version_url = f"http://{remote_host}:{remote_port}/api/version"
    try:
        req = urllib.request.Request(version_url)
        with urllib.request.urlopen(req, timeout=cfg.get("remote_check_timeout_sec", 0.8)) as r:
            if r.status == 200:
                if st != "online":
                    logger.info(f"[AI BACKEND] Удалённый ПК с GPU подключён: http://{remote_host}:{remote_port} (модель: {remote_model})")
                _remote_ollama_status = ("online", now)
                return remote_url, remote_model, True
    except Exception as e:
        if st != "offline":
            logger.warning(f"[AI BACKEND] Удалённый ПК ({remote_host}:{remote_port}) недоступен ({e}). Автоматический Fallback на локальный MacBook.")
        _remote_ollama_status = ("offline", now)

    return local_url, local_model, False


def mark_remote_ollama_failed(reason: str = ""):
    global _remote_ollama_status
    _remote_ollama_status = ("offline", time.time())
    if reason:
        logger.warning(f"[AI BACKEND] Удалённый Ollama временно отключён: {reason}")


def get_tts_endpoint() -> tuple:
    """
    Возвращает (synthesize_url, health_url, is_remote).
    Приоритет: удалённый ПК с GPU 16GB (если запущен OmniVoice) -> локальный MacBook.
    """
    global _remote_tts_status
    cfg = _ai_config
    remote_host = (cfg.get("remote_ai_host") or "").strip()
    if not remote_host:
        return LOCAL_TTS_API_URL, LOCAL_TTS_HEALTH_URL, False

    remote_tts_port = cfg.get("remote_tts_port", 8001)
    remote_synth = f"http://{remote_host}:{remote_tts_port}/synthesize"
    remote_health = f"http://{remote_host}:{remote_tts_port}/health"

    now = time.time()
    st, ts = _remote_tts_status
    if st == "online" and (now - ts < 30.0):
        return remote_synth, remote_health, True
    if st == "offline" and (now - ts < 30.0):
        return LOCAL_TTS_API_URL, LOCAL_TTS_HEALTH_URL, False

    try:
        r = requests.get(remote_health, timeout=cfg.get("remote_check_timeout_sec", 0.8))
        if r.status_code == 200 and r.json().get("status") == "ready":
            if st != "online":
                logger.info(f"[VOICE TTS] Удалённый OmniVoice на ПК активен: {remote_synth}")
            _remote_tts_status = ("online", now)
            return remote_synth, remote_health, True
    except Exception:
        pass

    _remote_tts_status = ("offline", now)
    return LOCAL_TTS_API_URL, LOCAL_TTS_HEALTH_URL, False

# Триггер-слово «Дос» и окно активного диалога
_wake_active_until = 0.0   # Timestamp активного окна диалога после оклика «Дос» (в секундах)
_wake_lock = threading.Lock()

_WAKE_WORD_RE = re.compile(r"(?i)\b(досжан|досым|досың|досын|доспысың|доспысын|доспын|доссың|доссын|достар|доспен|доске|досик|дос|dos)\b")
_LEADING_GREETINGS_RE = re.compile(
    r"(?i)^\s*(?:ассалаумағалейкум|ассаламуалейкум|ассалаумалейкум|сәлеметсізбе|сәлеметсіз\s*бе|"
    r"қайырлы\s*күн|сәлем|салем|привет|эй|слушай|тыңда|алло|хеллоу|hello|hey|ок|окей|жарайды)?\s*"
    r"(?:досжан|досым|досың|досын|доспысың|доспысын|доспын|доссың|доссын|достар|доспен|доске|досик|дос|dos)\s*(?:жан|досым)?\s*[,!?:;-]*\s*"
)

_KAZAKH_LETTERS = set("әғқңөұүһіӘҒҚҢӨҰҮҺІ")
_KAZAKH_MARKERS = {
    "сәлем", "дос", "қалай", "қалайсың", "жақсы", "күн", "рахмет", "рақмет",
    "көмек", "әуен", "музыка", "ойнат", "қос", "тоқтат", "жап", "аш", "өшір",
    "тыңда", "кім", "қайда", "не", "неге", "болады", "болды", "уақыт", "сағат"
}

try:
    import asyncio
    import edge_tts
    _EDGE_TTS_OK = True
except Exception:
    _EDGE_TTS_OK = False

try:
    from kazakh_stt import get_stt_engine
except Exception as _e:
    logger.warning(f"Не удалось импортировать локальный STT (kazakh_stt): {_e}")
    get_stt_engine = None


def set_event_broadcaster(fn):
    """Регистрирует функцию сервера для рассылки асинхронных событий на ESP32-S3."""
    global _event_broadcast_fn
    _event_broadcast_fn = fn


def is_tts_speaking_or_tail() -> bool:
    """Возвращает True, если сейчас звучит голос TTS на MacBook или идёт акустический хвост после него."""
    with _tts_lock:
        if _tts_pending:
            return True
        if _tts_proc is not None and _tts_proc.poll() is None:
            return True
        return time.time() < _tts_mute_until


_HOMOGLYPHS = {
    'a': 'а', 'e': 'е', 'i': 'і', 'o': 'о', 'p': 'р', 'c': 'с', 'y': 'у', 'x': 'х',
    'A': 'А', 'B': 'В', 'E': 'Е', 'K': 'К', 'M': 'М', 'H': 'Н', 'O': 'О', 'P': 'Р',
    'C': 'С', 'T': 'Т', 'Y': 'Ү', 'X': 'Х'
}

def _fix_mixed_word(w: str) -> str:
    has_cyr = bool(re.search(r'[а-яёәғқңөұүһіА-ЯЁӘҒҚҢӨҰҮҺІ]', w))
    has_lat = bool(re.search(r'[a-zA-Z]', w))
    if has_cyr and has_lat:
        w = re.sub(r'^[a-zA-Z]+(?=[а-яёәғқңөұүһіА-ЯЁӘҒҚҢӨҰҮҺІ])', '', w)
        w = re.sub(r'(?<=[а-яёәғқңөұүһіА-ЯЁӘҒҚҢӨҰҮҺІ])[a-zA-Z]+$', '', w)
        return ''.join(_HOMOGLYPHS.get(ch, ch) for ch in w)
    return w

def _clean_for_tts(text: str) -> str:
    """
    Очищает текст от ссылок, спецсимволов, смайликов и паразитных латинских букв
    внутри кириллицы (например: 'nда, belgilenet), которые вызывают резкий акцент и заикания TTS.
    """
    if not text:
        return ""
    cleaned = re.sub(r"https?://\S+|www\.\S+", " ", text)
    cleaned = re.sub(r"[*_`#~<>^•|\[\]{}«»\"'()]", " ", cleaned)
    cleaned = re.sub(r"(?<=[а-яёәғқңөұүһіА-ЯЁӘҒҚҢӨҰҮҺІ])'[a-zA-Z]+", "", cleaned)
    words = [_fix_mixed_word(w) for w in cleaned.split()]
    cleaned = " ".join(words)
    cleaned = re.sub(r"\b[a-zA-Z]{1,2}\b", " ", cleaned)  # одиночные латинские буквы
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    return cleaned


def _is_kazakh_text(text: str) -> bool:
    if any(ch in _KAZAKH_LETTERS for ch in text):
        return True
    words = set(re.findall(r"[a-zа-яёәғқңөұүһі]+", text.lower()))
    return bool(words & _KAZAKH_MARKERS)


def _omnivoice_ready() -> bool:
    """
    Быстрая (≤0.3 сек) проверка готовности OmniVoice с кэшированием результата.
    После ошибки OmniVoice отключается на OMNIVOICE_RETRY_SEC, чтобы не тормозить каждый ответ.
    """
    global _omnivoice_state
    now = time.time()
    status, ts = _omnivoice_state
    if status == "ready" and now - ts < 60:
        return True
    if status in ("down", "failed") and now - ts < OMNIVOICE_RETRY_SEC:
        return False
    try:
        r = requests.get(LOCAL_TTS_HEALTH_URL, timeout=0.3)
        ok = r.status_code == 200 and r.json().get("status") == "ready"
    except Exception:
        ok = False
    _omnivoice_state = ("ready" if ok else "down", now)
    return ok


def _omnivoice_mark_failed(reason: str):
    global _omnivoice_state
    _omnivoice_state = ("failed", time.time())
    logger.info(f"[VOICE TTS] OmniVoice отключён на {OMNIVOICE_RETRY_SEC} сек ({reason[:80]}), использую Edge TTS")


def _synthesize_to_cache(txt: str) -> tuple:
    """
    Синтезирует речь в кэш с учётом выбранного профиля голоса (Мужской / Женский).
    Возвращает (путь_к_аудиофайлу, движок) или (None, None).
    Порядок:
    1. Нейросетевой OmniVoice API (Удалённый ПК с GPU 16GB или локальный MacBook)
    2. Microsoft Edge Neural TTS:
       - Мужской: kk-KZ-DauletNeural / ru-RU-DmitryNeural
       - Женский: kk-KZ-AigulNeural / ru-RU-SvetlanaNeural
    3. gTTS (резервный)
    """
    is_kk = _is_kazakh_text(txt)
    gender = get_voice_gender()
    profile = VOICE_PROFILES.get(gender, VOICE_PROFILES["male"])

    # 1. Нейросетевой OmniVoice API (для казахоязычного ассистента Дос)
    if is_kk:
        tts_url, health_url, is_remote = get_tts_endpoint()
        can_try_omni = is_remote or _omnivoice_ready()
        if can_try_omni:
            try:
                omni_voice = profile.get("omni", "male")
                resp = requests.post(
                    tts_url,
                    json={"text": txt, "voice": omni_voice, "play_now": False},
                    timeout=OMNIVOICE_SYNTH_TIMEOUT
                )
                if resp.status_code == 200:
                    data = resp.json()
                    # Если локальный файл уже доступен (на этой же машине)
                    local_candidate = data.get("audio_path")
                    if local_candidate and os.path.exists(local_candidate) and os.path.getsize(local_candidate) > 1024:
                        return local_candidate, "omnivoice"

                    # Если файл сгенерирован на удалённом ПК — скачиваем его через audio_url
                    audio_rel = data.get("audio_url")
                    if audio_rel:
                        base_root = tts_url.rsplit("/synthesize", 1)[0]
                        dl_url = f"{base_root}{audio_rel}"
                        key = hashlib.md5(f"remote_omni|{omni_voice}|{txt}".encode("utf-8")).hexdigest()
                        cached_remote_wav = os.path.join(TTS_CACHE_DIR, f"{key}.wav")
                        with urllib.request.urlopen(dl_url, timeout=5.0) as dl_resp:
                            with open(cached_remote_wav, "wb") as f_out:
                                f_out.write(dl_resp.read())
                        if os.path.exists(cached_remote_wav) and os.path.getsize(cached_remote_wav) > 1024:
                            return cached_remote_wav, "omnivoice"
                else:
                    _omnivoice_mark_failed(f"HTTP {resp.status_code}")
            except Exception as e:
                _omnivoice_mark_failed(str(e))

    # 2. Microsoft Edge Neural TTS
    voice = profile["kk"] if is_kk else profile["ru"]
    if _EDGE_TTS_OK:
        key = hashlib.md5(f"edge|{voice}|{EDGE_RATE}|{txt}".encode("utf-8")).hexdigest()
        mp3_path = os.path.join(TTS_CACHE_DIR, f"{key}.mp3")
        if os.path.exists(mp3_path) and os.path.getsize(mp3_path) > 512:
            return mp3_path, "edge"
        try:
            tmp_path = mp3_path + ".part"

            async def _run():
                await edge_tts.Communicate(txt, voice, rate=EDGE_RATE).save(tmp_path)

            asyncio.run(_run())
            if os.path.exists(tmp_path) and os.path.getsize(tmp_path) > 512:
                os.replace(tmp_path, mp3_path)
                return mp3_path, "edge"
        except Exception as e:
            logger.warning(f"[VOICE TTS] Edge TTS ({voice}) недоступен ({e}), переключаюсь на gTTS")

    # 3. Резервный gTTS (Google TTS)
    key = hashlib.md5(f"gtts|{is_kk}|{gender}|{txt}".encode("utf-8")).hexdigest()
    mp3_path = os.path.join(TTS_CACHE_DIR, f"{key}.mp3")
    if os.path.exists(mp3_path) and os.path.getsize(mp3_path) > 128:
        return mp3_path, "gtts"
    try:
        # gTTS не поддерживает казахский язык ('kk'), поэтому используем 'ru' как аварийный фоллбэк
        gTTS(text=txt, lang="ru", slow=False).save(mp3_path)
        return mp3_path, "gtts"
    except Exception as e:
        logger.warning(f"[VOICE TTS] Ошибка gTTS: {e}")
    return None, None


_SENTENCE_END_RE = re.compile(r"(?<=[.!?…])\s+")
_tts_gen = 0                       # Номер текущей речевой сессии (новая сессия прерывает предыдущую)
_tts_synth_pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="tts-synth")


def _split_for_tts(text: str) -> list:
    """
    Делит текст на части для потоковой озвучки: первое предложение отдельно (быстрый старт звука),
    остальные склеиваются в блоки до ~180 символов (меньше пауз между фрагментами).
    """
    sentences = [s.strip() for s in _SENTENCE_END_RE.split(text) if s.strip()]
    if len(sentences) <= 1:
        return sentences
    chunks = [sentences[0]]
    buf = ""
    for s in sentences[1:]:
        if buf and len(buf) + len(s) + 1 > 180:
            chunks.append(buf)
            buf = s
        else:
            buf = f"{buf} {s}".strip()
    if buf:
        chunks.append(buf)
    return chunks


class SpeechSession:
    """
    Потоковая озвучка: фрагменты текста подаются через feed() по мере готовности
    (например, по предложениям из потока LLM). Синтез следующего фрагмента идёт
    параллельно с воспроизведением текущего, поэтому звук начинается почти сразу.
    """

    def __init__(self):
        global _tts_gen, _tts_proc, _tts_pending, _tts_mute_until
        with _tts_lock:
            _tts_gen += 1
            self.gen = _tts_gen
            if _tts_proc is not None and _tts_proc.poll() is None:
                try:
                    _tts_proc.terminate()
                except Exception:
                    pass
            _tts_pending = True
            _tts_mute_until = time.time() + 3.0
        self._queue = []
        self._cond = threading.Condition()
        self._closed = False
        self._fed_any = False
        threading.Thread(target=self._player, daemon=True).start()

    def feed(self, text: str):
        txt = _clean_for_tts(text or "")
        if not txt:
            return
        for chunk in _split_for_tts(txt):
            fut = _tts_synth_pool.submit(_synthesize_to_cache, chunk)
            with self._cond:
                self._queue.append((chunk, fut))
                self._fed_any = True
                self._cond.notify()

    def finish(self):
        with self._cond:
            self._closed = True
            self._cond.notify()

    def _is_current(self) -> bool:
        return self.gen == _tts_gen

    def _player(self):
        global _tts_proc, _tts_pending, _tts_mute_until
        while True:
            with self._cond:
                while not self._queue and not self._closed:
                    self._cond.wait(timeout=60)
                    if not self._queue and not self._closed:
                        self._closed = True  # защита от «зависшей» сессии
                if not self._queue:
                    break
                chunk, fut = self._queue.pop(0)

            try:
                mp3_path, engine = fut.result(timeout=20)
            except Exception as e:
                logger.warning(f"[VOICE TTS] Ошибка синтеза: {e}")
                mp3_path, engine = None, None

            with _tts_lock:
                if not self._is_current():
                    return
                try:
                    if mp3_path and os.path.exists(mp3_path):
                        # Edge TTS уже имеет естественный темп; gTTS слегка ускоряем
                        rate_args = ["-r", "1.10"] if engine == "gtts" else []
                        _tts_proc = subprocess.Popen(
                            ["/usr/bin/afplay", *rate_args, mp3_path],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    else:
                        say_voice = "Yuri" if get_voice_gender() == "male" else "Milena"
                        _tts_proc = subprocess.Popen(
                            ["/usr/bin/say", "-v", say_voice, "-r", "190", chunk],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    proc = _tts_proc
                except Exception as e:
                    logger.warning(f"[VOICE TTS] Ошибка запуска аудиоплеера: {e}")
                    proc = None

            if proc is not None:
                try:
                    proc.wait(timeout=60)
                except Exception:
                    pass

        with _tts_lock:
            if self._is_current():
                _tts_pending = False
                _tts_mute_until = time.time() + 1.1


def speak_on_mac(spoken_text: str):
    """
    Асинхронно озвучивает ответ живым нейросетевым голосом (Edge Neural TTS: Svetlana / Daulet)
    через динамики MacBook и блокирует приём эха с микрофона ESP32 на время синтеза, звучания + 1.1 сек.
    Длинный текст озвучивается по частям: первое предложение звучит, пока синтезируется остальное.
    """
    if not _clean_for_tts(spoken_text or ""):
        return
    session = SpeechSession()
    session.feed(spoken_text)
    session.finish()


def _prewarm_tts_cache():
    """Заранее синтезирует частые фразы, чтобы быстрые команды озвучивались мгновенно."""
    phrases = [
        "Включаю режим живых глаз", "Возвращаю основной интерфейс", "Открываю главный экран",
        "Открываю музыкальный плеер", "Открываю экран видео", "Включаю следующий трек",
        "Возвращаю предыдущий трек", "Ставлю на паузу", "Продолжаю воспроизведение", "Звук выключен",
        "Перехожу в сонный режим. Разбудите меня голосом, когда понадоблюсь.",
        "Время вышло! Таймер завершён.", "Таймер отменён.",
        "Сәлеметсіз бе! Мен дипломдық жобаның ақылды дауыстық көмекшісімін. Қандай көмек керек?",
        "Уақыт бітті! Таймер аяқталды.",
        "Иә, тыңдап тұрмын!", "Да, я слушаю!",
        "Жақсы, мен ұйықтадым. Шақырсаң, осындамын.",
    ]
    for p in phrases:
        for chunk in _split_for_tts(_clean_for_tts(p)):
            try:
                _synthesize_to_cache(chunk)
            except Exception:
                pass
    logger.info(f"[VOICE TTS] Кэш нейроголоса прогрет ({len(phrases)} фраз, Edge TTS: {'да' if _EDGE_TTS_OK else 'нет'})")


def normalize_pcm16(raw_pcm: bytes) -> bytes:
    """
    Убирает постоянное смещение (DC offset) и нормализует пиковую амплитуду
    16-битного моно PCM сигнала (8000/16000 Гц) для уверенного распознавания речи.
    """
    if not raw_pcm or len(raw_pcm) < 4:
        return raw_pcm

    if len(raw_pcm) % 2 != 0:
        raw_pcm = raw_pcm[:-1]

    samples = array.array("h")
    samples.frombytes(raw_pcm)
    n = len(samples)
    if n == 0:
        return raw_pcm

    mean_dc = int(sum(samples) / n)
    max_abs = 0
    for i in range(n):
        v = samples[i] - mean_dc
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        samples[i] = v
        av = -v if v < 0 else v
        if av > max_abs:
            max_abs = av

    if 150 < max_abs < 24000:
        gain = min(12.0, 24000.0 / float(max_abs))
        for i in range(n):
            v = int(samples[i] * gain)
            if v > 32767:
                v = 32767
            elif v < -32768:
                v = -32768
            samples[i] = v

    return samples.tobytes()


def recognize_pcm16(raw_pcm: bytes, sample_rate: int = 8000) -> str:
    """
    Распознаёт речь из сырого 16-битного моно PCM буфера (с поддержкой казахского и русского языков).
    Приоритет:
      1. Локальный нейросетевой STT (Kazakh-Russian Mixed STT, wav2vec2-CTC, офлайн, без задержек).
      2. Облачный резервный STT (Google Speech Recognition ru-RU / kk-KZ).
    Возвращает распознанную строку или пустую строку, если речь не разборчива.
    """
    norm_pcm = normalize_pcm16(raw_pcm)

    # 1. Приоритетный локальный билингвальный STT (офлайн, быстрый 300-500 мс)
    if get_stt_engine is not None:
        try:
            engine = get_stt_engine()
            if engine and engine.is_ready():
                t_stt0 = time.time()
                local_text = engine.transcribe_pcm16(norm_pcm, sample_rate=sample_rate)
                dt_stt = time.time() - t_stt0
                if local_text and len(local_text.strip()) > 1:
                    logger.info(f"[VOICE STT] Локальный Mixed STT ({dt_stt*1000:.1f} мс): «{local_text}»")
                    return local_text.strip()
        except Exception as e:
            logger.warning(f"[VOICE STT] Ошибка локального STT: {e}")

    # 2. Облачный резерв: Google Speech Recognition (ru-RU)
    audio_data = sr.AudioData(norm_pcm, sample_rate, 2)
    try:
        text = _recognizer.recognize_google(audio_data, language="ru-RU")
        if text and text.strip():
            logger.info(f"[VOICE STT] Облачный Google STT (ru-RU): «{text}»")
            return text.strip()
    except sr.UnknownValueError:
        pass
    except Exception as e:
        logger.warning(f"[VOICE STT] Ошибка сервиса распознавания (ru-RU): {e}")

    # 3. Облачный резерв: Google Speech Recognition (kk-KZ)
    try:
        text_kz = _recognizer.recognize_google(audio_data, language="kk-KZ")
        if text_kz and text_kz.strip():
            logger.info(f"[VOICE STT] Облачный Google STT (kk-KZ): «{text_kz}»")
            return text_kz.strip()
    except sr.UnknownValueError:
        return ""
    except Exception as e:
        logger.debug(f"[VOICE STT] Ошибка сервиса распознавания (kk-KZ): {e}")
        return ""


# ============================================================================
# Системные утилиты управления экосистемой MacBook (Батарея, CPU, Скриншот, Яркость)
# ============================================================================
def _get_mac_system_metrics(current_state: dict) -> tuple:
    """Возвращает (процент_батареи, заряжается_ли, нагрузка_cpu_проц)."""
    w_obj = current_state.get("weather") or {}
    batt_pct = w_obj.get("mac_battery")
    batt_chg = w_obj.get("mac_charging", False)
    if batt_pct is None:
        try:
            out = subprocess.check_output(["/usr/bin/pmset", "-g", "batt"], text=True, timeout=1.2)
            m = re.search(r"(\d+)%", out)
            batt_pct = int(m.group(1)) if m else 100
            batt_chg = ("AC Power" in out) or ("charging" in out.lower() and "discharging" not in out.lower())
        except Exception:
            batt_pct = 100
            batt_chg = False

    try:
        load1, _, _ = os.getloadavg()
        cpu_pct = min(99, max(3, int(round(load1 * 12.5))))
    except Exception:
        cpu_pct = 12
    return int(batt_pct), bool(batt_chg), int(cpu_pct)


def _take_mac_screenshot() -> bool:
    """Делает скриншот экрана MacBook и сохраняет на Рабочий стол."""
    try:
        desktop = os.path.expanduser("~/Desktop")
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        filepath = os.path.join(desktop, f"ESP32_Screenshot_{ts}.png")
        subprocess.Popen(["/usr/sbin/screencapture", "-x", filepath], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception as e:
        logger.warning(f"[MAC SYS] Ошибка скриншота: {e}")
        return False


def _adjust_mac_brightness(up: bool, steps: int = 4):
    """Изменяет яркость дисплея MacBook через системные коды клавиш."""
    key_code = 144 if up else 145
    script = f'tell application "System Events" to repeat {steps} times\nkey code {key_code}\nend repeat'
    try:
        subprocess.Popen(["/usr/bin/osascript", "-e", script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def _parse_timer_seconds(t: str) -> int:
    """Извлекает длительность таймера в секундах из голосовой фразы."""
    word_nums = {
        "одну": 1, "один": 1, "бір": 1, "бир": 1,
        "две": 2, "два": 2, "екі": 2, "еки": 2,
        "три": 3, "үш": 3, "уш": 3,
        "четыре": 4, "төрт": 4,
        "пять": 5, "бес": 5,
        "десять": 10, "он": 10,
        "пятнадцать": 15, "двадцать": 20, "тридцать": 30, "сорок": 40, "пятьдесят": 50
    }
    num_match = re.search(r"\b(\d{1,4})\b", t)
    val = int(num_match.group(1)) if num_match else None
    if val is None:
        for w, n in word_nums.items():
            if f" {w} " in f" {t} ":
                val = n
                break
    if val is None:
        if "минут" in t:
            val = 1
        elif "секунд" in t:
            val = 30
        else:
            val = 1

    if "секунд" in t or "сек" in t:
        return max(5, min(3600, val))
    return max(10, min(7200, val * 60))


# ============================================================================
# Серверный таймер: гарантированный звуковой сигнал на MacBook + событие на ESP32-S3
# ============================================================================
_timer_lock = threading.Lock()
_active_timer = None  # threading.Timer


def _format_timer_human(sec: int) -> str:
    mins, rem = sec // 60, sec % 60
    if mins and rem:
        return f"{mins} мин {rem} сек"
    if mins:
        return f"{mins} мин"
    return f"{rem} сек"


def _on_timer_finished(sec: int):
    global _active_timer
    with _timer_lock:
        _active_timer = None
    logger.info(f"[VOICE TIMER] Таймер на {sec} сек завершён — звуковой сигнал")

    # 1. Событие на ESP32-S3 (мелодия на MAX98357A + баннер на экране)
    if _event_broadcast_fn is not None:
        try:
            _event_broadcast_fn({"type": "timer_done", "seconds": sec})
        except Exception as e:
            logger.warning(f"[VOICE TIMER] Не удалось отправить событие на ESP32: {e}")

    # 2. Гарантированный звук на MacBook: системный сигнал x3 + голосовое уведомление
    def _alarm():
        for _ in range(3):
            try:
                subprocess.run(["/usr/bin/afplay", "/System/Library/Sounds/Glass.aiff"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=4)
            except Exception:
                pass
        speak_on_mac(f"Время вышло! Таймер на {_format_timer_human(sec)} завершён.")

    threading.Thread(target=_alarm, daemon=True).start()


def start_voice_timer(sec: int):
    global _active_timer
    with _timer_lock:
        if _active_timer is not None:
            _active_timer.cancel()
        _active_timer = threading.Timer(sec, _on_timer_finished, args=(sec,))
        _active_timer.daemon = True
        _active_timer.start()


def cancel_voice_timer() -> bool:
    global _active_timer
    with _timer_lock:
        if _active_timer is not None:
            _active_timer.cancel()
            _active_timer = None
            return True
    return False


# ============================================================================
# Запуск приложений macOS по голосу (русские и латинские названия от STT)
# ============================================================================
# (ключевые слова, имя приложения для `open -a`, человекочитаемое имя, веб-резерв)
_APP_ALIASES = [
    (["телеграм", "телеграмм", "telegram", "телега"], "Telegram", "Telegram", "https://web.telegram.org"),
    (["ватсап", "вотсап", "whatsapp", "whats app"], "WhatsApp", "WhatsApp", "https://web.whatsapp.com"),
    (["сафари", "safari"], "Safari", "Safari", None),
    (["хром", "chrome", "гугл хром"], "Google Chrome", "Google Chrome", None),
    (["заметки", "notes"], "Notes", "Заметки", None),
    (["калькулятор", "calculator"], "Calculator", "Калькулятор", None),
    (["терминал", "terminal"], "Terminal", "Терминал", None),
    (["vs code", "вс код", "visual studio", "вижуал студио", "редактор кода"], "Visual Studio Code", "VS Code", None),
    (["спотифай", "spotify"], "Spotify", "Spotify", "https://open.spotify.com"),
    (["яндекс музык", "yandex music"], "Yandex Music", "Яндекс Музыка", "https://music.yandex.kz"),
    (["настройки", "system settings", "параметры системы"], "System Settings", "Настройки", None),
    (["finder", "файндер", "проводник"], "Finder", "Finder", None),
    (["календарь", "calendar"], "Calendar", "Календарь", None),
    (["фото", "photos"], "Photos", "Фото", None),
    (["почту", "почта", "mail"], "Mail", "Почта", None),
    (["инстаграм", "instagram"], None, "Instagram", "https://www.instagram.com"),
    (["гитхаб", "github"], None, "GitHub", "https://github.com"),
    (["чатгпт", "chatgpt", "чат гпт"], None, "ChatGPT", "https://chatgpt.com"),
]
_APP_OPEN_VERBS = ("открой", "запусти", "включи", "открыть", "запустить", "аш", "іске қос", "қос")


def _find_app_alias(t: str):
    for keys, app, human, url in _APP_ALIASES:
        if any(k in t for k in keys):
            return app, human, url
    return None


def open_mac_app(app_name, human_name, web_url) -> tuple:
    """Открывает приложение macOS (или его веб-версию). Возвращает (успех, описание)."""
    if app_name:
        try:
            r = subprocess.run(["/usr/bin/open", "-a", app_name],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=4)
            if r.returncode == 0:
                return True, f"Открываю {human_name}."
        except Exception:
            pass
    if web_url:
        subprocess.Popen(["/usr/bin/open", web_url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True, f"Приложение не найдено, открываю веб-версию {human_name}."
    return False, f"Приложение {human_name} не установлено на этом MacBook."


# ============================================================================
# YouTube: поиск и автозапуск первого видео по голосовому запросу
# ============================================================================
_YT_WORD_RE = r"(?:ютуб\w*|ютюб\w*|youtube\w*|you\s*tube|ютубчик\w*|\w*тубтан|\w*тубқа|\w*тубка)"
_YT_FILLER_RE = (
    r"\b(?:найди|найти|включи|включить|открой|открыть|поищи|запусти|покажи|поставь|хочу|посмотреть|"
    r"мне|пожалуйста|плиз|давай|можешь|можно|видео\s+про|видос|тап|қос|аш|көрсет|ашып|қосып|тауып|берші|бершы|"
    r"маған|маган|бір|бир|любой|любое|какое-нибудь|видео|видеосы|бейне|"
    r"на|в|во|с|из|тан|тен|дан|ден)\b"
)


def _extract_youtube_query(t: str):
    """Возвращает (query, autoplay) если фраза — запрос на YouTube, иначе None."""
    if not re.search(_YT_WORD_RE, t):
        return None
    if not re.search(r"\b(найди|найти|включи|включить|открой|открыть|поищи|запусти|покажи|поставь|хочу|қос|тап|аш|көрсет|ашып|қосып|тауып|ашшы|қосшы|көрсетші)\b", t):
        return None
    autoplay = bool(re.search(r"\b(включи|включить|запусти|поставь|хочу посмотреть|қос|қосып|қосшы)\b", t))
    q = re.sub(_YT_WORD_RE, " ", t)
    q = re.sub(_YT_FILLER_RE, " ", q)
    q = re.sub(r"[^\w\s\-]", " ", q)
    q = re.sub(r"\s+", " ", q).strip()
    return q, autoplay


def _youtube_first_video_url(query: str):
    """Находит первое видео в выдаче YouTube (без API-ключа). Возвращает URL или None."""
    try:
        url = "https://www.youtube.com/results?search_query=" + urllib.parse.quote(query)
        req = urllib.request.Request(url, headers={
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
            "Accept-Language": "ru-RU,ru;q=0.9",
        })
        page = urllib.request.urlopen(req, timeout=4).read().decode("utf-8", errors="ignore")
        m = re.search(r'"videoRenderer":\{"videoId":"([\w-]{11})"', page) or re.search(r'"videoId":"([\w-]{11})"', page)
        if m:
            return f"https://www.youtube.com/watch?v={m.group(1)}"
    except Exception as e:
        logger.debug(f"[YOUTUBE] Ошибка поиска первого видео: {e}")
    return None


def open_youtube_query(query: str, autoplay: bool) -> str:
    """Открывает YouTube: первое видео (autoplay) или страницу поиска. Возвращает фразу для озвучки."""
    if not query:
        subprocess.Popen(["/usr/bin/open", "https://www.youtube.com"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return "Открываю Ютуб."
    if autoplay:
        video_url = _youtube_first_video_url(query)
        if video_url:
            subprocess.Popen(["/usr/bin/open", video_url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return f"Включаю на Ютубе: {query}."
    search_url = "https://www.youtube.com/results?search_query=" + urllib.parse.quote(query)
    subprocess.Popen(["/usr/bin/open", search_url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return f"Нашла на Ютубе: {query}. Выбирайте видео."


# ============================================================================
# Живой поиск в интернете (DuckDuckGo + Wikipedia RU) для актуальных знаний LLM
# ============================================================================
def _fetch_ddg_snippets(query: str) -> list:
    out = []
    try:
        url = "https://html.duckduckgo.com/html/?q=" + urllib.parse.quote(query)
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": (
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"
                ),
                "Accept-Language": "ru-RU,ru;q=0.9,en;q=0.8",
            },
        )
        raw_html = urllib.request.urlopen(req, timeout=WEB_SEARCH_TIMEOUT).read().decode("utf-8", errors="ignore")
        snippets = re.findall(r'class="result__snippet[^>]*>(.*?)</a>', raw_html, flags=re.S)
        titles = re.findall(r'class="result__a[^>]*>(.*?)</a>', raw_html, flags=re.S)
        for t, s in zip(titles[:3], snippets[:3]):
            t_clean = html.unescape(re.sub(r"<[^>]+>", "", t)).strip()
            s_clean = html.unescape(re.sub(r"<[^>]+>", "", s)).strip()
            if len(s_clean) > 190:
                s_clean = s_clean[:187] + "..."
            if s_clean:
                out.append(f"• {t_clean}: {s_clean}")
    except Exception as e:
        logger.debug(f"[WEB SEARCH] Ошибка DDG: {e}")
    return out


def _fetch_wiki_snippets(query: str) -> list:
    out = []
    try:
        w_url = (
            "https://ru.wikipedia.org/w/api.php?action=query&list=search&srsearch="
            + urllib.parse.quote(query)
            + "&utf8=1&format=json&srlimit=2"
        )
        req = urllib.request.Request(w_url, headers={"User-Agent": "ESP32VoiceAssistant/2.4"})
        w_data = json.loads(urllib.request.urlopen(req, timeout=WEB_SEARCH_TIMEOUT).read().decode("utf-8"))
        for item in w_data.get("query", {}).get("search", [])[:2]:
            title = item.get("title", "")
            snip = html.unescape(re.sub(r"<[^>]+>", "", item.get("snippet", ""))).strip()
            if title and snip:
                out.append(f"• Википедия ({title}): {snip}")
    except Exception as e:
        logger.debug(f"[WEB SEARCH] Ошибка Wiki: {e}")
    return out


def _needs_web_search(user_text: str) -> bool:
    """Определяет, требует ли запрос пользователя поиска фактов/новостей в интернете."""
    t = user_text.lower().strip()
    # Короткие бытовые реплики и команды управления не требуют поиска в сети
    trivial = {
        "привет", "здравствуй", "салем", "сәлем", "как дела", "спасибо", "рахмет",
        "молодец", "пока", "до свидания", "кто ты", "как тебя зовут",
        "как называется твоя модель", "какая у тебя модель", "проснись", "усни"
    }
    if t in trivial or len(t) < 6:
        return False

    # Творческие просьбы и болтовня не требуют фактов из сети (экономия ~1.5-2 сек)
    chit_chat = [
        "анекдот", "шутк", "пошути", "придумай", "сочини", "стих", "сказк", "как дела", "как ты",
        "кто ты", "что умеешь", "спасибо", "молодец", "әзіл", "ертегі", "өлең", "қалайсың", "калайсын",
        "сен кімсің", "не істей аласың", "рахмет", "рақмет", "жарайсың", "уақыт", "сағат қанша",
        "который час", "сколько времени", "какое сегодня число", "какой сегодня день",
    ]
    if any(k in t for k in chit_chat):
        return False

    # Базовые знания, счет, перевод, повторение и школьные факты НЕ требуют поиска в сети (поиск лишь путает модель)
    no_search_stems = [
        "повтори", "қайтала", "кайтала", "столиц", "астана", "алфавит", "әліпби", "алипби",
        "әріп", "арип", "букв", "неше әріп", "қанша әріп", "сколько букв", "переведи", "аудар",
        "қалай болады", "калай болады", "сколько будет", "неше болады", "умножь", "раздели",
        "плюс", "минус", "посчитай", "есепте", "дауыс", "голос", "ер адам", "әйел"
    ]
    if any(k in t for k in no_search_stems):
        return False

    # Команды управления воспроизведением, громкостью, приложениями, таймером и системой не ищут в интернете
    control_stems = [
        "пауз", "стоп", "останов", "включ", "продолж", "играй", "перемот", "следующ",
        "предыдущ", "громк", "звук", "тише", "таймер", "скриншот", "яркост", "открой",
        "запусти", "ютуб", "youtube", "телеграм", "музык", "трек", "песн", "қос", "өшір",
        "тоқтат", "дауыс", "аш"
    ]
    if any(stem in t for stem in control_stems):
        # Только если явно задан информационный вопрос (кто такой, почему, что такое)
        if not any(q in t for q in ["кто такой", "кто такая", "что такое", "расскажи про", "расскажи о", "почему", "зачем", "когда был", "когда родил", "в каком году"]):
            return False

    # Поиск нужен ТОЛЬКО если есть маркеры актуальных событий, свежих фактов или явный вопрос
    active_search_markers = [
        "новост", "жаңалық", "жаналык", "матч", "футбол", "чемпионат", "счет", "счёт",
        "кто выиграл", "кто победил", "жеңді", "жылғы", "жыл", "2024", "2025", "2026",
        "ертеңгі", "завтра", "прогноз", "акции", "бағасы", "бағам", "курс", "президент",
        "премьер", "министр", "қашан", "когда", "где находится", "қайда", "не болды", "что произошло"
    ]
    return any(m in t for m in active_search_markers)


WEB_SEARCH_TIMEOUT = 1.8
_web_pool = ThreadPoolExecutor(max_workers=4, thread_name_prefix="web-search")


def search_web_context(query: str) -> str:
    """
    Выполняет параллельный поиск в DuckDuckGo и русской Википедии (с кэшированием на 10 минут).
    Возвращает краткую сводку фактов из интернета для промпта локальной модели.
    """
    q_norm = query.strip().lower()
    now = time.time()
    if q_norm in _web_cache:
        ts, cached_val = _web_cache[q_norm]
        if now - ts < 600:
            return cached_val

    # Общий бюджет поиска ~2 сек: не ждём медленный источник (пул не блокирует выход через with)
    results = []
    f_ddg = _web_pool.submit(_fetch_ddg_snippets, query)
    f_wiki = _web_pool.submit(_fetch_wiki_snippets, query)
    deadline = now + WEB_SEARCH_TIMEOUT + 0.2
    for fut in (f_ddg, f_wiki):
        try:
            results.extend(fut.result(timeout=max(0.05, deadline - time.time())))
        except Exception:
            pass

    summary = "\n".join(results[:4]).strip()
    if summary:
        _web_cache[q_norm] = (now, summary)
        logger.info(f"[WEB SEARCH] Найдено {len(results[:4])} результатов по запросу '{query}'")
    return summary


def _try_fast_command(text: str, current_state: dict, set_volume_fn, media_cmd_fn):
    """
    Быстрый детерминированный обработчик для коротких аппаратных команд (экран, плеер, громкость),
    системных действий macOS (таймер, скриншот, яркость, заряд батареи, YouTube-поиск),
    живой погоды, курса валют и двуязычных команд (русский + казахский).
    Возвращает готовый словарь ответа за < 5 мс, либо None, если нужен интеллект локальной LLM.
    """
    t = text.lower().strip()
    w_obj = current_state.get("weather") or {}
    temp_c = int(w_obj.get("temp", 18))
    desc_kz = w_obj.get("desc_kz", "Ашық")
    desc_ru = w_obj.get("desc_ru", "ясно")
    usd_kzt = int(w_obj.get("usd_kzt", 495))
    temp_sign = f"+{temp_c}" if temp_c > 0 else str(temp_c)

    # 1. Мгновенный ответ о текущем треке / песне / исполнителе (RU + KZ)
    if any(k in t for k in [
        "какой трек", "какой сейчас трек", "какая песня", "какая сейчас песня",
        "что сейчас играет", "что играет", "кто поет", "кто поёт", "кто исполняет",
        "название песни", "название трека", "какая музыка играет",
        "қандай ән", "қандай трек", "не ойнап тұр", "кім айтып",
        "кандай ан", "кандай трек", "не ойнап тур"
    ]):
        is_kz = any(k in t for k in ["қандай", "кандай", "не ойнап", "кім айтып"])
        title = (current_state.get("title") or "").strip()
        artist = (current_state.get("artist") or "").strip()
        source = (current_state.get("source") or "macOS").strip()
        is_playing = (current_state.get("state") == "playing")
        if title and title != "Ойнатылмай тұр":
            reply = f"{title} — {artist}" if artist else f"Трек: {title}"
            if is_kz:
                st_kz = "Қазір ойнап тұрған ән" if is_playing else "Паузада тұрған ән"
                spoken = f"{st_kz}: {title}, орындаушы {artist}." if artist else f"{st_kz}: {title}."
            else:
                status_word = "Сейчас играет" if is_playing else "Сейчас на паузе"
                spoken = f"{status_word} трек {title}, исполнитель {artist}, в {source}." if artist else f"{status_word} {title}."
            target_emotion = "happy"
        else:
            if is_kz:
                reply = "Қазір музыка қосылмаған"
                spoken = "Қазір ешқандай музыка ойнап тұрған жоқ."
            else:
                reply = "Сейчас ничего не играет"
                spoken = "На данный момент музыка не воспроизводится."
            target_emotion = "speaking"

        logger.info(f"[VOICE FAST] Фраза: '{text}' | Экран: '{reply}' | Голос: '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": target_emotion,
        }

    # 2. Мгновенный ответ о живой погоде в Алматы (RU + KZ) — только для текущей погоды
    if any(k in t for k in [
        "какая погода", "какая сейчас погода", "погода в алматы", "сколько градусов",
        "как там погода", "ауа райы", "неше градус", "далада ауа райы"
    ]) and not any(k in t for k in ["ертең", "ертен", "завтра", "келесі", "келеси", "апта", "прогноз", "болады"]):
        is_kz = any(k in t for k in ["ауа райы", "неше градус", "далада"])
        reply = f"Алматы: {temp_sign}°C • {desc_kz}"
        if is_kz:
            spoken = f"Қазір Алматыда ауа температурасы {temp_sign} градус, {desc_kz.lower()}."
        else:
            spoken = f"Сейчас в Алматы {temp_sign} градусов, на улице {desc_ru}."
        logger.info(f"[VOICE FAST WEATHER] '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "home",
            "emotion": "happy",
        }

    # 3. Мгновенный ответ о курсе доллара USD/KZT (RU + KZ)
    if any(k in t for k in [
        "курс доллара", "какой курс", "сколько стоит доллар", "почем доллар",
        "почём доллар", "курс валют", "курс тенге", "доллар бағамы", "доллар багамы",
        "доллар қанша", "доллар канша"
    ]):
        is_kz = any(k in t for k in ["бағамы", "багамы", "қанша", "канша"])
        reply = f"Курс: 1 USD = {usd_kzt} KZT"
        if is_kz:
            spoken = f"Бүгінгі бағам бойынша бір АҚШ доллары {usd_kzt} теңге тұрады."
        else:
            spoken = f"Актуальный курс доллара сейчас составляет {usd_kzt} тенге за один доллар."
        logger.info(f"[VOICE FAST CURRENCY] '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "home",
            "emotion": "happy",
        }

    # 3a. Мгновенный ответ о времени и дате (RU + KZ) — без веб-поиска и LLM
    is_time_q = any(k in t for k in [
        "который час", "сколько времени", "сколько сейчас времени", "текущее время",
        "уақыт қанша", "уакыт канша", "сағат қанша", "сагат канша", "қазір сағат", "қазіргі уақыт", "казирги уакыт",
    ])
    is_date_q = any(k in t for k in [
        "какое сегодня число", "какой сегодня день", "какая сегодня дата", "сегодняшняя дата",
        "бүгін нешесі", "бугин нешеси", "бүгін қай күн", "бүгін қандай күн",
    ])
    if is_time_q or is_date_q:
        now_dt = datetime.datetime.now()
        is_kz = _is_kazakh_text(t) or any(k in t for k in ["уакыт", "сагат", "бугин", "казирги"])
        months_ru = ["января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                     "сентября", "октября", "ноября", "декабря"]
        months_kz = ["қаңтар", "ақпан", "наурыз", "сәуір", "мамыр", "маусым", "шілде", "тамыз",
                     "қыркүйек", "қазан", "қараша", "желтоқсан"]
        days_ru = ["понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье"]
        days_kz = ["дүйсенбі", "сейсенбі", "сәрсенбі", "бейсенбі", "жұма", "сенбі", "жексенбі"]
        hhmm = now_dt.strftime("%H:%M")
        if is_time_q:
            reply = f"Уақыт: {hhmm}"
            spoken = (f"Қазір сағат {now_dt.hour} {now_dt.minute:02d}." if is_kz
                      else f"Сейчас {now_dt.hour} часов {now_dt.minute:02d} минут.")
        else:
            reply = f"{now_dt.day:02d}.{now_dt.month:02d} • {days_kz[now_dt.weekday()] if is_kz else days_ru[now_dt.weekday()]}"
            spoken = (f"Бүгін {now_dt.day} {months_kz[now_dt.month - 1]}, {days_kz[now_dt.weekday()]}." if is_kz
                      else f"Сегодня {now_dt.day} {months_ru[now_dt.month - 1]}, {days_ru[now_dt.weekday()]}.")
        logger.info(f"[VOICE FAST TIME] '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    # 3b. Переключение профиля голоса: Мужской / Женский (RU + KZ)
    if any(k in t for k in [
        "женский голос", "включи женский голос", "сделай женский голос", "поставь женский голос",
        "переключи на женский голос", "әйел дауысын қос", "әйел дауысы", "қыздың дауысы", "кыздын дауысы",
        "қыз дауысы", "кыз дауысы", "әйел дауысын қой"
    ]):
        set_voice_gender("female")
        is_kz = _is_kazakh_text(t) or any(k in t for k in ["әйел", "айел", "қыз", "кыз", "дауыс"])
        reply = "Әйел дауысы қосылды" if is_kz else "Включён женский голос"
        spoken = ("Әйел дауысына ауыстым. Менің жаңа даусым қалай?" if is_kz
                  else "Включаю женский голос. Теперь я говорю так. Как вам моё звучание?")
        logger.info(f"[VOICE FAST] Переключение на женский голос: '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "eyes",
            "emotion": "happy",
        }

    if any(k in t for k in [
        "мужской голос", "включи мужской голос", "сделай мужской голос", "поставь мужской голос",
        "переключи на мужской голос", "ер адам дауысын қос", "ер дауысы", "еркек дауысы", "ер адам дауысы",
        "ер адамның дауысы", "ер дауысын қой"
    ]):
        set_voice_gender("male")
        is_kz = _is_kazakh_text(t) or any(k in t for k in ["ер", "адам", "еркек", "дауыс"])
        reply = "Ер адам дауысы қосылды" if is_kz else "Включён мужской голос"
        spoken = ("Ер адам дауысына ауыстым. Тыңдап тұрмын!" if is_kz
                  else "Включаю мужской голос. Теперь я звучу так. Чем могу помочь?")
        logger.info(f"[VOICE FAST] Переключение на мужской голос: '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "eyes",
            "emotion": "happy",
        }

    if any(k in t for k in [
        "какой у тебя голос", "какой голос включен", "чей голос", "даусың қандай", "кандай дауыс", "қандай дауыс"
    ]):
        g = get_voice_gender()
        is_kz = _is_kazakh_text(t) or any(k in t for k in ["дауыс", "қандай", "кандай"])
        p = VOICE_PROFILES.get(g, VOICE_PROFILES["male"])
        if is_kz:
            reply = f"Дауыс: {p['name_kz']}"
            spoken = f"Қазір {p['name_kz']} қосылып тұр. Қаласаңыз, әйел немесе ер адам дауысын таңдай аласыз."
        else:
            reply = f"Голос: {p['name_ru']}"
            spoken = f"Сейчас включён {p['name_ru']}. Вы можете сказать «включи женский голос» или «включи мужской голос»."
        logger.info(f"[VOICE FAST] Запрос голоса: '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "eyes",
            "emotion": "speaking",
        }

    # 4. Мониторинг ноутбука MacBook: заряд батареи и нагрузка CPU (RU + KZ)
    if any(k in t for k in [
        "заряд батареи", "сколько процентов заряда", "сколько заряда", "какой заряд",
        "состояние ноутбука", "нагрузка на процессор", "состояние макбука",
        "заряд қанша", "заряд канша", "батарея қанша", "ноутбук заряды"
    ]):
        batt_pct, batt_chg, cpu_pct = _get_mac_system_metrics(current_state)
        chg_str = "Зарядка" if batt_chg else "Батарея"
        reply = f"Mac: {batt_pct}% ({chg_str}) • CPU {cpu_pct}%"
        chg_spoken = "подключён к зарядке" if batt_chg else "работает от аккумулятора"
        spoken = f"Заряд MacBook составляет {batt_pct} процентов, ноутбук {chg_spoken}. Нагрузка на процессор {cpu_pct} процентов."
        logger.info(f"[VOICE FAST MAC SYS] '{text}' -> '{spoken}'")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    # 5. Голосовой таймер обратного отсчёта на экране ESP32-S3 (RU + KZ)
    if any(k in t for k in ["отмени таймер", "сбрось таймер", "выключи таймер", "останови таймер", "таймерді өшір"]):
        cancel_voice_timer()
        reply = "Таймер тоқтатылды • Сброшен"
        spoken = "Таймер отменён."
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "speaking",
            "timer_sec": -1,
        }

    if "таймер" in t and any(k in t for k in ["постав", "засеки", "включи", "запусти", "заведи", "установи", "таймер на", "қой"]):
        sec = _parse_timer_seconds(t)
        start_voice_timer(sec)
        reply = f"Таймер: {_format_timer_human(sec)} • Іске қосылды"
        spoken = f"Запускаю таймер на {_format_timer_human(sec).replace('мин', 'минут').replace('сек', 'секунд')}. По окончании подам сигнал."
        logger.info(f"[VOICE TIMER] Запущен таймер на {sec} сек")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
            "timer_sec": sec,
        }

    # 6. Умный поиск и автозапуск видео на YouTube голосом
    yt_req = _extract_youtube_query(t)
    if yt_req is not None:
        q_yt, autoplay = yt_req
        spoken = open_youtube_query(q_yt, autoplay)
        reply = f"YouTube: {q_yt[:24]}" if q_yt else "YouTube ашылды"
        logger.info(f"[VOICE YOUTUBE] '{text}' -> query='{q_yt}' autoplay={autoplay}")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "video",
            "emotion": "happy",
        }

    # 6a. Запуск приложений macOS (Telegram, Safari, VS Code и т.д.)
    if any(re.search(rf"\b{re.escape(v)}", t) for v in _APP_OPEN_VERBS):
        app_hit = _find_app_alias(t)
        if app_hit is not None:
            app_name, human_name, web_url = app_hit
            ok, spoken = open_mac_app(app_name, human_name, web_url)
            reply = f"Ашылды: {human_name}" if ok else f"{human_name} табылмады"
            logger.info(f"[VOICE APP] '{text}' -> {human_name} ({'OK' if ok else 'не найдено'})")
            speak_on_mac(spoken)
            return {
                "type": "voice_result",
                "status": "ok",
                "heard": text,
                "reply": reply,
                "screen": "",
                "emotion": "happy" if ok else "confused",
            }

    # 7. Скриншот экрана, яркость дисплея и запуск приложений macOS
    if any(k in t for k in ["сделай скриншот", "снимок экрана", "скриншот экрана", "скриншот жаса"]):
        _take_mac_screenshot()
        reply = "Скриншот сақталды • Готово!"
        spoken = "Скриншот экрана сделан и сохранён на рабочий стол MacBook."
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    if any(k in t for k in ["прибавь яркость", "сделай ярче", "яркость выше", "увеличь яркость", "экран ярче"]):
        _adjust_mac_brightness(up=True, steps=4)
        reply = "Жарықтық + • Экран ярче"
        spoken = "Прибавляю яркость экрана ноутбука."
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    if any(k in t for k in ["убавь яркость", "сделай темнее", "яркость ниже", "уменьши яркость", "экран темнее"]):
        _adjust_mac_brightness(up=False, steps=4)
        reply = "Жарықтық - • Экран темнее"
        spoken = "Убавляю яркость экрана ноутбука."
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "speaking",
        }

    # 7a. Мгновенная перемотка трека / видео (SEEK_FORWARD / SEEK_BACKWARD)
    seek_match = re.search(r"перемот(?:ай|ать)?\s+(?:на\s+)?(\d{1,3})\s*сек[^\s]*\s*(?:(впер[её]д|назад|алға|артқа))?", t)
    if not seek_match:
        seek_match = re.search(r"(\d{1,3})\s*сек[^\s]*\s*(впер[её]д|назад|алға|артқа)", t)
    if seek_match:
        sec_val = int(seek_match.group(1))
        if "назад" in t or "артқа" in t:
            media_cmd_fn("SEEK_BACKWARD", sec_val)
            reply = f"<< -{sec_val}с • Назад"
            spoken = f"Перемотал на {sec_val} секунд назад."
        else:
            media_cmd_fn("SEEK_FORWARD", sec_val)
            reply = f"+{sec_val}с • Вперёд >>"
            spoken = f"Перемотал на {sec_val} секунд вперёд."
        logger.info(f"[VOICE FAST SEEK] '{text}' -> {sec_val}s")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "speaking",
        }

    # 7b. Мгновенный Play / Pause
    if any(k in t for k in ["поставь на паузу", "на паузу", "музыку на паузу", "песню на паузу", "останови музыку", "выключи музыку", "пауза", "стоп", "тоқтат", "токтат", "пауза қой", "музыканы өшір"]):
        media_cmd_fn("PLAY_PAUSE")
        reply = "Пауза қойылды • На паузе"
        spoken = "Ставлю на паузу"
        logger.info(f"[VOICE FAST] '{text}' -> PLAY_PAUSE")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "speaking",
        }

    if any(k in t for k in ["включи музыку", "запусти музыку", "включи песню", "продолжи музыку", "сними с паузы", "играй", "плей", "воспроизведение", "музыканы қос", "музыканы кос", "жалғастыр", "ойнат"]):
        media_cmd_fn("PLAY_PAUSE")
        reply = "Ойнату қосылды • Играет"
        spoken = "Продолжаю воспроизведение"
        logger.info(f"[VOICE FAST] '{text}' -> PLAY_PAUSE")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    # 8. Быстрое двуязычное приветствие на казахском языке (Билингвальность)
    if t in ("сәлем", "салем", "ассалаумағалейкум", "ассалам алейкум", "қалың қалай", "калын калай", "қалайсың", "калайсын"):
        reply = "Сәлем! Жүйе дайын • Готов!"
        spoken = "Сәлеметсіз бе! Мен дипломдық жобаның ақылды дауыстық көмекшісімін. Қандай көмек керек?"
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": text,
            "reply": reply,
            "screen": "",
            "emotion": "happy",
        }

    words = t.split()
    if len(words) > 4 or "?" in t or any(w in t for w in ["расскажи", "почему", "зачем", "объясни", "посоветуй", "придумай", "переведи", "сколько будет", "кто выиграл", "кто стал", "чемпион"]):
        return None

    num_match = re.search(r"\b(\d{1,3})\b", t)
    num_val = int(num_match.group(1)) if num_match else None

    reply = ""
    spoken = ""
    target_screen = ""
    target_emotion = "speaking"

    if any(k in t for k in ["живые глаза", "покажи глаза", "открой глаза", "режим глаз", "глаза робота", "тірі көздер", "көзіңді аш", "козинди аш", "проснись", "оян"]):
        target_screen = "eyes"
        target_emotion = "happy"
        reply = "Тірі көздер • Живые глаза!"
        spoken = "Включаю режим живых глаз"
    elif any(k in t for k in ["усни", "режим сна", "спокойной ночи", "ложись спать", "ұйықта", "уйыкта", "демал"]):
        target_screen = "sleep_eyes"
        target_emotion = "sleepy"
        reply = "Ұйқы режимі • Zzz..."
        spoken = "Перехожу в сонный режим. Разбудите меня голосом, когда понадоблюсь."
    elif any(k in t for k in ["скрой глаза", "закрой глаза", "верни экран", "обычный режим", "көзіңді жап"]):
        target_screen = "hide_eyes"
        reply = "Возвращаю основной экран"
        spoken = "Возвращаю основной интерфейс"
    elif t in ("главный экран", "домой", "на главную", "открой главный экран", "басты экран", "үйге"):
        target_screen = "home"
        reply = "Главный экран открыт"
        spoken = "Открываю главный экран"
    elif t in ("экран музыки", "открой плеер", "музыкальный плеер", "эквалайзер", "музыка экраны"):
        target_screen = "music"
        reply = "Музыкальный плеер открыт"
        spoken = "Открываю музыкальный плеер"
    elif t in ("экран видео", "открой видео", "видеоплеер", "видео экраны"):
        target_screen = "video"
        reply = "Экран видео открыт"
        spoken = "Открываю экран видео"
    elif t in ("следующий трек", "следующая песня", "дальше", "скип", "переключи трек", "следующий", "келесі ән", "келесі трек", "келесы ан", "келесы трек", "алға"):
        media_cmd_fn("NEXT_TRACK")
        reply = "Келесі трек • Следующий >>"
        spoken = "Включаю следующий трек"
    elif t in ("предыдущий трек", "предыдущая песня", "прошлый трек", "верни трек", "алдыңғы ән", "алдынгы ан", "артқа"):
        media_cmd_fn("PREVIOUS_TRACK")
        reply = "<< Алдыңғы • Предыдущий"
        spoken = "Возвращаю предыдущий трек"
    elif t in ("пауза", "стоп", "останови", "выключи музыку", "поставь на паузу", "тоқтат", "токтат", "пауза қой", "музыканы өшір"):
        media_cmd_fn("PLAY_PAUSE")
        reply = "Пауза қойылды • На паузе"
        spoken = "Ставлю на паузу"
    elif t in ("включи музыку", "продолжи", "играй", "плей", "воспроизведение", "запусти музыку", "музыканы қос", "музыканы кос", "жалғастыр", "ойнат"):
        media_cmd_fn("PLAY_PAUSE")
        reply = "Ойнату қосылды • Играет"
        spoken = "Продолжаю воспроизведение"
    elif any(k in t for k in ["выключи звук", "без звука", "мут", "дыбысты өшір"]):
        _, cur_vol = set_volume_fn(0)
        reply = f"Звук выключен ({cur_vol}%)"
        spoken = "Звук выключен"
    elif any(k in t for k in ["громк", "звук", "дауыс"]) and num_val is not None and 0 <= num_val <= 100:
        _, cur_vol = set_volume_fn(num_val)
        reply = f"Громкость: {cur_vol}%"
        spoken = f"Ставлю громкость на {cur_vol} процентов"
    elif t in ("громче", "сделай громче", "прибавь звук", "прибавь громкость", "қаттырақ", "каттырак", "дауысын қаттырақ", "дауысын көтер"):
        cur = int(current_state.get("volume", 50))
        _, cur_vol = set_volume_fn(min(100, cur + 15))
        reply = f"Громче: {cur_vol}%"
        spoken = f"Делаю громче, {cur_vol} процентов"
    elif t in ("тише", "сделай тише", "убавь звук", "убавь громкость", "потише", "ақырын", "акырын", "дауысын ақырын", "дауысын азайт"):
        cur = int(current_state.get("volume", 50))
        _, cur_vol = set_volume_fn(max(0, cur - 15))
        reply = f"Тише: {cur_vol}%"
        spoken = f"Делаю тише, {cur_vol} процентов"
    else:
        return None

    logger.info(f"[VOICE FAST] Фраза: '{text}' | Экран: '{reply}' | Голос: '{spoken}'")
    speak_on_mac(spoken)
    return {
        "type": "voice_result",
        "status": "ok",
        "heard": text,
        "reply": reply,
        "screen": target_screen,
        "emotion": target_emotion,
    }


def _safe_parse_llm_json(content: str) -> dict:
    """Безопасно разбирает JSON от LLM, в том числе если внутри строки встретились неэкранированные кавычки."""
    try:
        return json.loads(content)
    except Exception:
        result = {"action": "none", "value": 0, "spoken": "", "reply": "", "emotion": "speaking"}
        m_act = re.search(r'"action"\s*:\s*"([^"]+)"', content)
        if m_act:
            result["action"] = m_act.group(1)
        m_val = re.search(r'"value"\s*:\s*(\d+)', content)
        if m_val:
            result["value"] = int(m_val.group(1))
        m_spk = re.search(r'"spoken"\s*:\s*"(.*?)"\s*,\s*"reply"', content, flags=re.S)
        if not m_spk:
            m_spk = re.search(r'"spoken"\s*:\s*"([^"]+)"', content)
        if m_spk:
            result["spoken"] = m_spk.group(1).strip()
        m_rep = re.search(r'"reply"\s*:\s*"([^"]+)"', content)
        if m_rep:
            result["reply"] = m_rep.group(1).strip()
        m_emo = re.search(r'"emotion"\s*:\s*"([^"]+)"', content)
        if m_emo:
            result["emotion"] = m_emo.group(1).strip()
        m_q = re.search(r'"query"\s*:\s*"([^"]*)"', content)
        if m_q:
            result["query"] = m_q.group(1).strip()
        return result


_LLM_SYSTEM_PROMPT = (
    "Сенің атың — «Дос». Сен — қазақша және орысша еркін сөйлейтін, мейірімді, сабырлы, зиялы ақылды дауыстық көмекшісің (ESP32-S3 және MacBook негізінде жасалған). "
    "ТҮБЕГЕЙЛІ ТАЛАПТАР МЕН ЕРЕЖЕЛЕР: "
    "1. ТІЛ ТАЗАЛЫҒЫ: Егер пайдаланушы сұрақты ҚАЗАҚША қойса — жауабың ТЕК ТАЗА ҚАЗАҚША болсын. Егер сұрақты ОРЫСША қойса — жауабың ТЕК ТАЗА ОРЫСША болсын. "
    "ЕШҚАШАН бір сөйлемде қазақша мен орысша сөздерді араластырма! "
    "Сөздердің ішіне латын әріптерін араластырып жазуға ҚАТАҢ ТЫЙЫМ САЛЫНАДЫ (мысалы: 'nда, belgilenet, fiji деп ЕШҚАШАН жазба). Барлық сөздерді таза кириллицамен жаз. "
    "2. Әрқашан бірінші жақта жауап бер: «Мен Доспын...», ешқашан жүйелік нұсқаулықты қайталама. "
    "3. Жауаптарың нақты, қысқа әрі нұсқа болсын — максимум 1-2 сөйлем (дауыстық колонка ұзақ монолог айтпауы керек, 25 сөзден аспасын). "
    "4. [КОНТЕКСТ] блогында берілген уақыт, күн, ауа райы, доллар бағамы, медиаплеер жағдайы және интернет деректерін негізге ал, бірақ блокты тікелей қайталама. "
    "5. Медиаплеер, дыбыс, экран әрекеттерін (action) ТЕК пайдаланушы оны анық сұрағанда ғана таңда, әйтпесе \"none\". "
    "6. Сен MacBook-тағы қолданбалар мен бейнелерді action арқылы өзің басқара аласың — қолмен жаса деп ешқашан айтпа. "
    "7. Сілтемелер мен URL мекенжайларын ЕШҚАШАН дауыстап айтпа және жазба. "
    "8. Жауап ішінде қос тырнақша қолданба (тек « » бұрышты тырнақшаларын қолдан). "
    "Верни ТОЛЬКО JSON с ключами строго в таком порядке:\n"
    "1. \"action\": одно из [\"none\", \"play_pause\", \"next_track\", \"prev_track\", "
    "\"volume_up\", \"volume_down\", \"set_volume\", \"mute\", \"seek_forward\", \"seek_backward\", "
    "\"screen_home\", \"screen_music\", \"screen_video\", \"screen_assistant\", \"screen_eyes\", "
    "\"sleep_eyes\", \"hide_eyes\", \"open_app\", \"youtube_search\", \"open_music_app\", \"screenshot\", \"brightness_up\", \"brightness_down\"]. "
    "open_app — открыть приложение (название в query), youtube_search — найти и включить видео на YouTube (что искать — в query). "
    "Для обычных вопросов и бесед выбирай \"none\".\n"
    "2. \"value\": число (проценты громкости 0..100 или секунды перемотки, иначе 0).\n"
    "3. \"query\": название приложения или поисковый запрос для YouTube (иначе пустая строка).\n"
    "4. \"emotion\": \"happy\", \"speaking\", \"thinking\", \"confused\" или \"sleepy\".\n"
    "5. \"spoken\": твой точный, содержательный ответ пользователю — 1-2 коротких предложения (до 25 слов), без повторов; он будет озвучен вслух. "
    "Ключ spoken всегда ПОСЛЕДНИЙ."
)

# Ключевые основы слов, без которых аппаратное действие от LLM считается галлюцинацией модели 3B
_ACTION_GUARD_STEMS = {
    "play_pause": ["пауз", "стоп", "останов", "включ", "продолж", "играй", "плей", "тоқтат", "токтат", "қос", "кос", "ойнат", "жалғас", "музык"],
    "next_track": ["след", "дальше", "скип", "переключ", "келесі", "келеси", "трек", "ән", "песн"],
    "prev_track": ["предыд", "прошл", "верни", "алдыңғы", "алдынгы", "трек", "ән", "песн"],
    "volume_up": ["громк", "звук", "громче", "дыбыс", "дауыс", "қатт", "катт"],
    "volume_down": ["громк", "звук", "тише", "дыбыс", "дауыс", "ақырын", "акырын", "азайт"],
    "set_volume": ["громк", "звук", "дыбыс", "дауыс"],
    "mute": ["звук", "мут", "дыбыс", "дауыс", "өшір", "ошир"],
    "seek_forward": ["перемот", "секунд", "алға", "вперед", "вперёд"],
    "seek_backward": ["перемот", "секунд", "артқа", "назад"],
    "screen_home": ["экран", "главн", "домой", "басты"],
    "screen_music": ["экран", "плеер", "музык", "эквалайз"],
    "screen_video": ["экран", "видео", "бейне"],
    "screen_assistant": ["экран", "ассистент", "көмекші"],
    "screen_eyes": ["глаз", "көз", "коз", "проснись", "оян"],
    "sleep_eyes": ["спать", "спокойн", "усни", "засни", "сон", "режим сна", "ұйықта", "уйыкта", "демал"],
    "hide_eyes": ["глаз", "көз", "коз", "экран"],
    "open_app": ["откр", "запуст", "аш", "қос", "кос", "іске"],
    "youtube_search": ["ютуб", "ютюб", "youtube", "видео", "бейне", "тубтан"],
    "open_youtube": ["ютуб", "ютюб", "youtube"],
    "open_music_app": ["музык", "music"],
    "screenshot": ["скрин", "снимок"],
    "brightness_up": ["ярк", "ярче", "жарық", "жарык"],
    "brightness_down": ["ярк", "темн", "жарық", "жарык"],
}


def _guard_llm_action(action: str, user_text: str) -> str:
    """Отклоняет аппаратное действие, если в фразе пользователя нет ни одного связанного слова."""
    stems = _ACTION_GUARD_STEMS.get(action)
    if stems is None:
        return "none" if action not in ("none",) else action
    t = user_text.lower()
    if any(st in t for st in stems):
        return action
    logger.info(f"[VOICE LLM] Отклонено действие «{action}» — в запросе нет соответствующей команды")
    return "none"

_SPOKEN_START_RE = re.compile(r'"spoken"\s*:\s*"')
LLM_MAX_SENTENCES = 2
LLM_MAX_SPOKEN_CHARS = 240
_SENTENCE_BOUNDARY_RE = re.compile(r"[.!?…](?=\s)")


def _build_llm_context(current_state: dict, web_snippets: str) -> str:
    """Динамический контекст (время, погода, плеер, интернет) — передаётся в сообщении пользователя,
    чтобы системный промпт оставался неизменным и переиспользовался из KV-кэша Ollama."""
    now = datetime.datetime.now()
    title = (current_state.get("title") or "").strip()
    artist = (current_state.get("artist") or "").strip()
    source = (current_state.get("source") or "macOS").strip()
    vol = int(current_state.get("volume", 50))
    is_playing = (current_state.get("state") == "playing")
    if title and title != "Ойнатылмай тұр":
        track_info = f"«{title}» — {artist or 'белгісіз'} ({source})"
    else:
        track_info = "ештеңе ойнатылмай тұр"

    w_obj = current_state.get("weather") or {}
    temp_c = int(w_obj.get("temp", 18))
    desc_ru = w_obj.get("desc_ru", "ясно")
    usd_kzt = int(w_obj.get("usd_kzt", 495))
    batt_pct = int(w_obj.get("mac_battery", 100))
    temp_sign = f"+{temp_c}" if temp_c > 0 else str(temp_c)

    ctx = (
        f"[КОНТЕКСТ] Алматы, {now.strftime('%H:%M')}, {current_state.get('date', now.strftime('%d.%m'))}, 2026 жыл. "
        f"Ауа райы: {temp_sign}°C ({desc_ru}). 1 USD = {usd_kzt} ₸. MacBook батареясы: {batt_pct}%. "
        f"Плеер ({'ойнап тұр' if is_playing else 'кідіртілген'}): {track_info}, дыбыс {vol}%."
    )
    if web_snippets:
        ctx += f"\n[ИНТЕРНЕТ ДЕРЕКТЕРІ]\n{web_snippets}"
    return ctx


def _decode_partial_json_string(buf: str, start: int) -> tuple:
    """
    Декодирует JSON-строку, начинающуюся с buf[start] (после открывающей кавычки), даже если она ещё не дописана.
    Возвращает (текст, закрыта_ли_строка).
    """
    out = []
    i, n = start, len(buf)
    escapes = {"n": " ", "t": " ", "r": " ", '"': '"', "\\": "\\", "/": "/", "b": "", "f": ""}
    while i < n:
        ch = buf[i]
        if ch == '"':
            return "".join(out), True
        if ch == "\\":
            if i + 1 >= n:
                break
            esc = buf[i + 1]
            if esc == "u":
                if i + 6 > n:
                    break
                try:
                    out.append(chr(int(buf[i + 2:i + 6], 16)))
                except ValueError:
                    pass
                i += 6
                continue
            out.append(escapes.get(esc, esc))
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out), False


def _short_reply(spoken: str, limit: int = 32) -> str:
    """Краткая подпись для маленького экрана ESP32 из озвучиваемого ответа."""
    first = re.split(r"(?<=[.!?…])\s+", spoken.strip(), maxsplit=1)[0].strip()
    if len(first) <= limit:
        return first
    cut = first[:limit - 1].rsplit(" ", 1)[0].rstrip(",;:—- ")
    return (cut or first[:limit - 1]) + "…"


def _run_ollama_stream(url: str, model: str, messages: list, on_sentence=None, timeout: float = 30.0) -> tuple:
    """Выполняет потоковый запрос к Ollama и парсит поле spoken по предложениям."""
    payload = json.dumps({
        "model": model,
        "messages": messages,
        "format": "json",
        "stream": True,
        "keep_alive": "60m",
        "options": {
            "temperature": 0.3,
            "num_predict": 150,
            "repeat_penalty": 1.15,
            "repeat_last_n": 96,
        },
    }).encode("utf-8")

    req = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    content = ""
    spoken_start = None
    spoken_txt = ""
    emitted = 0
    t_first = None
    t0_req = time.time()

    with urllib.request.urlopen(req, timeout=timeout) as resp:
        for raw_line in resp:
            if not raw_line.strip():
                continue
            try:
                obj = json.loads(raw_line)
            except Exception:
                continue
            if obj.get("error"):
                raise RuntimeError(f"Ollama: {obj['error']}")
            content += obj.get("message", {}).get("content", "")

            if spoken_start is None:
                m = _SPOKEN_START_RE.search(content)
                if m:
                    spoken_start = m.end()
            if spoken_start is not None:
                spoken_txt, closed = _decode_partial_json_string(content, spoken_start)
                if on_sentence is not None:
                    pending = spoken_txt[emitted:]
                    cut = len(pending) if closed else 0
                    if not closed:
                        for mb in _SENTENCE_BOUNDARY_RE.finditer(pending):
                            cut = mb.end()
                    if cut > 0 and pending[:cut].strip():
                        if t_first is None:
                            t_first = time.time() - t0_req
                        try:
                            on_sentence(pending[:cut].strip())
                        except Exception as e:
                            logger.debug(f"[VOICE LLM] Ошибка on_sentence: {e}")
                        emitted += cut
                if not closed:
                    # Жёсткий лимит длины голосового ответа: 2 предложения или ~240 символов
                    ends = list(_SENTENCE_BOUNDARY_RE.finditer(spoken_txt))
                    if len(ends) >= LLM_MAX_SENTENCES:
                        spoken_txt = spoken_txt[:ends[LLM_MAX_SENTENCES - 1].end()]
                        closed = True
                    elif len(spoken_txt) > LLM_MAX_SPOKEN_CHARS:
                        cut_txt = spoken_txt[:LLM_MAX_SPOKEN_CHARS].rsplit(" ", 1)[0].rstrip(",;:—- ")
                        spoken_txt = cut_txt + "."
                        closed = True
                    if closed and on_sentence is not None and len(spoken_txt) > emitted:
                        tail = spoken_txt[emitted:].strip()
                        if tail:
                            if t_first is None:
                                t_first = time.time() - t0_req
                            on_sentence(tail)
                        emitted = len(spoken_txt)
                if closed:
                    break
            if obj.get("done"):
                break

    return content, spoken_txt, t_first


def query_local_llm(user_text: str, current_state: dict, on_sentence=None) -> dict:
    """
    Ищет актуальную информацию в интернете (если нужно) и отправляет запрос
    в модель Ollama в потоковом режиме.
    Приоритет: удалённый ПК с GPU 16GB (если доступен) -> локальный MacBook (авто-fallback).
    """
    t0 = time.time()

    # Поиск свежих данных в интернете для вопросов о фактах, спорте, событиях 2025/2026 и т.д.
    web_snippets = ""
    if _needs_web_search(user_text):
        web_snippets = search_web_context(user_text)
    t_web = time.time() - t0

    # Статичная часть промпта идёт первой и переиспользуется из KV-кэша; динамика — в самом конце
    messages = [{"role": "system", "content": f"{_LLM_SYSTEM_PROMPT}\n{_build_llm_context(current_state, web_snippets)}"}]
    for h in _chat_history[-4:]:
        messages.append(h)
    messages.append({"role": "user", "content": user_text})

    target_url, target_model, is_remote = get_ollama_endpoint()
    content = ""
    spoken_txt = ""
    t_first = None

    try:
        content, spoken_txt, t_first = _run_ollama_stream(
            target_url, target_model, messages, on_sentence=on_sentence, timeout=30.0
        )
    except Exception as e:
        if is_remote and _ai_config.get("fallback_local_ollama", True):
            logger.warning(f"[VOICE LLM] Удалённый ПК с GPU не ответил ({e}). Переключаюсь на локальный MacBook...")
            mark_remote_ollama_failed(str(e))
            local_url = _ai_config.get("local_ollama_url", "http://127.0.0.1:11434/api/chat")
            local_model = _ai_config.get("local_ollama_model", "qwen2.5:3b")
            content, spoken_txt, t_first = _run_ollama_stream(
                local_url, local_model, messages, on_sentence=on_sentence, timeout=35.0
            )
            target_url, target_model, is_remote = local_url, local_model, False
        else:
            raise

    parsed = _safe_parse_llm_json(content)
    if spoken_txt:
        parsed["spoken"] = spoken_txt.strip()
    if not parsed.get("reply") and parsed.get("spoken"):
        parsed["reply"] = _short_reply(parsed["spoken"])

    host_tag = "УДАЛЁННЫЙ ПК (GPU 16GB)" if is_remote else "ЛОКАЛЬНЫЙ MACBOOK"
    logger.info(
        f"[VOICE LLM] Ответ за {time.time() - t0:.2f} с [{host_tag}: {target_model}] "
        f"(поиск {t_web:.2f} с" + (f", первое предложение {t_first:.2f} с" if t_first is not None else "") + ")"
    )

    # Сохраняем реплику в краткосрочную память диалога
    _chat_history.append({"role": "user", "content": user_text})
    _chat_history.append({"role": "assistant", "content": json.dumps(
        {k: parsed.get(k) for k in ("action", "value", "query", "emotion", "spoken")}, ensure_ascii=False)})
    if len(_chat_history) > 8:
        del _chat_history[:-8]

    return parsed


def _execute_action(action: str, value: int, current_state: dict, set_volume_fn, media_cmd_fn, query: str = "") -> str:
    """Выполняет аппаратное/системное действие, выбранное локальной моделью, и возвращает код экрана."""
    target_screen = ""
    cur_vol = int(current_state.get("volume", 50))

    if action == "open_app":
        app_hit = _find_app_alias((query or "").lower())
        if app_hit is not None:
            open_mac_app(*app_hit)
        elif query:
            open_mac_app(query.strip(), query.strip(), None)
        return ""
    if action in ("youtube_search", "open_youtube"):
        open_youtube_query((query or "").strip(), autoplay=True)
        return "video"

    if action == "screen_eyes":
        target_screen = "eyes"
    elif action == "sleep_eyes":
        target_screen = "sleep_eyes"
    elif action == "hide_eyes":
        target_screen = "hide_eyes"
    elif action == "screen_home":
        target_screen = "home"
    elif action == "screen_music":
        target_screen = "music"
    elif action == "screen_video":
        target_screen = "video"
    elif action == "screen_assistant":
        target_screen = "assistant"
    elif action == "next_track":
        media_cmd_fn("NEXT_TRACK")
    elif action == "prev_track":
        media_cmd_fn("PREVIOUS_TRACK")
    elif action == "play_pause":
        media_cmd_fn("PLAY_PAUSE")
    elif action == "seek_forward":
        sec = value if (isinstance(value, int) and 1 <= value <= 600) else 10
        media_cmd_fn("SEEK_FORWARD", sec)
    elif action == "seek_backward":
        sec = value if (isinstance(value, int) and 1 <= value <= 600) else 10
        media_cmd_fn("SEEK_BACKWARD", sec)
    elif action == "mute":
        set_volume_fn(0)
    elif action == "set_volume":
        v = value if (isinstance(value, int) and 0 <= value <= 100) else cur_vol
        set_volume_fn(v)
    elif action == "volume_up":
        step = value if (isinstance(value, int) and 1 <= value <= 50) else 15
        set_volume_fn(min(100, cur_vol + step))
    elif action == "volume_down":
        step = value if (isinstance(value, int) and 1 <= value <= 50) else 15
        set_volume_fn(max(0, cur_vol - step))
    elif action == "open_youtube":
        subprocess.Popen(["/usr/bin/open", "https://www.youtube.com"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif action == "open_music_app":
        subprocess.Popen(["/usr/bin/open", "-a", "Music"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif action == "screenshot":
        _take_mac_screenshot()
    elif action == "brightness_up":
        _adjust_mac_brightness(up=True, steps=4)
    elif action == "brightness_down":
        _adjust_mac_brightness(up=False, steps=4)

    return target_screen


def process_voice_command(
    text: str,
    current_state: dict,
    set_volume_fn,
    media_cmd_fn,
    on_thinking_fn=None,
    bypass_wake: bool = False
) -> dict:
    """
    Обрабатывает распознанную голосовую фразу:
    0) Проверяет наличие триггер-слова «Дос» (или активность открытого окна диалога).
       Если триггер-слова нет — фраза тихо игнорируется (status='ignored'), не беспокоя экран и динамики.
    1) Если названо только имя («Дос» / «Сәлем Дос» / «Досым») — откликается («Иә, тыңдап тұрмын!»)
       и открывает окно ожидания команды на 7 секунд.
    2) Короткие чёткие команды медиа/экрана, системные действия и вопросы о треке/погоде/времени выполняет мгновенно.
    3) Свободные формулировки и общие вопросы направляет в DuckDuckGo + локальную LLM (Ollama qwen2.5:3b).
    """
    global _wake_active_until

    raw_text = (text or "").strip()
    now = time.time()
    with _wake_lock:
        is_session_active = (now < _wake_active_until)

    if not raw_text:
        # Если речь не распознана и нет активного окна диалога — тихо игнорируем
        if not bypass_wake and not is_session_active:
            return {
                "type": "voice_result",
                "status": "ignored",
                "heard": "",
                "reply": "",
                "screen": "",
                "emotion": "",
            }
        return {
            "type": "voice_result",
            "status": "empty",
            "heard": "Речь не распознана",
            "reply": "Повторите команду чётче",
            "screen": "",
            "emotion": "confused",
        }

    has_wake_word = bool(_WAKE_WORD_RE.search(raw_text))

    # Если триггер-слова нет и нет активного окна диалога (и не включён bypass) — игнорируем!
    if not bypass_wake and not has_wake_word and not is_session_active:
        logger.info(f"[VOICE WAKE] Проигнорирована фраза без триггер-слова «Дос»: «{raw_text}»")
        return {
            "type": "voice_result",
            "status": "ignored",
            "heard": raw_text,
            "reply": "",
            "screen": "",
            "emotion": "",
        }

    # Команда сна / отбоя («ұйықта», «болды», «демал», «засни», «отбой»)
    t_lower = raw_text.lower()
    if any(k in t_lower for k in ["ұйықта", "уйыкта", "засни", "болды", "отбой", "демал", "тыныштал"]):
        with _wake_lock:
            _wake_active_until = 0.0
        spoken = "Жақсы, мен ұйықтадым. Шақырсаң, осындамын."
        reply = "Демалу режимі (Zzz)"
        logger.info(f"[VOICE WAKE] Команда сна: «{raw_text}» -> переход в режим сна")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": raw_text,
            "reply": reply,
            "screen": "sleep_eyes",
            "emotion": "sleepy",
        }

    # Очищаем команду от обращения к Досу в начале или конце фразы
    effective_text = raw_text
    if has_wake_word:
        effective_text = _LEADING_GREETINGS_RE.sub("", effective_text).strip()
        if _WAKE_WORD_RE.search(effective_text):
            effective_text = _WAKE_WORD_RE.sub("", effective_text).strip(" ,!?:;-")

    # Если пользователь ТОЛЬКО позвал помощника по имени («Дос», «Сәлем Дос», «Досым»):
    if has_wake_word and (not effective_text or len(effective_text) <= 1):
        with _wake_lock:
            # Окно ожидания команды активно 7 секунд
            _wake_active_until = time.time() + 7.0

        is_kk = _is_kazakh_text(raw_text) or any(k in t_lower for k in ["сәлем", "салем", "досым", "дос"])
        if is_kk:
            spoken = "Иә, тыңдап тұрмын!"
            reply = "Тыңдап тұрмын..."
        else:
            spoken = "Да, я слушаю!"
            reply = "Слушаю вас..."

        logger.info(f"[VOICE WAKE] Оклик помощника: «{raw_text}» -> Окно ожидания команды активно (7 сек)")
        speak_on_mac(spoken)
        return {
            "type": "voice_result",
            "status": "ok",
            "heard": raw_text,
            "reply": reply,
            "screen": "eyes",
            "emotion": "speaking",
        }

    # Если была конкретная команда, закрываем окно диалога после её выполнения
    with _wake_lock:
        _wake_active_until = 0.0

    command_text = effective_text if effective_text else raw_text

    # 1. Проверяем быстрый путь для мгновенных аппаратных команд
    fast_res = _try_fast_command(command_text, current_state, set_volume_fn, media_cmd_fn)
    if fast_res is not None:
        fast_res["heard"] = raw_text
        return fast_res

    # 2. Сообщаем на экран ESP32-S3, что фраза распознана и идёт поиск/генерация ответа
    if on_thinking_fn is not None:
        try:
            on_thinking_fn({
                "type": "voice_result",
                "status": "thinking",
                "heard": raw_text,
                "reply": "Ойланып жатырмын...",
                "screen": "",
                "emotion": "thinking",
            })
        except Exception:
            pass

    t = command_text.lower().strip()
    num_match = re.search(r"\b(\d{1,3})\b", t)
    num_val = int(num_match.group(1)) if num_match else 0

    # Потоковая озвучка: речевая сессия создаётся при первом готовом предложении от LLM
    stream_session = {}

    def _on_sentence(sentence: str):
        if "s" not in stream_session:
            stream_session["s"] = SpeechSession()
        stream_session["s"].feed(sentence)

    try:
        llm_res = query_local_llm(command_text, current_state, on_sentence=_on_sentence)
        action = _guard_llm_action(str(llm_res.get("action") or "none").strip(), command_text)
        val = llm_res.get("value")
        val = int(val) if isinstance(val, (int, float)) or (isinstance(val, str) and val.isdigit()) else num_val
        spoken = str(llm_res.get("spoken") or "").strip()
        reply = str(llm_res.get("reply") or "").strip()
        target_emotion = str(llm_res.get("emotion") or "speaking").strip()
        if target_emotion not in ("happy", "speaking", "thinking", "confused", "sleepy"):
            target_emotion = "speaking"

        if action == "none":
            if any(k in t for k in ["живые глаза", "покажи глаза", "открой глаза", "глаза робота"]):
                action = "screen_eyes"
                target_emotion = "happy"
            elif any(k in t for k in ["скрой глаза", "закрой глаза", "обычный режим"]):
                action = "hide_eyes"

        target_screen = _execute_action(action, val, current_state, set_volume_fn, media_cmd_fn,
                                        str(llm_res.get("query") or ""))

        # Ссылки не озвучиваем и не выводим на маленький экран
        spoken = _clean_for_tts(spoken)
        reply = re.sub(r"https?://\S+|www\.\S+", "", reply).strip()

        if not spoken:
            spoken = reply or "Готово!"
        if not reply:
            reply = _short_reply(spoken)

    except Exception as e:
        logger.warning(f"[VOICE LLM] Ошибка обращения к Ollama ({e}), использую резервный ответ")
        action = "none"
        target_screen = ""
        target_emotion = "speaking"
        reply = f"Услышал: {command_text[:24]}"
        spoken = f"Мен сені естідім: {command_text}"

    logger.info(
        f"[VOICE AI] Фраза: '{raw_text}' | Команда: '{command_text}' | Action: '{action}' | Экран: '{reply}' | Голос: '{spoken}'"
    )

    if "s" in stream_session:
        stream_session["s"].finish()  # Ответ уже озвучивается по предложениям
    else:
        speak_on_mac(spoken)

    return {
        "type": "voice_result",
        "status": "ok",
        "heard": raw_text,
        "reply": reply,
        "screen": target_screen,
        "emotion": target_emotion,
    }


def _warmup_endpoint(url: str, model: str, tag: str = "Ollama"):
    try:
        payload = json.dumps({
            "model": model,
            "messages": [
                {"role": "system", "content": _LLM_SYSTEM_PROMPT},
                {"role": "user", "content": "[СҰРАҚ] сәлем"},
            ],
            "format": "json",
            "stream": False,
            "keep_alive": "60m",
            "options": {"num_predict": 1},
        }).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        urllib.request.urlopen(req, timeout=15).read()
        logger.info(f"[VOICE LLM] Модель {model} на {tag} успешно прогрета в VRAM/ОЗУ")
    except Exception as e:
        logger.debug(f"[VOICE LLM] Прогрев {tag} ({url}): {e}")


def _warmup_llm():
    """
    Фоновый прогрев моделей Ollama при старте сервера.
    Прогревает локальный MacBook (гарантия быстрого fallback), а также удалённый ПК (если задан).
    """
    local_url = _ai_config.get("local_ollama_url", "http://127.0.0.1:11434/api/chat")
    local_model = _ai_config.get("local_ollama_model", "qwen2.5:3b")
    _warmup_endpoint(local_url, local_model, "локальном MacBook")

    target_url, target_model, is_remote = get_ollama_endpoint()
    if is_remote:
        _warmup_endpoint(target_url, target_model, "удалённом ПК (GPU 16GB)")


threading.Thread(target=_warmup_llm, daemon=True).start()
threading.Thread(target=_prewarm_tts_cache, daemon=True).start()


def _preload_stt():
    """Загружает локальную STT-модель заранее, чтобы первый голосовой запрос не ждал ~1 сек загрузки."""
    if get_stt_engine is None:
        return
    try:
        get_stt_engine()
    except Exception as e:
        logger.warning(f"[VOICE STT] Не удалось предзагрузить STT: {e}")


threading.Thread(target=_preload_stt, daemon=True).start()
