#!/usr/bin/env python3
"""
Kazakh TTS FastAPI Service на базе KazakhTTS-OmniVoice (612M параметров).
Обеспечивает естественное озвучивание казахоязычного ассистента «Дос».
"""

import os
import sys
import time
import hashlib
import tempfile
import threading
import subprocess
from typing import Optional
import torch
import soundfile as sf
from fastapi import FastAPI, HTTPException, BackgroundTasks
from fastapi.responses import FileResponse
from pydantic import BaseModel
import uvicorn

# Базовые пути
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(BASE_DIR)
MODEL_DIR = os.path.join(PROJECT_DIR, "models", "KazakhTTS-OmniVoice")
CACHE_DIR = os.path.join(tempfile.gettempdir(), "kazakh_tts_cache")
os.makedirs(CACHE_DIR, exist_ok=True)

# Инициализация FastAPI
app = FastAPI(title="Дос — Kazakh TTS API", version="1.0.0")

# Глобальные переменные модели
_model = None
if torch.cuda.is_available():
    _device = "cuda"
    _dtype = torch.float16
elif torch.backends.mps.is_available():
    _device = "mps"
    _dtype = torch.float16
else:
    _device = "cpu"
    _dtype = torch.float32
_voice_prompts = {}
_model_lock = threading.Lock()
_playback_lock = threading.Lock()
_current_play_proc = None
_load_error = None

class TTSRequest(BaseModel):
    text: str
    voice: Optional[str] = "male"  # "male" или "female"
    play_now: Optional[bool] = False

class TTSResponse(BaseModel):
    status: str
    text: str
    audio_path: str
    audio_url: Optional[str] = None
    duration_sec: float
    synth_time_sec: float
    cached: bool

def _get_cache_path(text: str, voice: str) -> str:
    key = hashlib.md5(f"omnivoice|{voice}|{text.strip()}".encode("utf-8")).hexdigest()
    return os.path.join(CACHE_DIR, f"{key}.wav")

def _play_audio_file(file_path: str):
    global _current_play_proc
    with _playback_lock:
        if _current_play_proc is not None and _current_play_proc.poll() is None:
            _current_play_proc.terminate()
        _current_play_proc = subprocess.Popen(["afplay", file_path])

def load_tts_model():
    global _model, _voice_prompts, _load_error
    if _model is not None:
        return
    
    with _model_lock:
        if _model is not None:
            return
        print(f"[TTS API] Загрузка OmniVoice ({_device}, {_dtype})...")
        t0 = time.time()
        try:
            from omnivoice import OmniVoice
            _model = OmniVoice.from_pretrained(
                MODEL_DIR,
                device_map=_device,
                dtype=_dtype
            )
        except Exception as e:
            # Запоминаем ошибку (например, OOM на 8 ГБ), чтобы не пытаться грузить модель на каждый запрос
            _load_error = f"{type(e).__name__}: {str(e)[:200]}"
            _model = None
            print(f"[TTS API] Не удалось загрузить OmniVoice: {_load_error}")
            return
        _load_error = None
        print(f"[TTS API] Модель загружена за {time.time() - t0:.2f} сек.")

        # Предварительная сборка векторов клонирования голосов
        samples_dir = os.path.join(PROJECT_DIR, "audio_samples")
        male_ref = os.path.join(samples_dir, "omnivoice_male.wav")
        female_ref = os.path.join(samples_dir, "omnivoice_female.wav")

        if os.path.exists(male_ref):
            print("[TTS API] Инициализация мужского голоса Доса (male)...")
            _voice_prompts["male"] = _model.create_voice_clone_prompt(
                ref_audio=male_ref,
                ref_text="Қазақстанда жасанды интеллект пен сөйлеу технологиялары қарқынды дамып келеді."
            )
        
        if os.path.exists(female_ref):
            print("[TTS API] Инициализация женского голоса (female)...")
            _voice_prompts["female"] = _model.create_voice_clone_prompt(
                ref_audio=female_ref,
                ref_text="Қайырлы күн! Бүгін біз қазақ тіліндегі мәтінді дыбыстау сапасын тексеріп жатырмыз."
            )

@app.on_event("startup")
def startup_event():
    threading.Thread(target=load_tts_model, daemon=True).start()

@app.get("/audio/{filename}")
def get_audio_file(filename: str):
    safe_name = os.path.basename(filename)
    path = os.path.join(CACHE_DIR, safe_name)
    if os.path.exists(path):
        return FileResponse(path, media_type="audio/wav")
    raise HTTPException(status_code=404, detail="Audio file not found")

@app.get("/health")
def health():
    return {
        "status": "ready" if _model is not None else ("failed" if _load_error else "loading"),
        "error": _load_error,
        "device": _device,
        "dtype": str(_dtype),
        "voices_loaded": list(_voice_prompts.keys()),
        "cache_dir": CACHE_DIR
    }

@app.post("/synthesize", response_model=TTSResponse)
def synthesize(req: TTSRequest, background_tasks: BackgroundTasks):
    text = req.text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="Текст не может быть пустым.")

    cached_file = _get_cache_path(text, req.voice)
    if os.path.exists(cached_file) and os.path.getsize(cached_file) > 1024:
        # Аудио уже в кэше
        info = sf.info(cached_file)
        if req.play_now:
            background_tasks.add_task(_play_audio_file, cached_file)
        return TTSResponse(
            status="ok",
            text=text,
            audio_path=cached_file,
            audio_url=f"/audio/{os.path.basename(cached_file)}",
            duration_sec=info.duration,
            synth_time_sec=0.0,
            cached=True
        )

    # Модель не загрузилась (например, нехватка памяти) — отвечаем сразу, без повторной загрузки
    if _model is None:
        raise HTTPException(status_code=503, detail=f"OmniVoice не готов: {_load_error or 'загрузка'}")

    voice_key = req.voice if req.voice in _voice_prompts else "male"
    prompt = _voice_prompts.get(voice_key)

    t0 = time.time()
    with _model_lock:
        try:
            from omnivoice.models.omnivoice import OmniVoiceGenerationConfig
            gen_cfg = OmniVoiceGenerationConfig(num_step=12)
        except Exception:
            gen_cfg = None

        if prompt is not None:
            audio = _model.generate(
                text=text,
                language="Kazakh",
                voice_clone_prompt=prompt,
                generation_config=gen_cfg
            )
        else:
            audio = _model.generate(
                text=text,
                language="Kazakh",
                generation_config=gen_cfg
            )

    synth_time = time.time() - t0
    audio_arr = audio[0]
    if hasattr(audio_arr, "detach"):
        audio_arr = audio_arr.detach().cpu().numpy()

    # Сохраняем в кэш
    sf.write(cached_file, audio_arr, 24000)
    dur_sec = len(audio_arr) / 24000.0

    if req.play_now:
        background_tasks.add_task(_play_audio_file, cached_file)

    return TTSResponse(
        status="ok",
        text=text,
        audio_path=cached_file,
        audio_url=f"/audio/{os.path.basename(cached_file)}",
        duration_sec=dur_sec,
        synth_time_sec=synth_time,
        cached=False
    )

if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=8001)

