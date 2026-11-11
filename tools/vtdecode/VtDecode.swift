// VCN decode probe: encode a synthetic clip in software, decode it with VideoToolbox, check pixels.
//   VtDecode [h264|hevc] [hw|any] [frames]
// hw = kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder (fails if VCN is not offered).
import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

setvbuf(stdout, nil, _IONBF, 0)
let args = CommandLine.arguments
let codecName = args.count > 1 ? args[1] : "h264"
let requireHW = (args.count > 2 ? args[2] : "hw") == "hw"
let nFrames = args.count > 3 ? Int(args[3]) ?? 120 : 120
let codec: CMVideoCodecType = codecName == "hevc" ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
let W = 1280, H = 720

print("VTIsHardwareDecodeSupported h264=\(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)) hevc=\(VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC))")

// Frame i: left half solid color (R,G,B) depending on i, right half horizontal gray ramp.
func expected(_ i: Int) -> (UInt8, UInt8, UInt8) {
    return (UInt8((i * 7) % 200 + 30), UInt8((i * 13) % 200 + 30), UInt8((i * 29) % 200 + 30))
}
func makeFrame(_ i: Int) -> CVPixelBuffer {
    var pb: CVPixelBuffer?
    let attrs: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:]]
    CVPixelBufferCreate(nil, W, H, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb)
    let b = pb!
    CVPixelBufferLockBaseAddress(b, [])
    let base = CVPixelBufferGetBaseAddress(b)!.assumingMemoryBound(to: UInt8.self)
    let bpr = CVPixelBufferGetBytesPerRow(b)
    let (r, g, bl) = expected(i)
    for y in 0..<H {
        let row = base + y * bpr
        for x in 0..<W {
            let p = row + x * 4
            if x < W / 2 { p[0] = bl; p[1] = g; p[2] = r } else { let v = UInt8((x - W / 2) * 255 / (W / 2)); p[0] = v; p[1] = v; p[2] = v }
            p[3] = 255
        }
    }
    CVPixelBufferUnlockBaseAddress(b, [])
    return b
}

// --- software encode
var samples: [CMSampleBuffer] = []
let encLock = NSLock()
var enc: VTCompressionSession?
let encSpec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: false]
var st = VTCompressionSessionCreate(allocator: nil, width: Int32(W), height: Int32(H), codecType: codec,
    encoderSpecification: encSpec as CFDictionary, imageBufferAttributes: nil, compressedDataAllocator: nil,
    outputCallback: nil, refcon: nil, compressionSessionOut: &enc)
guard st == noErr, let encS = enc else { print("encoder create failed \(st)"); exit(2) }
VTSessionSetProperty(encS, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanFalse)
VTSessionSetProperty(encS, key: kVTCompressionPropertyKey_AverageBitRate, value: 8_000_000 as CFNumber)
VTSessionSetProperty(encS, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 30 as CFNumber)
VTSessionSetProperty(encS, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
var hwEnc: CFTypeRef?
VTSessionCopyProperty(encS, key: kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, allocator: nil, valueOut: &hwEnc)
print("encoder \(codecName) hardware=\(String(describing: hwEnc))")
let t0 = Date()
for i in 0..<nFrames {
    let pts = CMTime(value: CMTimeValue(i), timescale: 30)
    VTCompressionSessionEncodeFrame(encS, imageBuffer: makeFrame(i), presentationTimeStamp: pts, duration: CMTime(value: 1, timescale: 30),
        frameProperties: nil, infoFlagsOut: nil) { status, _, sb in
        if status == noErr, let sb = sb { encLock.lock(); samples.append(sb); encLock.unlock() }
    }
}
VTCompressionSessionCompleteFrames(encS, untilPresentationTimeStamp: .invalid)
print(String(format: "encoded %d/%d frames in %.2f s", samples.count, nFrames, Date().timeIntervalSince(t0)))
guard let fmt = samples.first.flatMap({ CMSampleBufferGetFormatDescription($0) }) else { print("no samples"); exit(2) }

// --- decode
var decSpec: [CFString: Any] = [:]
if requireHW { decSpec[kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder] = true }
else { decSpec[kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder] = true }
let outAttrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA, kCVPixelBufferIOSurfacePropertiesKey: [:]]
var dec: VTDecompressionSession?
st = VTDecompressionSessionCreate(allocator: nil, formatDescription: fmt, decoderSpecification: decSpec as CFDictionary,
    imageBufferAttributes: outAttrs as CFDictionary, outputCallback: nil, decompressionSessionOut: &dec)
guard st == noErr, let decS = dec else { print("decoder create failed \(st) (requireHW=\(requireHW))"); exit(3) }
var hwDec: CFTypeRef?
VTSessionCopyProperty(decS, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, allocator: nil, valueOut: &hwDec)
print("decoder hardware=\(String(describing: hwDec))")
var good = 0, bad = 0, errs = 0, firstBad = ""
let t1 = Date()
for (i, sb) in samples.enumerated() {
    let s = VTDecompressionSessionDecodeFrame(decS, sampleBuffer: sb, flags: [], infoFlagsOut: nil) { status, _, img, _, _ in
        guard status == noErr, let img = img else { errs += 1; return }
        CVPixelBufferLockBaseAddress(img, .readOnly)
        let base = CVPixelBufferGetBaseAddress(img)!.assumingMemoryBound(to: UInt8.self)
        let bpr = CVPixelBufferGetBytesPerRow(img)
        let p = base + (H / 2) * bpr + (W / 4) * 4
        let q = base + (H / 2) * bpr + (W * 3 / 4) * 4
        let (r, g, b) = expected(i)
        let ok = abs(Int(p[2]) - Int(r)) < 12 && abs(Int(p[1]) - Int(g)) < 12 && abs(Int(p[0]) - Int(b)) < 12 && abs(Int(q[1]) - 127) < 12
        if ok { good += 1 } else { bad += 1; if firstBad.isEmpty { firstBad = "frame \(i): got \(p[2]),\(p[1]),\(p[0]) ramp \(q[1]) want \(r),\(g),\(b) ramp 127" } }
        CVPixelBufferUnlockBaseAddress(img, .readOnly)
    }
    if s != noErr { errs += 1; if errs < 4 { print("decode frame \(i) error \(s)") } }
}
VTDecompressionSessionWaitForAsynchronousFrames(decS)
let dt = Date().timeIntervalSince(t1)
print(String(format: "decoded good %d bad %d errors %d of %d in %.3f s (%.0f fps)", good, bad, errs, samples.count, dt, Double(samples.count) / dt))
if !firstBad.isEmpty { print("first mismatch: \(firstBad)") }
VTDecompressionSessionInvalidate(decS)
exit(good == samples.count ? 0 : 1)
