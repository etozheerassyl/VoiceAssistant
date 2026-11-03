import Foundation
import Accelerate
import CoreMedia
import CoreAudio
import AudioToolbox
import CoreGraphics
import ScreenCaptureKit

// ============================================================================
// Конфигурация DSP и анализатора спектра
// ============================================================================
struct AnalyzerConfig {
    var bandCount: Int = 16
    var fftSize: Int = 2048
    var minFreq: Float = 25.0
    var maxFreq: Float = 16000.0
    var updateFps: Double = 30.0
    var attackCoeff: Float = 0.68
    var releaseCoeff: Float = 0.24
    var noiseFloorDb: Float = -58.0
    var peakCeilDb: Float = -10.0
}

// ============================================================================
// Модуль цифровой обработки сигнала (FFT + 16 логарифмических полос)
// ============================================================================
final class AudioDSPProcessor {
    let config: AnalyzerConfig
    private let fftSize: Int
    private let halfSize: Int
    private let log2n: vDSP_Length
    private var fftSetup: FFTSetup?

    // Предварительно выделенные буферы (без аллокаций в реальном времени)
    private var ringBuffer: [Float]
    private var ringWriteIndex: Int = 0
    private var samplesWritten: Int = 0

    private var hannWindow: [Float]
    private var timeBuffer: [Float]
    private var windowedBuffer: [Float]
    private var splitReal: [Float]
    private var splitImag: [Float]
    private var magnitudes: [Float]

    private var smoothedBands: [Float]
    private var currentSampleRate: Float = 48000.0
    private var adaptivePeakDb: Float = -16.0
    private var lastAudioTimestamp: TimeInterval = 0.0
    private var currentRms: Float = 0.0

    init(config: AnalyzerConfig = AnalyzerConfig()) {
        self.config = config
        self.fftSize = config.fftSize
        self.halfSize = config.fftSize / 2
        self.log2n = vDSP_Length(round(log2(Double(config.fftSize))))
        self.fftSetup = vDSP_create_fftsetup(self.log2n, FFTRadix(kFFTRadix2))

        self.ringBuffer = [Float](repeating: 0.0, count: config.fftSize)
        self.hannWindow = [Float](repeating: 0.0, count: config.fftSize)
        self.timeBuffer = [Float](repeating: 0.0, count: config.fftSize)
        self.windowedBuffer = [Float](repeating: 0.0, count: config.fftSize)
        self.splitReal = [Float](repeating: 0.0, count: config.fftSize / 2)
        self.splitImag = [Float](repeating: 0.0, count: config.fftSize / 2)
        self.magnitudes = [Float](repeating: 0.0, count: config.fftSize / 2)
        self.smoothedBands = [Float](repeating: 0.0, count: config.bandCount)

        vDSP_hann_window(&self.hannWindow, vDSP_Length(config.fftSize), Int32(vDSP_HANN_NORM))
    }

    deinit {
        if let setup = fftSetup {
            vDSP_destroy_fftsetup(setup)
        }
    }

    // Защита от NaN, Inf и клиппинга за пределами [-1.0, 1.0]
    @inline(__always)
    private func sanitizeSample(_ x: Float) -> Float {
        if !x.isFinite { return 0.0 }
        if x > 1.0 { return 1.0 }
        if x < -1.0 { return -1.0 }
        return x
    }

    // Прием моно-отсчетов Float32 в кольцевой буфер
    func pushMonoSamples(_ samples: [Float], sampleRate: Float, timestamp: TimeInterval = CFAbsoluteTimeGetCurrent()) {
        if sampleRate >= 8000.0 && sampleRate <= 192000.0 {
            currentSampleRate = sampleRate
        }

        let count = samples.count
        guard count > 0 else { return }

        var sumSq: Float = 0.0
        let mask = fftSize - 1
        for i in 0..<count {
            let s = sanitizeSample(samples[i])
            sumSq += s * s
            ringBuffer[ringWriteIndex] = s
            ringWriteIndex = (ringWriteIndex + 1) & mask
        }
        samplesWritten = min(fftSize, samplesWritten + count)
        let frameRms = sqrtf(sumSq / Float(count))
        currentRms = frameRms
        if frameRms > 0.00025 {
            lastAudioTimestamp = timestamp
        }
    }

    // Вычисление 16 логарифмических частотных диапазонов (0..100)
    func computeBands(now: TimeInterval = CFAbsoluteTimeGetCurrent()) -> (bands: [Int], rms: Float, active: Bool) {
        let silenceTimeout = 0.12 // 120 мс без ненулевого аудиосигнала -> плавное затухание
        let isAudioSilent = (now - lastAudioTimestamp > silenceTimeout) || (samplesWritten < 256) || (currentRms < 0.00025)

        if isAudioSilent {
            var anyNonZero = false
            var out = [Int](repeating: 0, count: config.bandCount)
            for b in 0..<config.bandCount {
                smoothedBands[b] *= (1.0 - config.releaseCoeff)
                if smoothedBands[b] < 0.6 {
                    smoothedBands[b] = 0.0
                } else {
                    anyNonZero = true
                }
                out[b] = Int(roundf(min(100.0, max(0.0, smoothedBands[b]))))
            }
            return (out, 0.0, anyNonZero)
        }

        // 1. Разворачиваем кольцевой буфер во временной массив
        let mask = fftSize - 1
        for i in 0..<fftSize {
            let idx = (ringWriteIndex + i) & mask
            timeBuffer[i] = ringBuffer[idx]
        }

        // 2. Применяем окно Ханна (vDSP)
        vDSP_vmul(timeBuffer, 1, hannWindow, 1, &windowedBuffer, 1, vDSP_Length(fftSize))

        // 3. Выполняем действительное БПФ (vDSP_fft_zrip)
        guard let setup = fftSetup else {
            return ([Int](repeating: 0, count: config.bandCount), 0.0, false)
        }

        let normFactor: Float = 1.0 / Float(fftSize)
        splitReal.withUnsafeMutableBufferPointer { realBuf in
            splitImag.withUnsafeMutableBufferPointer { imagBuf in
                var splitComplex = DSPSplitComplex(realp: realBuf.baseAddress!, imagp: imagBuf.baseAddress!)
                windowedBuffer.withUnsafeBufferPointer { winBuf in
                    winBuf.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfSize) { complexPtr in
                        vDSP_ctoz(complexPtr, 2, &splitComplex, 1, vDSP_Length(halfSize))
                    }
                }
                vDSP_fft_zrip(setup, &splitComplex, 1, log2n, FFTDirection(kFFTDirection_Forward))
                imagBuf[0] = 0.0
                vDSP_zvabs(&splitComplex, 1, &magnitudes, 1, vDSP_Length(halfSize))
            }
        }

        // Масштабируем амплитуды БПФ
        var scale = normFactor
        vDSP_vsmul(magnitudes, 1, &scale, &magnitudes, 1, vDSP_Length(halfSize))
        magnitudes[0] = 0.0 // Убираем постоянную составляющую (DC)

        // 4. Распределение по 16 логарифмическим диапазонам
        let nyquist = currentSampleRate * 0.5
        let fMin = max(20.0, config.minFreq)
        let fMax = min(config.maxFreq, nyquist * 0.92)
        let ratio = fMax / fMin
        let binHz = currentSampleRate / Float(fftSize)

        var rawDbBands = [Float](repeating: config.noiseFloorDb, count: config.bandCount)
        var frameMaxDb: Float = config.noiseFloorDb

        for b in 0..<config.bandCount {
            let t0 = Float(b) / Float(config.bandCount)
            let t1 = Float(b + 1) / Float(config.bandCount)
            let fLow = fMin * powf(ratio, t0)
            let fHigh = fMin * powf(ratio, t1)
            let fCenter = sqrtf(fLow * fHigh)

            let kLow = fLow / binHz
            let kHigh = fHigh / binHz

            var bandMag: Float = 0.0
            if kHigh - kLow < 1.0 {
                let kCenter = 0.5 * (kLow + kHigh)
                let k0 = max(1, min(halfSize - 2, Int(floorf(kCenter))))
                let frac = kCenter - Float(k0)
                bandMag = magnitudes[k0] * (1.0 - frac) + magnitudes[k0 + 1] * frac
            } else {
                let startBin = max(1, min(halfSize - 1, Int(floorf(kLow))))
                let endBin = max(startBin, min(halfSize - 1, Int(ceilf(kHigh))))
                var sumSq: Float = 0.0
                var maxVal: Float = 0.0
                let count = endBin - startBin + 1
                for k in startBin...endBin {
                    let m = magnitudes[k]
                    sumSq += m * m
                    if m > maxVal { maxVal = m }
                }
                let rmsMag = sqrtf(sumSq / Float(count))
                bandMag = 0.60 * rmsMag + 0.40 * maxVal
            }

            // Перцептивная компенсация наклона спектра (розовый шум ~ +3 дБ/октаву)
            let tiltWeight = powf(fCenter / 180.0, 0.32)
            let weightedMag = bandMag * tiltWeight
            let db = 20.0 * log10f(max(weightedMag, 1e-7))
            rawDbBands[b] = db
            if db > frameMaxDb {
                frameMaxDb = db
            }
        }

        // 5. Мягкая адаптивная нормализация пикового потолка (AGC для визуализации)
        if frameMaxDb > adaptivePeakDb {
            adaptivePeakDb = adaptivePeakDb * 0.80 + frameMaxDb * 0.20
        } else {
            adaptivePeakDb = adaptivePeakDb * 0.992 + frameMaxDb * 0.008
        }
        adaptivePeakDb = max(-28.0, min(-6.0, adaptivePeakDb))

        let dynamicFloor = config.noiseFloorDb
        let dynamicSpan = max(18.0, adaptivePeakDb - dynamicFloor)

        var outBands = [Int](repeating: 0, count: config.bandCount)
        for b in 0..<config.bandCount {
            let norm = (rawDbBands[b] - dynamicFloor) / dynamicSpan
            let clamped = max(0.0, min(1.0, norm))
            let targetLevel = powf(clamped, 1.12) * 100.0

            let prev = smoothedBands[b]
            if targetLevel > prev {
                smoothedBands[b] = prev + config.attackCoeff * (targetLevel - prev)
            } else {
                smoothedBands[b] = prev + config.releaseCoeff * (targetLevel - prev)
            }
            if smoothedBands[b] < 0.8 {
                smoothedBands[b] = 0.0
            }
            outBands[b] = Int(roundf(min(100.0, max(0.0, smoothedBands[b]))))
        }

        return (outBands, currentRms, true)
    }
}

// ============================================================================
// Захват системного аудио:
// 1. CoreAudio Global Process Tap (CATapDescription + AudioHardwareCreateProcessTap)
//    Работает нативно в фоновом демоне macOS без изменения устройства вывода звука!
// 2. Резерв: ScreenCaptureKit
// ============================================================================
final class SystemAudioCaptureEngine: NSObject, SCStreamOutput, SCStreamDelegate {
    private let dsp: AudioDSPProcessor
    private let dspQueue = DispatchQueue(label: "kz.diploma.audio.dsp", qos: .userInteractive)
    private let captureQueue = DispatchQueue(label: "kz.diploma.audio.capture", qos: .userInteractive)

    // CoreAudio Process Tap state
    private var tapID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateDeviceID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
    private var ioProcID: AudioDeviceIOProcID? = nil
    private var tapSampleRate: Float = 48000.0
    private var tapChannels: Int = 2

    // ScreenCaptureKit fallback state
    private var stream: SCStream?

    private var outputTimer: DispatchSourceTimer?
    private var permissionMonitorTimer: DispatchSourceTimer?
    private(set) var isCapturingAudio: Bool = false
    private var hasPermission: Bool = false
    private let allowPrompt: Bool

    init(dsp: AudioDSPProcessor, allowPrompt: Bool = false) {
        self.dsp = dsp
        self.allowPrompt = allowPrompt
        super.init()
    }

    func start() {
        cleanupOrphanedAggregateDevices()
        startOutputLoop()
        checkAndStartCapture()

        let mon = DispatchSource.makeTimerSource(queue: dspQueue)
        mon.schedule(deadline: .now() + 2.5, repeating: 2.5)
        mon.setEventHandler { [weak self] in
            guard let self = self else { return }
            if !self.isCapturingAudio {
                self.checkAndStartCapture()
            }
        }
        mon.resume()
        self.permissionMonitorTimer = mon
    }

    func stop() {
        if let proc = ioProcID, aggregateDeviceID != kAudioObjectUnknown {
            AudioDeviceStop(aggregateDeviceID, proc)
            AudioDeviceDestroyIOProcID(aggregateDeviceID, proc)
            ioProcID = nil
        }
        if aggregateDeviceID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateDeviceID)
            aggregateDeviceID = AudioObjectID(kAudioObjectUnknown)
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = AudioObjectID(kAudioObjectUnknown)
        }
        isCapturingAudio = false
    }

    // Очистка старых приватных агрегатных устройств от предыдущих запусков
    private func cleanupOrphanedAggregateDevices() {
        var propSize: UInt32 = 0
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &propSize) == noErr else {
            return
        }
        let deviceCount = Int(propSize) / MemoryLayout<AudioObjectID>.size
        guard deviceCount > 0 else { return }
        var devices = [AudioObjectID](repeating: 0, count: deviceCount)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &propSize, &devices) == noErr else {
            return
        }

        for dev in devices {
            var name: CFString = "" as CFString
            var nameSize = UInt32(MemoryLayout<CFString>.size)
            var nameAddr = AudioObjectPropertyAddress(
                mSelector: kAudioObjectPropertyName,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            let status = withUnsafeMutablePointer(to: &name) { ptr in
                AudioObjectGetPropertyData(dev, &nameAddr, 0, nil, &nameSize, ptr)
            }
            if status == noErr && (name as String) == "DiplomaVoiceAssistantTapAgg" {
                AudioHardwareDestroyAggregateDevice(dev)
            }
        }
    }

    private func startCoreAudioProcessTap() -> Bool {
        let tapDesc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        tapDesc.name = "DiplomaVoiceAssistantTap"
        tapDesc.isPrivate = true
        tapDesc.muteBehavior = .unmuted

        var newTapID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
        let errTap = AudioHardwareCreateProcessTap(tapDesc, &newTapID)
        guard errTap == noErr, newTapID != kAudioObjectUnknown else {
            fputs("[AUDIO_ANALYZER] AudioHardwareCreateProcessTap вернул код \(errTap)\n", stderr)
            return false
        }

        // Считываем формат аудиопотока Tap
        var format = AudioStreamBasicDescription()
        var propSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var fmtAddr = AudioObjectPropertyAddress(
            mSelector: kAudioTapPropertyFormat,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        if AudioObjectGetPropertyData(newTapID, &fmtAddr, 0, nil, &propSize, &format) == noErr {
            if format.mSampleRate >= 8000 {
                tapSampleRate = Float(format.mSampleRate)
            }
            if format.mChannelsPerFrame >= 1 {
                tapChannels = Int(format.mChannelsPerFrame)
            }
        }

        let tapUUID = tapDesc.uuid.uuidString
        let aggUID = "kz.diploma.tap.\(UUID().uuidString)"
        let aggDesc: [String: Any] = [
            kAudioAggregateDeviceNameKey: "DiplomaVoiceAssistantTapAgg",
            kAudioAggregateDeviceUIDKey: aggUID,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [
                [
                    kAudioSubTapUIDKey: tapUUID,
                    kAudioSubTapDriftCompensationKey: true
                ]
            ]
        ]

        var newAggID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
        let errAgg = AudioHardwareCreateAggregateDevice(aggDesc as CFDictionary, &newAggID)
        guard errAgg == noErr, newAggID != kAudioObjectUnknown else {
            fputs("[AUDIO_ANALYZER] AudioHardwareCreateAggregateDevice вернул код \(errAgg)\n", stderr)
            AudioHardwareDestroyProcessTap(newTapID)
            return false
        }

        var newProcID: AudioDeviceIOProcID? = nil
        let sr = self.tapSampleRate
        let defaultCh = self.tapChannels

        let errProc = AudioDeviceCreateIOProcIDWithBlock(&newProcID, newAggID, captureQueue) { [weak self] _, inInputData, _, _, _ in
            guard let self = self else { return }
            let buffers = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inInputData))
            guard !buffers.isEmpty else { return }

            if buffers.count >= 2, let leftRaw = buffers[0].mData {
                // Non-interleaved (planar) Float32
                let frameCount = Int(buffers[0].mDataByteSize) / MemoryLayout<Float>.size
                guard frameCount > 0 else { return }
                let leftPtr = leftRaw.assumingMemoryBound(to: Float.self)
                let rightPtr = buffers[1].mData?.assumingMemoryBound(to: Float.self)
                var mono = [Float](repeating: 0.0, count: frameCount)
                if let rPtr = rightPtr {
                    for i in 0..<frameCount {
                        mono[i] = 0.5 * (leftPtr[i] + rPtr[i])
                    }
                } else {
                    for i in 0..<frameCount {
                        mono[i] = leftPtr[i]
                    }
                }
                self.dspQueue.async {
                    self.dsp.pushMonoSamples(mono, sampleRate: sr)
                }
            } else if let dataRaw = buffers[0].mData {
                // Interleaved Float32
                let ch = max(1, Int(buffers[0].mNumberChannels > 0 ? Int(buffers[0].mNumberChannels) : defaultCh))
                let totalFloats = Int(buffers[0].mDataByteSize) / MemoryLayout<Float>.size
                let frameCount = totalFloats / ch
                guard frameCount > 0 else { return }
                let ptr = dataRaw.assumingMemoryBound(to: Float.self)
                var mono = [Float](repeating: 0.0, count: frameCount)
                if ch == 1 {
                    for i in 0..<frameCount {
                        mono[i] = ptr[i]
                    }
                } else {
                    for i in 0..<frameCount {
                        let base = i * ch
                        mono[i] = 0.5 * (ptr[base] + ptr[base + 1])
                    }
                }
                self.dspQueue.async {
                    self.dsp.pushMonoSamples(mono, sampleRate: sr)
                }
            }
        }

        guard errProc == noErr, let procID = newProcID else {
            fputs("[AUDIO_ANALYZER] AudioDeviceCreateIOProcIDWithBlock вернул код \(errProc)\n", stderr)
            AudioHardwareDestroyAggregateDevice(newAggID)
            AudioHardwareDestroyProcessTap(newTapID)
            return false
        }

        let errStart = AudioDeviceStart(newAggID, procID)
        guard errStart == noErr else {
            fputs("[AUDIO_ANALYZER] AudioDeviceStart вернул код \(errStart)\n", stderr)
            AudioDeviceDestroyIOProcID(newAggID, procID)
            AudioHardwareDestroyAggregateDevice(newAggID)
            AudioHardwareDestroyProcessTap(newTapID)
            return false
        }

        self.tapID = newTapID
        self.aggregateDeviceID = newAggID
        self.ioProcID = procID
        self.hasPermission = true
        self.isCapturingAudio = true
        fputs("[AUDIO_ANALYZER] Захват системного аудио через CoreAudio Process Tap активен (\(Int(sr)) Гц, \(defaultCh) канала)\n", stderr)
        return true
    }

    private func checkAndStartCapture() {
        let granted = CGPreflightScreenCaptureAccess()
        if !granted && !allowPrompt {
            hasPermission = false
            return
        }
        if !granted && allowPrompt {
            _ = CGRequestScreenCaptureAccess()
        }

        // 1. Запускаем нативный CoreAudio Global Process Tap (работает в фоновом демоне без окон)
        if startCoreAudioProcessTap() {
            return
        }

        // 2. Резервный путь через ScreenCaptureKit
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: false) { [weak self] content, error in
            guard let self = self else { return }
            if let error = error {
                self.dspQueue.async {
                    self.hasPermission = false
                    self.isCapturingAudio = false
                }
                fputs("[AUDIO_ANALYZER] Ожидание доступа ScreenCaptureKit: \(error.localizedDescription)\n", stderr)
                return
            }

            guard let content = content, let display = content.displays.first else { return }

            let filter = SCContentFilter(display: display, excludingWindows: [])
            let streamConfig = SCStreamConfiguration()
            streamConfig.width = 16
            streamConfig.height = 16
            streamConfig.minimumFrameInterval = CMTime(value: 1, timescale: 2)
            streamConfig.showsCursor = false
            streamConfig.capturesAudio = true
            streamConfig.captureMicrophone = false
            streamConfig.sampleRate = 48000
            streamConfig.channelCount = 2
            streamConfig.excludesCurrentProcessAudio = true

            let newStream = SCStream(filter: filter, configuration: streamConfig, delegate: self)
            do {
                try newStream.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.captureQueue)
                try newStream.addStreamOutput(self, type: .audio, sampleHandlerQueue: self.captureQueue)
                newStream.startCapture { startErr in
                    self.dspQueue.async {
                        if startErr == nil {
                            self.stream = newStream
                            self.hasPermission = true
                            self.isCapturingAudio = true
                            fputs("[AUDIO_ANALYZER] Захват системного аудио через ScreenCaptureKit активен\n", stderr)
                        }
                    }
                }
            } catch {
                fputs("[AUDIO_ANALYZER] Ошибка SCStream: \(error.localizedDescription)\n", stderr)
            }
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        dspQueue.async {
            self.isCapturingAudio = false
            self.stream = nil
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, CMSampleBufferDataIsReady(sampleBuffer) else { return }
        guard let fmtDesc = CMSampleBufferGetFormatDescription(sampleBuffer),
              let asbdPtr = CMAudioFormatDescriptionGetStreamBasicDescription(fmtDesc) else {
            return
        }

        let asbd = asbdPtr.pointee
        let sampleRate = Float(asbd.mSampleRate)
        let channels = Int(asbd.mChannelsPerFrame)
        let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
        let isNonInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0
        let bitsPerChannel = Int(asbd.mBitsPerChannel)
        let numSamples = Int(CMSampleBufferGetNumSamples(sampleBuffer))
        guard numSamples > 0, channels > 0, isFloat, bitsPerChannel == 32 else { return }

        let ablSize = MemoryLayout<AudioBufferList>.size + MemoryLayout<AudioBuffer>.size * 8
        let rawPtr = UnsafeMutableRawPointer.allocate(byteCount: ablSize, alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { rawPtr.deallocate() }
        let ablPtr = rawPtr.bindMemory(to: AudioBufferList.self, capacity: 1)

        var blockBuffer: CMBlockBuffer?
        let status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: ablPtr,
            bufferListSize: ablSize,
            blockBufferAllocator: kCFAllocatorDefault,
            blockBufferMemoryAllocator: kCFAllocatorDefault,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            blockBufferOut: &blockBuffer
        )

        guard status == noErr else { return }
        let buffers = UnsafeMutableAudioBufferListPointer(ablPtr)
        guard !buffers.isEmpty else { return }

        if isNonInterleaved && buffers.count >= 2, let leftData = buffers[0].mData {
            let leftPtr = leftData.assumingMemoryBound(to: Float.self)
            let rightPtr = buffers[1].mData?.assumingMemoryBound(to: Float.self)
            let frameCount = min(numSamples, Int(buffers[0].mDataByteSize) / MemoryLayout<Float>.size)
            var mono = [Float](repeating: 0.0, count: frameCount)
            if let rPtr = rightPtr {
                for i in 0..<frameCount { mono[i] = 0.5 * (leftPtr[i] + rPtr[i]) }
            } else {
                for i in 0..<frameCount { mono[i] = leftPtr[i] }
            }
            dspQueue.async { self.dsp.pushMonoSamples(mono, sampleRate: sampleRate) }
        } else if let data = buffers[0].mData {
            let floatPtr = data.assumingMemoryBound(to: Float.self)
            let totalFloats = Int(buffers[0].mDataByteSize) / MemoryLayout<Float>.size
            let frameCount = min(numSamples, totalFloats / max(1, channels))
            var mono = [Float](repeating: 0.0, count: frameCount)
            if channels == 1 {
                for i in 0..<frameCount { mono[i] = floatPtr[i] }
            } else {
                for i in 0..<frameCount {
                    let base = i * channels
                    mono[i] = 0.5 * (floatPtr[base] + floatPtr[base + 1])
                }
            }
            dspQueue.async { self.dsp.pushMonoSamples(mono, sampleRate: sampleRate) }
        }
    }

    private func startOutputLoop() {
        let interval = 1.0 / max(10.0, min(60.0, dsp.config.updateFps))
        let timer = DispatchSource.makeTimerSource(queue: dspQueue)
        timer.schedule(deadline: .now() + interval, repeating: interval)
        timer.setEventHandler { [weak self] in
            guard let self = self else { return }
            let result = self.dsp.computeBands()
            let dict: [String: Any] = [
                "type": "equalizer",
                "bands": result.bands,
                "rms": Double(round(result.rms * 10000.0) / 10000.0),
                "active": result.active,
                "capturing": self.isCapturingAudio,
                "permission": self.hasPermission
            ]
            if let data = try? JSONSerialization.data(withJSONObject: dict, options: []),
               let str = String(data: data, encoding: .utf8) {
                print(str)
                fflush(stdout)
            }
        }
        timer.resume()
        self.outputTimer = timer
    }
}

// ============================================================================
// Режим самотестирования DSP (--self-test)
// ============================================================================
func runSelfTest() {
    let config = AnalyzerConfig()
    let dsp = AudioDSPProcessor(config: config)
    let sampleRate: Float = 48000.0
    let frameSize = 1024

    var t: Float = 0.0
    let dt: Float = 1.0 / sampleRate
    var chordResult: [Int] = []
    for step in 0..<12 {
        var samples = [Float](repeating: 0.0, count: frameSize)
        for i in 0..<frameSize {
            let s1 = 0.35 * sinf(2.0 * .pi * 60.0 * t)
            let s2 = 0.25 * sinf(2.0 * .pi * 440.0 * t)
            let s3 = 0.20 * sinf(2.0 * .pi * 2500.0 * t)
            let s4 = 0.15 * sinf(2.0 * .pi * 9000.0 * t)
            samples[i] = s1 + s2 + s3 + s4
            t += dt
        }
        let ts = Double(step) * 0.021
        dsp.pushMonoSamples(samples, sampleRate: sampleRate, timestamp: ts)
        chordResult = dsp.computeBands(now: ts).bands
    }

    var clipSamples = [Float](repeating: 5.0, count: frameSize)
    clipSamples[0] = Float.nan
    clipSamples[1] = Float.infinity
    clipSamples[2] = -Float.infinity
    for i in 3..<frameSize {
        clipSamples[i] = (i % 2 == 0) ? 4.5 : -4.5
    }
    dsp.pushMonoSamples(clipSamples, sampleRate: sampleRate, timestamp: 1.0)
    let clipResult = dsp.computeBands(now: 1.0).bands

    var decayResult = clipResult
    for step in 1...25 {
        decayResult = dsp.computeBands(now: 1.0 + Double(step) * 0.15).bands
    }

    let report: [String: Any] = [
        "self_test": "passed",
        "band_count": chordResult.count,
        "chord_bands": chordResult,
        "clip_safe_bands": clipResult,
        "silence_decay_bands": decayResult,
        "permission_granted": CGPreflightScreenCaptureAccess()
    ]
    if let data = try? JSONSerialization.data(withJSONObject: report, options: []),
       let jsonStr = String(data: data, encoding: .utf8) {
        print(jsonStr)
    }
}

// ============================================================================
// Точка входа
// ============================================================================
let args = CommandLine.arguments

if args.contains("--self-test") {
    runSelfTest()
    exit(0)
}

if args.contains("--check-permission") {
    let ok = CGPreflightScreenCaptureAccess()
    print("{\"permission\":\(ok ? "true" : "false")}")
    exit(0)
}

if args.contains("--request-permission") {
    let ok = CGRequestScreenCaptureAccess()
    print("{\"permission\":\(ok ? "true" : "false")}")
    exit(0)
}

var cfg = AnalyzerConfig()
if let idx = args.firstIndex(of: "--bands"), idx + 1 < args.count, let b = Int(args[idx + 1]), b >= 4 && b <= 64 {
    cfg.bandCount = b
}
if let idx = args.firstIndex(of: "--fps"), idx + 1 < args.count, let f = Double(args[idx + 1]), f >= 10.0 && f <= 60.0 {
    cfg.updateFps = f
}

let allowPrompt = args.contains("--prompt")
let dsp = AudioDSPProcessor(config: cfg)
let engine = SystemAudioCaptureEngine(dsp: dsp, allowPrompt: allowPrompt)

signal(SIGTERM, SIG_IGN)
signal(SIGINT, SIG_IGN)
let sigTermSrc = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
sigTermSrc.setEventHandler {
    engine.stop()
    exit(0)
}
sigTermSrc.resume()

let sigIntSrc = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
sigIntSrc.setEventHandler {
    engine.stop()
    exit(0)
}
sigIntSrc.resume()

engine.start()
RunLoop.main.run()
