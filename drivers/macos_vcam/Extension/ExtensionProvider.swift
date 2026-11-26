import CoreMediaIO
import CoreVideo
import Foundation
import IOKit.audio
import os.log

private let kFrameRate: Int32 = 30
// Output sizes offered to client apps (16:9 and 9:16 vertical).
private let kSizes: [(Int32, Int32)] = [(1920, 1080), (1280, 720), (3840, 2160), (1080, 1920), (720, 1280)]

// Same UUID scheme as the Windows DirectShow CLSIDs: last byte = 0x61 + bus slot.
private func deviceUUID(_ slot: Int32, stream: Bool) -> UUID {
    UUID(uuidString: String(format: "6C4A8B10-2E5D-4F7A-9B3C-1D2E3F40%@%02X", stream ? "52" : "51", 0x61 + Int(slot)))!
}

final class LanVidStreamSource: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    let device: CMIOExtensionDevice
    private let streamFormats: [CMIOExtensionStreamFormat]
    var activeFormatIndex: Int = 0

    init(localizedName: String, streamID: UUID, streamFormats: [CMIOExtensionStreamFormat], device: CMIOExtensionDevice) {
        self.device = device
        self.streamFormats = streamFormats
        super.init()
        stream = CMIOExtensionStream(localizedName: localizedName, streamID: streamID, direction: .source,
                                     clockType: .hostTime, source: self)
    }

    var formats: [CMIOExtensionStreamFormat] { streamFormats }
    var availableProperties: Set<CMIOExtensionProperty> { [.streamActiveFormatIndex, .streamFrameDuration] }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let sp = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { sp.activeFormatIndex = activeFormatIndex }
        if properties.contains(.streamFrameDuration) { sp.frameDuration = CMTime(value: 1, timescale: kFrameRate) }
        return sp
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        if let idx = streamProperties.activeFormatIndex, idx >= 0, idx < streamFormats.count { activeFormatIndex = idx }
    }

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }
    func startStream() throws { (device.source as? LanVidDeviceSource)?.startStreaming() }
    func stopStream() throws { (device.source as? LanVidDeviceSource)?.stopStreaming() }
}

final class LanVidDeviceSource: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!
    private var streamSource: LanVidStreamSource!
    private var streamingCounter = 0
    private var timer: DispatchSourceTimer?
    private let timerQueue: DispatchQueue
    private var descriptions: [CMFormatDescription] = []
    private var pools: [CVPixelBufferPool] = []
    private let auxAttributes: NSDictionary = [kCVPixelBufferPoolAllocationThresholdKey: 5]
    let slot: Int32

    init(localizedName: String, slot: Int32) {
        self.slot = slot
        timerQueue = DispatchQueue(label: "lanvid.vcam.\(slot)", qos: .userInteractive)
        super.init()
        device = CMIOExtensionDevice(localizedName: localizedName, deviceID: deviceUUID(slot, stream: false),
                                     legacyDeviceID: nil, source: self)
        var formats: [CMIOExtensionStreamFormat] = []
        let fd = CMTime(value: 1, timescale: kFrameRate)
        for (w, h) in kSizes {
            var desc: CMFormatDescription?
            CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_32BGRA,
                                           width: w, height: h, extensions: nil, formatDescriptionOut: &desc)
            let attrs: NSDictionary = [kCVPixelBufferWidthKey: w, kCVPixelBufferHeightKey: h,
                                       kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
                                       kCVPixelBufferIOSurfacePropertiesKey: NSDictionary()]
            var pool: CVPixelBufferPool?
            CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs, &pool)
            guard let d = desc, let p = pool else { continue }
            descriptions.append(d)
            pools.append(p)
            formats.append(CMIOExtensionStreamFormat(formatDescription: d, maxFrameDuration: fd, minFrameDuration: fd,
                                                     validFrameDurations: nil))
        }
        streamSource = LanVidStreamSource(localizedName: "\(localizedName).Video", streamID: deviceUUID(slot, stream: true),
                                          streamFormats: formats, device: device)
        do {
            try device.addStream(streamSource.stream)
        } catch {
            os_log(.error, "LanVid vcam: addStream failed: %{public}@", error.localizedDescription)
        }
    }

    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel] }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        let dp = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) { dp.transportType = kIOAudioDeviceTransportTypeVirtual }
        if properties.contains(.deviceModel) { dp.model = "LanVid Virtual Camera" }
        return dp
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}

    func startStreaming() {
        timerQueue.async { [weak self] in
            guard let self else { return }
            self.streamingCounter += 1
            guard self.timer == nil else { return }
            let t = DispatchSource.makeTimerSource(flags: .strict, queue: self.timerQueue)
            t.schedule(deadline: .now(), repeating: 1.0 / Double(kFrameRate), leeway: .milliseconds(1))
            t.setEventHandler { [weak self] in self?.emitFrame() }
            self.timer = t
            t.resume()
        }
    }

    func stopStreaming() {
        timerQueue.async {
            if self.streamingCounter > 1 {
                self.streamingCounter -= 1
            } else {
                self.streamingCounter = 0
                self.timer?.cancel()
                self.timer = nil
            }
        }
    }

    private func emitFrame() {
        let idx = min(max(streamSource.activeFormatIndex, 0), pools.count - 1)
        guard idx >= 0 else { return }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pools[idx], auxAttributes, &pb) == kCVReturnSuccess,
              let pixelBuffer = pb else { return }
        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        if let base = CVPixelBufferGetBaseAddress(pixelBuffer) {
            _ = lvb_render_bgra(slot, base.assumingMemoryBound(to: UInt8.self), UInt32(CVPixelBufferGetWidth(pixelBuffer)),
                                UInt32(CVPixelBufferGetHeight(pixelBuffer)), UInt32(CVPixelBufferGetBytesPerRow(pixelBuffer)))
        }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, [])
        var timing = CMSampleTimingInfo()
        timing.presentationTimeStamp = CMClockGetTime(CMClockGetHostTimeClock())
        timing.duration = CMTime(value: 1, timescale: kFrameRate)
        var sbuf: CMSampleBuffer?
        let err = CMSampleBufferCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pixelBuffer, dataReady: true,
                                                     makeDataReadyCallback: nil, refcon: nil,
                                                     formatDescription: descriptions[idx], sampleTiming: &timing,
                                                     sampleBufferOut: &sbuf)
        if err == noErr, let s = sbuf {
            streamSource.stream.send(s, discontinuity: [],
                                     hostTimeInNanoseconds: UInt64(timing.presentationTimeStamp.seconds * Double(NSEC_PER_SEC)))
        }
    }
}

final class LanVidProviderSource: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var devices: [LanVidDeviceSource] = []

    init(clientQueue: DispatchQueue?) {
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
        for i in 0..<lvb_device_count() {
            let name = String(cString: lvb_device_name(i))
            let d = LanVidDeviceSource(localizedName: name, slot: i)
            devices.append(d)
            do {
                try provider.addDevice(d.device)
            } catch {
                os_log(.error, "LanVid vcam: addDevice failed: %{public}@", error.localizedDescription)
            }
        }
    }

    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) {}
    var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
        let pp = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerManufacturer) { pp.manufacturer = "LanVid" }
        return pp
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}
