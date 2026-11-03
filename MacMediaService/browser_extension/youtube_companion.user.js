// ==UserScript==
// @name         Media Companion for ESP32 Voice Assistant (YouTube & Yandex Music)
// @namespace    http://tampermonkey.net/
// @version      2.1
// @description  Безопасная трансляция статуса воспроизведения YouTube и Яндекс Музыки для ESP32-S3
// @match        https://www.youtube.com/*
// @match        https://music.youtube.com/*
// @match        https://music.yandex.ru/*
// @match        https://music.yandex.com/*
// @match        https://music.yandex.kz/*
// @match        https://music.yandex.by/*
// @grant        GM_xmlhttpRequest
// @grant        unsafeWindow
// @connect      127.0.0.1
// @run-at       document-idle
// ==/UserScript==

(function() {
    'use strict';

    const BRIDGE_URL_UPDATE = 'http://127.0.0.1:8766/api/browser_update';
    const BRIDGE_URL_CMDS   = 'http://127.0.0.1:8766/api/browser_commands';

    let lastSentState = "";
    let wasPlaying = false;

    function cleanTitle(raw) {
        if (!raw) return "";
        return raw.replace(/^\(\d+\)\s*/, '').trim();
    }

    // ------------------------------------------------------------------------
    // 1. YouTube Data Fetcher
    // ------------------------------------------------------------------------
    function getYouTubeData() {
        const video = document.querySelector('video.html5-main-video') || document.querySelector('video');
        if (!video) return null;

        let title = "";
        const titleEl = document.querySelector('h1.ytd-watch-metadata yt-formatted-string') ||
                        document.querySelector('#title h1 yt-formatted-string') ||
                        document.querySelector('.ytmusic-player-bar .title');
        if (titleEl && titleEl.innerText.trim()) {
            title = cleanTitle(titleEl.innerText.trim());
        } else if (navigator.mediaSession && navigator.mediaSession.metadata && navigator.mediaSession.metadata.title) {
            title = cleanTitle(navigator.mediaSession.metadata.title);
        } else {
            title = cleanTitle(document.title.replace(' - YouTube Music', '').replace(' - YouTube', ''));
        }

        let channel = "";
        const channelEl = document.querySelector('#channel-name a') ||
                          document.querySelector('ytd-channel-name yt-formatted-string') ||
                          document.querySelector('.ytmusic-player-bar .byline a');
        if (channelEl && channelEl.innerText.trim()) {
            channel = channelEl.innerText.trim();
        } else if (navigator.mediaSession && navigator.mediaSession.metadata && navigator.mediaSession.metadata.artist) {
            channel = navigator.mediaSession.metadata.artist;
        }

        const duration = Math.round(video.duration || 0);
        const position = Math.round(video.currentTime || 0);
        const state = video.paused ? 'paused' : 'playing';

        return {
            title: title,
            artist: channel,
            source: "YouTube",
            position: position,
            duration: duration,
            state: state
        };
    }

    // ------------------------------------------------------------------------
    // 2. Yandex Music Data Fetcher (Supports both v5 Next.js and classic UI)
    // ------------------------------------------------------------------------
    function getYandexMusicData() {
        const pageWin = (typeof unsafeWindow !== 'undefined') ? unsafeWindow : window;
        const extApi = pageWin.externalAPI;
        if (extApi && typeof extApi.getCurrentTrack === 'function') {
            const track = extApi.getCurrentTrack();
            if (track && track.title) {
                const artists = track.artists ? track.artists.map(a => a.name).join(', ') : "";
                const prog = extApi.getProgress ? extApi.getProgress() : { position: 0, duration: 0 };
                const isPlaying = extApi.isPlaying ? extApi.isPlaying() : false;

                return {
                    title: cleanTitle(track.title),
                    artist: artists,
                    source: "Яндекс Музыка",
                    position: Math.round(prog.position || 0),
                    duration: Math.round(prog.duration || 0),
                    state: isPlaying ? "playing" : "paused"
                };
            }
        }

        // MediaSession + HTML5 Audio (новый интерфейс Яндекс Музыки music.yandex.kz / .ru / .com)
        const audio = document.querySelector('audio');
        if (navigator.mediaSession && navigator.mediaSession.metadata && navigator.mediaSession.metadata.title) {
            const meta = navigator.mediaSession.metadata;
            const isPlaying = audio ? !audio.paused : (navigator.mediaSession.playbackState === 'playing');
            return {
                title: cleanTitle(meta.title),
                artist: meta.artist || "Яндекс Музыка",
                source: "Яндекс Музыка",
                position: audio ? Math.round(audio.currentTime || 0) : 0,
                duration: audio ? Math.round(audio.duration || 0) : 0,
                state: isPlaying ? "playing" : "paused"
            };
        }

        // Резервный DOM-парсинг (новый и старый плеер)
        const titleEl = document.querySelector('[class*="PlayerBarTitle_title"]') ||
                        document.querySelector('[class*="Meta_title"]') ||
                        document.querySelector('.track__title') ||
                        document.querySelector('.d-track__title');
        const artistEl = document.querySelector('[class*="SeparatedArtists"]') ||
                         document.querySelector('[class*="Meta_artists"]') ||
                         document.querySelector('.track__artists') ||
                         document.querySelector('.d-track__artists');

        if (titleEl && titleEl.innerText.trim()) {
            const title = cleanTitle(titleEl.innerText.trim());
            const artist = artistEl ? artistEl.innerText.trim() : "Яндекс Музыка";
            const isPlaying = audio ? !audio.paused : false;

            return {
                title: title,
                artist: artist,
                source: "Яндекс Музыка",
                position: audio ? Math.round(audio.currentTime || 0) : 0,
                duration: audio ? Math.round(audio.duration || 0) : 0,
                state: isPlaying ? "playing" : "paused"
            };
        }

        return null;
    }

    function getMediaData() {
        const host = window.location.hostname;
        if (host.includes('youtube.com')) {
            return getYouTubeData();
        } else if (host.includes('music.yandex')) {
            return getYandexMusicData();
        }
        return null;
    }

    function reportState() {
        const data = getMediaData();
        if (!data || !data.title) return;

        // Не перебиваем активную вкладку данными фоновой вкладки, которая давно на паузе
        if (data.state !== 'playing' && !wasPlaying && document.hidden) {
            return;
        }
        wasPlaying = (data.state === 'playing');

        const signature = `${data.title}|${data.artist}|${data.state}|${data.position}|${data.duration}`;
        if (signature === lastSentState) return;
        lastSentState = signature;

        const body = JSON.stringify(data);

        if (typeof GM_xmlhttpRequest !== 'undefined') {
            GM_xmlhttpRequest({
                method: "POST",
                url: BRIDGE_URL_UPDATE,
                headers: { "Content-Type": "application/json" },
                data: body
            });
        } else {
            fetch(BRIDGE_URL_UPDATE, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: body
            }).catch(() => {});
        }
    }

    function pollCommands() {
        const handler = (cmds) => {
            if (!Array.isArray(cmds) || cmds.length === 0) return;
            const host = window.location.hostname;
            const pageWin = (typeof unsafeWindow !== 'undefined') ? unsafeWindow : window;

            for (const item of cmds) {
                const cmd = item.command;
                const val = item.value;

                if (host.includes('music.yandex')) {
                    const api = pageWin.externalAPI;
                    const audio = document.querySelector('audio');
                    if (api && typeof api.togglePause === 'function') {
                        if (cmd === 'PLAY_PAUSE') api.togglePause();
                        else if (cmd === 'PLAY') { if (!api.isPlaying()) api.togglePause(); }
                        else if (cmd === 'PAUSE') { if (api.isPlaying()) api.togglePause(); }
                        else if (cmd === 'NEXT_TRACK') api.next();
                        else if (cmd === 'PREVIOUS_TRACK') api.prev();
                        else if (cmd === 'SEEK_TO' && typeof val === 'number') api.setPosition(val);
                        else if (cmd === 'SEEK_FORWARD') api.setPosition((api.getProgress().position || 0) + (val || 10));
                        else if (cmd === 'SEEK_BACKWARD') api.setPosition(Math.max(0, (api.getProgress().position || 0) - (val || 10)));
                    } else if (audio) {
                        if (cmd === 'PLAY_PAUSE') { if (audio.paused) audio.play(); else audio.pause(); }
                        else if (cmd === 'SEEK_TO' && typeof val === 'number') audio.currentTime = val;
                        else if (cmd === 'SEEK_FORWARD') audio.currentTime = Math.min(audio.duration || 9999, audio.currentTime + (val || 10));
                        else if (cmd === 'SEEK_BACKWARD') audio.currentTime = Math.max(0, audio.currentTime - (val || 10));
                    }
                } else {
                    const video = document.querySelector('video.html5-main-video') || document.querySelector('video');
                    if (!video) continue;

                    if (cmd === 'PLAY_PAUSE') {
                        if (video.paused) video.play();
                        else video.pause();
                    } else if (cmd === 'PLAY') {
                        video.play();
                    } else if (cmd === 'PAUSE') {
                        video.pause();
                    } else if (cmd === 'SEEK_TO') {
                        if (typeof val === 'number') video.currentTime = val;
                    } else if (cmd === 'SEEK_FORWARD') {
                        video.currentTime = Math.min(video.duration, video.currentTime + (val || 10));
                    } else if (cmd === 'SEEK_BACKWARD') {
                        video.currentTime = Math.max(0, video.currentTime - (val || 10));
                    } else if (cmd === 'NEXT_TRACK') {
                        const nextBtn = document.querySelector('.ytp-next-button') || document.querySelector('.next-button');
                        if (nextBtn) nextBtn.click();
                    } else if (cmd === 'PREVIOUS_TRACK') {
                        const prevBtn = document.querySelector('.ytp-prev-button') || document.querySelector('.previous-button');
                        if (prevBtn) prevBtn.click();
                        else video.currentTime = 0;
                    }
                }
            }
            setTimeout(reportState, 80);
        };

        if (typeof GM_xmlhttpRequest !== 'undefined') {
            GM_xmlhttpRequest({
                method: "GET",
                url: BRIDGE_URL_CMDS,
                onload: (res) => {
                    try {
                        handler(JSON.parse(res.responseText));
                    } catch(e) {}
                }
            });
        } else {
            fetch(BRIDGE_URL_CMDS)
                .then(r => r.json())
                .then(handler)
                .catch(() => {});
        }
    }

    setInterval(reportState, 450);
    setInterval(pollCommands, 400);
})();
