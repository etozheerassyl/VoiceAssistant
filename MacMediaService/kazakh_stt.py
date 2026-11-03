#!/usr/bin/env python3
"""
kazakh_stt.py — Локальный билингвальный модуль распознавания речи (Kazakh-Russian Mixed STT).
Использует модель alibiserikbay/kazakh-russian-mixed-stt (wav2vec2-CTC, 188.8M параметров).
Обеспечивает быстрое (300-500 мс) распознавание казахской, русской и смешанной речи без облачных API.
"""

import os
import sys
import re
import logging
import numpy as np
import torch
import torchaudio

logger = logging.getLogger("KazakhSTT")
if not logger.handlers:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter("[%(asctime)s] [%(name)s] [%(levelname)s] %(message)s"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(BASE_DIR)
MODEL_DIR = os.path.join(PROJECT_ROOT, "models", "kazakh-russian-mixed-stt", "asr", "rukk")
MODEL_PATH = os.path.join(MODEL_DIR, "model.pt")
TOKENS_PATH = os.path.join(MODEL_DIR, "tokens.lst")


class KazakhRussianMixedSTT:
    """Офлайн-движок распознавания казахской и русской речи на базе TorchScript wav2vec2-CTC."""

    _instance = None

    @classmethod
    def get_instance(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self, model_path: str = MODEL_PATH, tokens_path: str = TOKENS_PATH):
        self.model_path = model_path
        self.tokens_path = tokens_path
        self.model = None
        self.tokens = {}
        self.blank_id = 0
        self.resamplers = {}
        self._load_model()

    def _load_model(self):
        if not os.path.exists(self.model_path) or not os.path.exists(self.tokens_path):
            logger.warning(f"Файлы STT модели не найдены: {self.model_path} или {self.tokens_path}")
            return

        try:
            logger.info("Загрузка модели Kazakh-Russian Mixed STT (TorchScript)...")
            self.model = torch.jit.load(self.model_path, map_location="cpu").eval()

            # Загрузка словаря токенов
            self.tokens = {}
            with open(self.tokens_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        parts = line.split("\t")
                        if len(parts) >= 2:
                            sym, idx = parts[0], int(parts[1])
                            self.tokens[idx] = sym

            self.blank_id = max(self.tokens.keys()) + 1 if self.tokens else 0
            logger.info(f"STT модель успешно инициализирована ({len(self.tokens)} токенов, blank={self.blank_id})")
        except Exception as e:
            logger.error(f"Ошибка загрузки модели STT: {e}", exc_info=True)
            self.model = None

    def is_ready(self) -> bool:
        return self.model is not None

    def _get_resampler(self, orig_sr: int, target_sr: int = 16000):
        key = (orig_sr, target_sr)
        if key not in self.resamplers:
            self.resamplers[key] = torchaudio.transforms.Resample(orig_freq=orig_sr, new_freq=target_sr)
        return self.resamplers[key]

    def transcribe_waveform(self, waveform: torch.Tensor, sample_rate: int = 16000) -> str:
        """
        Принимает 1D или 2D тензор waveform float32 [-1.0, 1.0].
        Ресемплирует до 16000 Гц при необходимости и возвращает распознанный текст.
        """
        if not self.is_ready():
            return ""

        if waveform.ndim > 1 and waveform.shape[0] > 1:
            waveform = waveform.mean(dim=0)
        elif waveform.ndim > 1 and waveform.shape[0] == 1:
            waveform = waveform.squeeze(0)

        if sample_rate != 16000:
            resampler = self._get_resampler(sample_rate, 16000)
            waveform = resampler(waveform)

        # Инференс
        try:
            with torch.no_grad():
                logits = self.model(waveform.unsqueeze(0))[0]

            ids = logits[0].argmax(dim=-1).tolist()

            # CTC декодирование (схлопывание повторов и удаление blank)
            out = []
            prev = None
            for idx in ids:
                if idx != prev and idx != self.blank_id:
                    out.append(self.tokens.get(idx, ""))
                prev = idx

            raw_text = "".join(out).replace("|", " ").replace("_", " ")
            cleaned = re.sub(r"\s+", " ", raw_text).strip()
            return cleaned
        except Exception as e:
            logger.error(f"Ошибка инференса STT: {e}")
            return ""

    def transcribe_pcm16(self, raw_pcm: bytes, sample_rate: int = 8000) -> str:
        """
        Принимает сырой 16-битный моно PCM буфер (например, с микрофона ESP32-S3).
        Возвращает распознанную строку.
        """
        if not raw_pcm or not self.is_ready():
            return ""

        try:
            # Преобразование int16 в float32 тензор [-1.0, 1.0]
            samples_i16 = np.frombuffer(raw_pcm, dtype=np.int16)
            if len(samples_i16) == 0:
                return ""
            samples_f32 = torch.from_numpy(samples_i16.astype(np.float32) / 32768.0)
            return self.transcribe_waveform(samples_f32, sample_rate=sample_rate)
        except Exception as e:
            logger.error(f"Ошибка декодирования PCM16 в STT: {e}")
            return ""

    def transcribe_file(self, file_path: str) -> str:
        """Распознаёт речь из аудиофайла (WAV, AIFF, FLAC и т.д.)."""
        import soundfile as sf
        if not os.path.exists(file_path):
            return ""
        try:
            wav, sr = sf.read(file_path, dtype="float32")
            if wav.ndim > 1:
                wav = wav.mean(axis=1)
            tensor_wav = torch.from_numpy(wav)
            return self.transcribe_waveform(tensor_wav, sample_rate=sr)
        except Exception as e:
            logger.error(f"Ошибка чтения файла {file_path}: {e}")
            return ""


_engine = None

def get_stt_engine() -> KazakhRussianMixedSTT:
    global _engine
    if _engine is None:
        _engine = KazakhRussianMixedSTT.get_instance()
    return _engine


if __name__ == "__main__":
    stt = get_stt_engine()
    print("Готовность STT:", stt.is_ready())

    samples_dir = os.path.join(PROJECT_ROOT, "audio_samples")
    for f in ["greeting.wav", "letters_spec.wav", "numbers_dates.wav", "mixed_speech.wav", "geo_names.wav"]:
        p = os.path.join(samples_dir, f)
        if os.path.exists(p):
            t = stt.transcribe_file(p)
            print(f"[{f}]: {t}")

