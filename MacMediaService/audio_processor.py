#!/usr/bin/env python3
"""
audio_processor.py - Модуль захвата и спектральной обработки системного аудио macOS
для проекта умного голосового помощника на базе ESP32-S3 (320x240 TFT).

Архитектура модуля:
1. Нативный движок захвата системного звука (audio_analyzer / ScreenCaptureKit + Accelerate vDSP):
   - Захватывает системный микс macOS (Яндекс Музыка, YouTube, Apple Music, Spotify и др.)
     без изменения устройства вывода звука и без использования микрофона.
   - Выполняет оконное преобразование Ханна и 2048-точечное БПФ (vDSP) в отдельном DSP-потоке.
2. Полноценный DSP-процессор PCM в Python (PCMSpectrumAnalyzer):
   - Поддерживает частоту дискретизации 44100 / 48000 / 88200 / 96000 Гц.
   - Преобразует стерео PCM (float32 / int16) в моно с защитой от клиппинга и NaN/Inf.
   - Вычисляет 16 логарифмически распределённых частотных диапазонов (25 Гц – 16 кГц).
   - Выполняет перцептивную коррекцию наклона спектра, нормализацию (0..100) и
     асимметричное сглаживание (быстрая атака / плавный спад).
3. Менеджер потока эквалайзера (AudioEqualizerService):
   - Работает в фоновом потоке, не блокируя основной цикл сервера или отрисовку.
   - Ограничивает частоту отправки пакетов (до 30 Гц при активном звуке, экономичный режим в тишине).
   - Обеспечивает плавное затухание полос до 0 при паузе, остановке или отсутствии звука.
"""

import os
import sys
import math
import cmath
import json
import time
import struct
import logging
import threading
import subprocess

logger = logging.getLogger("MacMediaService.Audio")

DEFAULT_BAND_COUNT = 16
MIN_FREQ_HZ = 25.0
MAX_FREQ_HZ = 16000.0
SUPPORTED_SAMPLE_RATES = (22050, 32000, 44100, 48000, 88200, 96000, 192000)


class PCMSpectrumAnalyzer:
    """
    DSP-процессор PCM-сигнала с вычислением N (по умолчанию 16) логарифмических
    частотных диапазонов с помощью БПФ Кули-Тьюки (Radix-2 FFT).
    """

    def __init__(
        self,
        band_count=DEFAULT_BAND_COUNT,
        fft_size=1024,
        sample_rate=48000,
        min_freq=MIN_FREQ_HZ,
        max_freq=MAX_FREQ_HZ,
        attack=0.65,
        release=0.22,
        noise_floor_db=-58.0,
    ):
        if band_count < 4 or band_count > 64:
            raise ValueError("band_count должен быть в диапазоне 4..64")
        if fft_size & (fft_size - 1) != 0 or fft_size < 256:
            raise ValueError("fft_size должен быть степенью двойки >= 256")

        self.band_count = int(band_count)
        self.fft_size = int(fft_size)
        self.half_size = self.fft_size // 2
        self.sample_rate = float(sample_rate) if sample_rate in SUPPORTED_SAMPLE_RATES else 48000.0
        self.min_freq = float(min_freq)
        self.max_freq = float(max_freq)
        self.attack = float(attack)
        self.release = float(release)
        self.noise_floor_db = float(noise_floor_db)

        self._ring_buffer = [0.0] * self.fft_size
        self._write_pos = 0
        self._samples_collected = 0
        self._smoothed_bands = [0.0] * self.band_count
        self._adaptive_peak_db = -14.0
        self._last_rms = 0.0
        self._last_audio_ts = 0.0

        # Предварительный расчёт окна Ханна и бит-реверсивной таблицы для быстрого БПФ
        self._hann = [
            0.5 * (1.0 - math.cos(2.0 * math.pi * i / self.fft_size))
            for i in range(self.fft_size)
        ]
        self._bit_rev = self._build_bit_reversal(self.fft_size)
        self._twiddles = self._build_twiddles(self.fft_size)
        self._lock = threading.Lock()

    @staticmethod
    def _build_bit_reversal(n):
        bits = int(round(math.log2(n)))
        rev = [0] * n
        for i in range(n):
            r = 0
            val = i
            for _ in range(bits):
                r = (r << 1) | (val & 1)
                val >>= 1
            rev[i] = r
        return rev

    @staticmethod
    def _build_twiddles(n):
        twiddles = {}
        step = 2
        while step <= n:
            half = step // 2
            angle = -2.0 * math.pi / step
            twiddles[step] = [cmath. rect(1.0, angle * k) for k in range(half)]
            step <<= 1
        return twiddles

    @staticmethod
    def sanitize_sample(val):
        """Защита от NaN, Inf и жёсткого клиппинга за пределами [-1.0, 1.0]."""
        if not isinstance(val, (int, float)) or math.isnan(val) or math.isinf(val):
            return 0.0
        if val > 1.0:
            return 1.0
        if val < -1.0:
            return -1.0
        return float(val)

    @classmethod
    def stereo_to_mono(cls, samples, channels=2):
        """Преобразует интерливированный стерео/многоканальный массив отсчётов в моно."""
        if not samples:
            return []
        if channels <= 1:
            return [cls.sanitize_sample(s) for s in samples]

        frame_count = len(samples) // channels
        mono = [0.0] * frame_count
        for i in range(frame_count):
            base = i * channels
            left = cls.sanitize_sample(samples[base])
            right = cls.sanitize_sample(samples[base + 1])
            mono[i] = 0.5 * (left + right)
        return mono

    @classmethod
    def decode_pcm_bytes(cls, raw_bytes, sample_format="float32", channels=2):
        """Декодирует сырые PCM-байты (float32 или int16) в нормализованный моно-список [-1.0, 1.0]."""
        if not raw_bytes:
            return []
        if sample_format == "float32":
            count = len(raw_bytes) // 4
            if count == 0:
                return []
            floats = struct.unpack(f"<{count}f", raw_bytes[: count * 4])
            return cls.stereo_to_mono(floats, channels=channels)
        elif sample_format == "int16":
            count = len(raw_bytes) // 2
            if count == 0:
                return []
            shorts = struct.unpack(f"<{count}h", raw_bytes[: count * 2])
            scaled = [s / 32768.0 for s in shorts]
            return cls.stereo_to_mono(scaled, channels=channels)
        else:
            raise ValueError(f"Неподдерживаемый формат PCM: {sample_format}")

    def push_pcm(self, samples, channels=1, sample_rate=None, now=None):
        """Принимает новые PCM-отсчёты (моно или стерео) в кольцевой буфер."""
        if now is None:
            now = time.time()
        if sample_rate is not None and 8000 <= sample_rate <= 192000:
            self.sample_rate = float(sample_rate)

        mono = self.stereo_to_mono(samples, channels=channels) if channels > 1 else [
            self.sanitize_sample(s) for s in samples
        ]
        if not mono:
            return

        with self._lock:
            sum_sq = 0.0
            mask = self.fft_size - 1
            for s in mono:
                sum_sq += s * s
                self._ring_buffer[self._write_pos] = s
                self._write_pos = (self._write_pos + 1) & mask

            self._samples_collected = min(self.fft_size, self._samples_collected + len(mono))
            rms = math.sqrt(sum_sq / len(mono))
            self._last_rms = rms
            if rms > 0.0004:
                self._last_audio_ts = now

    def _fft_magnitudes(self, windowed):
        """Выполняет быстрое преобразование Фурье Кули-Тьюки (Radix-2) и возвращает спектр амплитуд."""
        n = self.fft_size
        rev = self._bit_rev
        buf = [complex(windowed[rev[i]], 0.0) for i in range(n)]

        step = 2
        while step <= n:
            half = step // 2
            tw = self._twiddles[step]
            for start in range(0, n, step):
                for k in range(half):
                    t = tw[k] * buf[start + half + k]
                    u = buf[start + k]
                    buf[start + k] = u + t
                    buf[start + half + k] = u - t
            step <<= 1

        inv_n = 2.0 / n
        mags = [0.0] * self.half_size
        for k in range(1, self.half_size):
            mags[k] = abs(buf[k]) * inv_n
        return mags

    def compute_bands(self, now=None):
        """Вычисляет уровни 16 логарифмических частотных диапазонов (0..100)."""
        if now is None:
            now = time.time()

        with self._lock:
            silent = (
                (now - self._last_audio_ts > 0.10)
                or (self._samples_collected < 256)
                or (self._last_rms < 0.00035)
            )
            if silent:
                out = [0] * self.band_count
                for b in range(self.band_count):
                    self._smoothed_bands[b] *= (1.0 - self.release)
                    if self._smoothed_bands[b] < 0.6:
                        self._smoothed_bands[b] = 0.0
                    out[b] = int(round(min(100.0, max(0.0, self._smoothed_bands[b]))))
                return out

            mask = self.fft_size - 1
            windowed = [
                self._ring_buffer[(self._write_pos + i) & mask] * self._hann[i]
                for i in range(self.fft_size)
            ]
            sr = self.sample_rate

        mags = self._fft_magnitudes(windowed)
        nyquist = sr * 0.5
        f_min = max(20.0, self.min_freq)
        f_max = min(self.max_freq, nyquist * 0.92)
        ratio = f_max / f_min
        bin_hz = sr / self.fft_size

        raw_db = [self.noise_floor_db] * self.band_count
        frame_max_db = self.noise_floor_db

        for b in range(self.band_count):
            t0 = b / self.band_count
            t1 = (b + 1) / self.band_count
            f_low = f_min * (ratio ** t0)
            f_high = f_min * (ratio ** t1)
            f_center = math.sqrt(f_low * f_high)

            k_low = f_low / bin_hz
            k_high = f_high / bin_hz

            if k_high - k_low < 1.0:
                k_center = 0.5 * (k_low + k_high)
                k0 = max(1, min(self.half_size - 2, int(math.floor(k_center))))
                frac = k_center - k0
                band_mag = mags[k0] * (1.0 - frac) + mags[k0 + 1] * frac
            else:
                start_bin = max(1, min(self.half_size - 1, int(math.floor(k_low))))
                end_bin = max(start_bin, min(self.half_size - 1, int(math.ceil(k_high))))
                count = end_bin - start_bin + 1
                sum_sq = 0.0
                max_val = 0.0
                for k in range(start_bin, end_bin + 1):
                    m = mags[k]
                    sum_sq += m * m
                    if m > max_val:
                        max_val = m
                rms_mag = math.sqrt(sum_sq / count)
                band_mag = 0.60 * rms_mag + 0.40 * max_val

            tilt = (f_center / 180.0) ** 0.32
            weighted = band_mag * tilt
            db = 20.0 * math.log10(max(weighted, 1e-7))
            raw_db[b] = db
            if db > frame_max_db:
                frame_max_db = db

        with self._lock:
            if frame_max_db > self._adaptive_peak_db:
                self._adaptive_peak_db = self._adaptive_peak_db * 0.80 + frame_max_db * 0.20
            else:
                self._adaptive_peak_db = self._adaptive_peak_db * 0.992 + frame_max_db * 0.008
            self._adaptive_peak_db = max(-26.0, min(-6.0, self._adaptive_peak_db))

            dyn_span = max(18.0, self._adaptive_peak_db - self.noise_floor_db)
            out_bands = [0] * self.band_count

            for b in range(self.band_count):
                norm = (raw_db[b] - self.noise_floor_db) / dyn_span
                clamped = max(0.0, min(1.0, norm))
                target = (clamped ** 1.15) * 100.0
                prev = self._smoothed_bands[b]
                if target > prev:
                    self._smoothed_bands[b] = prev + self.attack * (target - prev)
                else:
                    self._smoothed_bands[b] = prev + self.release * (target - prev)
                if self._smoothed_bands[b] < 0.8:
                    self._smoothed_bands[b] = 0.0
                out_bands[b] = int(round(min(100.0, max(0.0, self._smoothed_bands[b]))))

            return out_bands


def sanitize_equalizer_bands(bands, expected_count=DEFAULT_BAND_COUNT):
    """Проверяет и нормализует список полос эквалайзера (защита входных данных)."""
    if not isinstance(bands, (list, tuple)):
        return [0] * expected_count
    result = [0] * expected_count
    limit = min(len(bands), expected_count)
    for i in range(limit):
        val = bands[i]
        try:
            if isinstance(val, (int, float)) and not math.isnan(val) and not math.isinf(val):
                result[i] = max(0, min(100, int(round(val))))
        except Exception:
            result[i] = 0
    return result


class AudioEqualizerService:
    """
    Фоновый сервис захвата системного аудио macOS и рассылки 16 полос эквалайзера.
    Управляет нативным процессом audio_analyzer (ScreenCaptureKit + Accelerate vDSP).
    """

    def __init__(self, base_dir, on_equalizer_frame, band_count=DEFAULT_BAND_COUNT, target_fps=30):
        self.base_dir = base_dir
        self.on_equalizer_frame = on_equalizer_frame
        self.band_count = band_count
        self.target_fps = target_fps

        self.swift_src = os.path.join(base_dir, "audio_analyzer.swift")
        self.binary_path = os.path.join(base_dir, "audio_analyzer")

        self._proc = None
        self._running = False
        self._thread = None
        self._lock = threading.Lock()

        self.current_bands = [0] * band_count
        self.audio_active = False
        self.capturing = False
        self.permission_granted = False
        self.last_update_ts = 0.0
        self._logged_permission_notice = False

    def ensure_binary_compiled(self):
        """Компилирует audio_analyzer.swift в нативный бинарный файл arm64 при необходимости."""
        if not os.path.exists(self.swift_src):
            logger.error(f"Исходный файл {self.swift_src} не найден.")
            return False

        need_compile = not os.path.exists(self.binary_path)
        if not need_compile:
            try:
                src_mtime = os.path.getmtime(self.swift_src)
                bin_mtime = os.path.getmtime(self.binary_path)
                if src_mtime > bin_mtime:
                    need_compile = True
            except OSError:
                need_compile = True

        if need_compile:
            logger.info("Компиляция нативного модуля анализа аудио (audio_analyzer.swift)...")
            try:
                res = subprocess.run(
                    ["/usr/bin/swiftc", "-O", self.swift_src, "-o", self.binary_path],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=60,
                )
                if res.returncode != 0:
                    logger.error(f"Ошибка компиляции audio_analyzer.swift: {res.stderr}")
                    return False
                logger.info("Модуль audio_analyzer успешно скомпилирован.")
            except Exception as e:
                logger.error(f"Не удалось скомпилировать audio_analyzer: {e}")
                return False
        return True

    def start(self):
        """Запускает фоновый поток чтения спектральных данных."""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._worker_loop, daemon=True, name="AudioEqualizerThread")
        self._thread.start()

    def stop(self):
        """Останавливает фоновый сервис и дочерний процесс анализатора."""
        self._running = False
        proc = self._proc
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            self._proc = None

    def get_snapshot(self):
        """Возвращает текущее состояние анализатора аудио."""
        with self._lock:
            return {
                "bands": list(self.current_bands),
                "active": self.audio_active,
                "capturing": self.capturing,
                "permission": self.permission_granted,
                "last_update": self.last_update_ts,
            }

    def _stderr_reader(self, pipe):
        """Читает диагностические сообщения из stderr процесса audio_analyzer."""
        try:
            for line in iter(pipe.readline, ""):
                if not self._running:
                    break
                msg = line.strip()
                if msg:
                    logger.info(msg)
        except Exception:
            pass

    def _worker_loop(self):
        if not os.path.exists(self.swift_src):
            logger.error(f"Исходный файл {self.swift_src} не найден.")
            return

        while self._running:
            try:
                cmd = [
                    "/usr/bin/swift",
                    self.swift_src,
                    "--bands", str(self.band_count),
                    "--fps", str(self.target_fps),
                ]
                self._proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1,
                )

                t_err = threading.Thread(target=self._stderr_reader, args=(self._proc.stderr,), daemon=True)
                t_err.start()

                for line in iter(self._proc.stdout.readline, ""):
                    if not self._running:
                        break
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        payload = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    if payload.get("type") != "equalizer":
                        continue

                    bands = sanitize_equalizer_bands(payload.get("bands"), self.band_count)
                    active = bool(payload.get("active", False)) or any(b > 0 for b in bands)
                    capturing = bool(payload.get("capturing", False))
                    perm = bool(payload.get("permission", False))
                    now = time.time()

                    if not perm and not self._logged_permission_notice:
                        self._logged_permission_notice = True
                        logger.warning(
                            "[AUDIO] Для захвата системного звука через ScreenCaptureKit требуется разрешение "
                            "'Запись экрана и системного аудио' в Системных настройках macOS."
                        )
                    elif perm and self._logged_permission_notice:
                        self._logged_permission_notice = False
                        logger.info("[AUDIO] Разрешение ScreenCaptureKit получено, захват звука активен!")

                    with self._lock:
                        self.current_bands = bands
                        self.audio_active = active
                        self.capturing = capturing
                        self.permission_granted = perm
                        self.last_update_ts = now

                    if self.on_equalizer_frame:
                        try:
                            self.on_equalizer_frame(bands, active)
                        except Exception as cb_err:
                            logger.error(f"Ошибка в callback эквалайзера: {cb_err}")

            except Exception as e:
                if self._running:
                    logger.error(f"Ошибка процесса audio_analyzer: {e}")
            finally:
                if self._proc is not None:
                    try:
                        self._proc.kill()
                    except Exception:
                        pass
                    self._proc = None

            if self._running:
                time.sleep(2.0)

