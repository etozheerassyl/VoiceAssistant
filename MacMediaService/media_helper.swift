import Foundation
import CoreAudio
import AudioToolbox
import AppKit
import ObjectiveC

// ============================================================================
// CoreAudio: macOS System Volume Control
// ============================================================================
func getDefaultOutputDeviceID() -> AudioDeviceID? {
    var defaultOutputDeviceID = AudioDeviceID(0)
    var propertySize = UInt32(MemoryLayout<AudioDeviceID>.size)
    var propertyAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    let status = AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject),
        &propertyAddress,
        0,
        nil,
        &propertySize,
        &defaultOutputDeviceID
    )
    return (status == noErr) ? defaultOutputDeviceID : nil
}

func getSystemVolume() -> Int {
    guard let deviceID = getDefaultOutputDeviceID() else { return 50 }
    var volume: Float32 = 0.0
    var propertySize = UInt32(MemoryLayout<Float32>.size)
    var propertyAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        mScope: kAudioDevicePropertyScopeOutput,
        mElement: kAudioObjectPropertyElementMain
    )
    let status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nil, &propertySize, &volume)
    return (status == noErr) ? Int(round(volume * 100.0)) : 50
}

func setSystemVolume(_ percent: Int) -> Bool {
    guard let deviceID = getDefaultOutputDeviceID() else { return false }
    var vol = Float32(max(0, min(100, percent))) / 100.0
    let propertySize = UInt32(MemoryLayout<Float32>.size)
    var propertyAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        mScope: kAudioDevicePropertyScopeOutput,
        mElement: kAudioObjectPropertyElementMain
    )
    let status = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, nil, propertySize, &vol)
    return status == noErr
}

// ============================================================================
// MediaRemote: Objective-C Runtime Interface (macOS 15+ / 26+ / 27+ compatible)
// ============================================================================
let mrHandle = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_NOW)

@objc protocol MRControllerProto {
    @objc(sendCommand:options:completion:)
    func sendCommand(_ cmd: UInt32, options: NSDictionary?, completion: @escaping (Any?) -> Void)
}

enum MediaCommand: UInt32 {
    case play = 0
    case pause = 1
    case togglePlayPause = 2
    case stop = 3
    case nextTrack = 4
    case previousTrack = 5
    case seekToPlaybackPosition = 24
}

var lastSeekTarget: Double? = nil
var lastSeekTime: Date = .distantPast
var lastSeekTitle: String = ""

func sendMRCommandSync(_ cmd: UInt32, options: NSDictionary? = nil) -> Bool {
    guard let ctrlCls = NSClassFromString("MRNowPlayingController") as? NSObject.Type,
          let ctrlObj = ctrlCls.value(forKey: "localRouteController") as? NSObject else {
        return false
    }
    let ctrl = unsafeBitCast(ctrlObj, to: MRControllerProto.self)
    var completed = false
    ctrl.sendCommand(cmd, options: options) { _ in
        completed = true
    }
    let deadline = Date().addingTimeInterval(0.35)
    while !completed && Date() < deadline {
        CFRunLoopRunInMode(CFRunLoopMode.defaultMode, 0.015, false)
    }
    return true
}

func cleanMediaTitle(_ raw: String) -> String {
    let stripped = raw.replacingOccurrences(
        of: #"^\(\d+\)\s*"#,
        with: "",
        options: .regularExpression
    )
    return stripped.trimmingCharacters(in: .whitespacesAndNewlines)
}

// ============================================================================
// Media State Fetcher via MRNowPlayingRequest
// ============================================================================
func fetchCurrentState() -> [String: Any] {
    CFRunLoopRunInMode(CFRunLoopMode.defaultMode, 0.005, false)
    let vol = getSystemVolume()
    var isPlaying = false
    var appName = "macOS"
    var bundleId = ""
    var title = ""
    var artist = ""
    var album = ""
    var duration = 0
    var position = 0

    if let reqCls = NSClassFromString("MRNowPlayingRequest") as? NSObject.Type {
        isPlaying = (reqCls.value(forKey: "localIsPlaying") as? Bool) ?? false

        if let path = reqCls.value(forKey: "localNowPlayingPlayerPath") as? NSObject,
           let client = path.value(forKey: "client") as? NSObject {
            if let dn = client.value(forKey: "displayName") as? String, !dn.isEmpty {
                appName = dn
            }
            if let bid = client.value(forKey: "bundleIdentifier") as? String {
                bundleId = bid
            }
        }

        if let item = reqCls.value(forKey: "localNowPlayingItem") as? NSObject,
           let meta = item.value(forKey: "metadata") as? NSObject {
            let rawTitle = (meta.value(forKey: "title") as? String) ?? ""
            title = cleanMediaTitle(rawTitle)
            artist = ((meta.value(forKey: "trackArtistName") as? String) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            album = ((meta.value(forKey: "albumName") as? String) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)

            let durVal = (meta.value(forKey: "duration") as? Double) ?? 0.0
            let posVal = (meta.value(forKey: "elapsedTime") as? Double) ?? 0.0
            let tsVal  = (meta.value(forKey: "elapsedTimeTimestamp") as? Double) ?? 0.0
            let rate   = (meta.value(forKey: "playbackRate") as? Double) ?? (isPlaying ? 1.0 : 0.0)

            duration = max(0, Int(round(durVal)))

            var livePos = posVal
            if isPlaying && tsVal > 0 {
                let nowRef = Date().timeIntervalSinceReferenceDate
                let delta = max(0.0, nowRef - tsVal) * (rate > 0 ? rate : 1.0)
                livePos = posVal + delta
            }

            // If we recently performed a seek on the same track, smooth over browser MediaSession lag
            if let seekTarget = lastSeekTarget,
               title == lastSeekTitle,
               Date().timeIntervalSince(lastSeekTime) < 1.2 {
                let sinceSeek = max(0.0, Date().timeIntervalSince(lastSeekTime))
                livePos = seekTarget + (isPlaying ? sinceSeek * (rate > 0 ? rate : 1.0) : 0.0)
            }

            if duration > 0 {
                livePos = min(Double(duration), livePos)
            }
            position = max(0, Int(round(livePos)))
        }
    }

    let stateStr = isPlaying ? "playing" : (title.isEmpty ? "stopped" : "paused")
    return [
        "type": "media_state",
        "title": title,
        "artist": artist,
        "album": album,
        "source": appName,
        "bundle_id": bundleId,
        "state": stateStr,
        "position": position,
        "duration": duration,
        "volume": vol
    ]
}

func executeCommand(_ cmd: String, arg: String?) -> [String: Any] {
    var handled = false
    var targetPos: Int? = nil

    switch cmd {
    case "PLAY_PAUSE":
        handled = sendMRCommandSync(MediaCommand.togglePlayPause.rawValue)
    case "PLAY":
        handled = sendMRCommandSync(MediaCommand.play.rawValue)
    case "PAUSE":
        handled = sendMRCommandSync(MediaCommand.pause.rawValue)
    case "NEXT_TRACK":
        lastSeekTarget = nil
        handled = sendMRCommandSync(MediaCommand.nextTrack.rawValue)
    case "PREVIOUS_TRACK":
        lastSeekTarget = nil
        handled = sendMRCommandSync(MediaCommand.previousTrack.rawValue)
    case "SEEK_TO":
        if let a = arg, let sec = Double(a) {
            let state = fetchCurrentState()
            let dur = Double(state["duration"] as? Int ?? 0)
            let target = (dur > 0) ? min(dur, max(0.0, sec)) : max(0.0, sec)
            lastSeekTarget = target
            lastSeekTime = Date()
            lastSeekTitle = (state["title"] as? String) ?? ""
            targetPos = Int(round(target))
            let opts: NSDictionary = ["kMRMediaRemoteOptionPlaybackPosition": target]
            handled = sendMRCommandSync(MediaCommand.seekToPlaybackPosition.rawValue, options: opts)
        }
    case "SEEK_FORWARD":
        let delta = (arg != nil) ? (Double(arg!) ?? 10.0) : 10.0
        let state = fetchCurrentState()
        let curPos = Double(state["position"] as? Int ?? 0)
        let dur = Double(state["duration"] as? Int ?? 0)
        let target = (dur > 0) ? min(dur, curPos + delta) : (curPos + delta)
        lastSeekTarget = target
        lastSeekTime = Date()
        lastSeekTitle = (state["title"] as? String) ?? ""
        targetPos = Int(round(target))
        let opts: NSDictionary = ["kMRMediaRemoteOptionPlaybackPosition": target]
        handled = sendMRCommandSync(MediaCommand.seekToPlaybackPosition.rawValue, options: opts)
    case "SEEK_BACKWARD":
        let delta = (arg != nil) ? (Double(arg!) ?? 10.0) : 10.0
        let state = fetchCurrentState()
        let curPos = Double(state["position"] as? Int ?? 0)
        let target = max(0.0, curPos - delta)
        lastSeekTarget = target
        lastSeekTime = Date()
        lastSeekTitle = (state["title"] as? String) ?? ""
        targetPos = Int(round(target))
        let opts: NSDictionary = ["kMRMediaRemoteOptionPlaybackPosition": target]
        handled = sendMRCommandSync(MediaCommand.seekToPlaybackPosition.rawValue, options: opts)
    default:
        handled = false
    }

    var resp: [String: Any] = ["command": cmd, "success": handled]
    if let tp = targetPos {
        resp["position"] = tp
    }
    return resp
}

func printJSON(_ dict: [String: Any]) {
    if let data = try? JSONSerialization.data(withJSONObject: dict, options: []),
       let jsonStr = String(data: data, encoding: .utf8) {
        print(jsonStr)
        fflush(stdout)
    }
}

// ============================================================================
// Entry Point (Supports --daemon for persistent zero-latency IPC & CLI modes)
// ============================================================================
let args = CommandLine.arguments

if args.count > 1 {
    let mode = args[1]

    if mode == "--daemon" {
        // Persistent line-oriented IPC loop on main thread + RunLoop pumping
        DispatchQueue.global(qos: .userInteractive).async {
            while let line = readLine() {
                let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
                if trimmed.isEmpty { continue }
                let parts = trimmed.split(separator: " ", maxSplits: 2).map(String.init)
                let verb = parts[0]

                DispatchQueue.main.sync {
                    if verb == "GET" {
                        printJSON(fetchCurrentState())
                    } else if verb == "SET_VOLUME", parts.count > 1, let v = Int(parts[1]) {
                        let ok = setSystemVolume(v)
                        let cur = getSystemVolume()
                        printJSON(["success": ok, "volume": cur])
                    } else if verb == "CMD", parts.count > 1 {
                        let cmdName = parts[1]
                        let cmdArg = (parts.count > 2) ? parts[2] : nil
                        let res = executeCommand(cmdName, arg: cmdArg)
                        printJSON(res)
                    } else {
                        printJSON(["success": false, "error": "Unknown daemon command"])
                    }
                }
            }
            exit(0)
        }
        RunLoop.main.run()
        exit(0)
    } else if mode == "--get" {
        printJSON(fetchCurrentState())
        exit(0)
    } else if mode == "--set-volume", args.count > 2, let v = Int(args[2]) {
        let ok = setSystemVolume(v)
        let cur = getSystemVolume()
        printJSON(["success": ok, "volume": cur])
        exit(0)
    } else if mode == "--cmd", args.count > 2 {
        let cmd = args[2]
        let arg = (args.count > 3) ? args[3] : nil
        printJSON(executeCommand(cmd, arg: arg))
        exit(0)
    }
}

printJSON(fetchCurrentState())
