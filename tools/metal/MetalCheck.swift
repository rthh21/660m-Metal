// Prepared offline; NOT COMPILED or RUN on macOS in this workspace.
// No window, drawable, CAMetalLayer, installation, or driver changes.
import Foundation
import Metal
import Dispatch
import Darwin

struct CheckError: Error, CustomStringConvertible {
    let description: String
    init(_ message: String) { description = message }
}

let shader = """
#include <metal_stdlib>
using namespace metal;
kernel void compute_check(device const uint *a [[buffer(0)]],
                          device uint *b [[buffer(1)]],
                          constant uint &n [[buffer(2)]],
                          uint i [[thread_position_in_grid]]) {
    if (i < n) b[i] = (a[i] * 1664525u + 1013904223u) ^ (i * 2654435761u);
}
struct Vertex { float4 position [[position]]; };
vertex Vertex vertex_check(uint id [[vertex_id]]) {
    float2 points[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return {float4(points[id],0,1)};
}
fragment float4 fragment_check(Vertex v [[stage_in]]) {
    uint x = uint(v.position.x), y = uint(v.position.y);
    return float4(float((x*17+y*3)&255), float((x*5+y*29)&255), float((x^y)&255),255.0)/255.0;
}
fragment float4 texture_check(Vertex v [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) {
    return t.read(uint2(v.position.xy));
}
"""

func finish(_ command: MTLCommandBuffer) throws {
    let completed = DispatchSemaphore(value: 0)
    command.addCompletedHandler { _ in completed.signal() }
    command.commit()
    guard completed.wait(timeout: .now() + .seconds(10)) == .success else {
        throw CheckError("command timeout (10s); GPU work cannot be cancelled by this tool")
    }
    guard command.status == .completed, command.error == nil else {
        throw CheckError("command status=\(command.status.rawValue), error=\(String(describing: command.error))")
    }
}

func compute(_ device: MTLDevice, _ queue: MTLCommandQueue, _ library: MTLLibrary) throws -> [String: Any] {
    let n = 4099, guardWords = 16, sentinel: UInt32 = 0xa55ac33c
    let bytes = (n + guardWords) * MemoryLayout<UInt32>.stride
    guard let input = device.makeBuffer(length: bytes, options: .storageModeShared),
          let output = device.makeBuffer(length: bytes, options: .storageModeShared),
          let function = library.makeFunction(name: "compute_check") else { throw CheckError("compute allocation/function failed") }
    let a = input.contents().bindMemory(to: UInt32.self, capacity: n + guardWords)
    let b = output.contents().bindMemory(to: UInt32.self, capacity: n + guardWords)
    for i in 0..<(n + guardWords) { a[i] = UInt32(i) &* 747796405 &+ 2891336453; b[i] = sentinel }
    let pipeline = try device.makeComputePipelineState(function: function)
    let width = min(64, pipeline.maxTotalThreadsPerThreadgroup)
    guard width > 0, let command = queue.makeCommandBuffer(),
          let encoder = command.makeComputeCommandEncoder() else { throw CheckError("compute encoder unavailable") }
    command.label = "Rembrandt660M exact integer readback"
    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(input, offset: 0, index: 0)
    encoder.setBuffer(output, offset: 0, index: 1)
    var count = UInt32(n)
    encoder.setBytes(&count, length: MemoryLayout<UInt32>.stride, index: 2)
    encoder.dispatchThreadgroups(MTLSize(width: (n + width - 1)/width, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
    encoder.endEncoding()
    try finish(command)
    for i in 0..<n {
        let original = UInt32(i) &* 747796405 &+ 2891336453
        let expected = (original &* 1664525 &+ 1013904223) ^ (UInt32(i) &* 2654435761)
        guard a[i] == original, b[i] == expected else {
            throw CheckError("compute mismatch index=\(i), expected=\(expected), got=\(b[i])")
        }
    }
    for i in n..<(n + guardWords) {
        guard b[i] == sentinel, a[i] == UInt32(i) &* 747796405 &+ 2891336453 else {
            throw CheckError("compute guard/input modified at \(i)")
        }
    }
    return ["stage": "compute", "status": "PASS", "elements": n,
            "guard_words": guardWords, "command_status": command.status.rawValue]
}

func pixel(_ x: Int, _ y: Int) -> [UInt8] {
    [UInt8((x*17+y*3)&255), UInt8((x*5+y*29)&255), UInt8((x^y)&255), 255]
}

func render(_ device: MTLDevice, _ queue: MTLCommandQueue, _ library: MTLLibrary,
            sampleTexture: Bool) throws -> [String: Any] {
    let width = 37, height = 23, rowBytes = 256
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm,
                         width: width, height: height, mipmapped: false)
    descriptor.storageMode = .private
    descriptor.usage = [.renderTarget, .shaderRead]
    guard let target = device.makeTexture(descriptor: descriptor),
          let source = device.makeTexture(descriptor: descriptor),
          let readback = device.makeBuffer(length: rowBytes*height, options: .storageModeShared),
          let upload = device.makeBuffer(length: rowBytes*height, options: .storageModeShared),
          let command = queue.makeCommandBuffer() else { throw CheckError("texture/buffer allocation failed") }
    memset(readback.contents(), 0xa5, rowBytes*height)
    memset(upload.contents(), 0, rowBytes*height)
    let pixels = upload.contents().bindMemory(to: UInt8.self, capacity: rowBytes*height)
    for y in 0..<height { for x in 0..<width {
        let color = pixel(x,y)
        for c in 0..<4 { pixels[y*rowBytes+x*4+c] = color[c] }
    } }
    if sampleTexture {
        guard let blit = command.makeBlitCommandEncoder() else { throw CheckError("upload blit unavailable") }
        blit.copy(from: upload, sourceOffset: 0, sourceBytesPerRow: rowBytes,
                  sourceBytesPerImage: rowBytes*height, sourceSize: MTLSize(width: width,height: height,depth: 1),
                  to: source, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0,y: 0,z: 0))
        blit.endEncoding()
    }
    let pipelineDescriptor = MTLRenderPipelineDescriptor()
    pipelineDescriptor.vertexFunction = library.makeFunction(name: "vertex_check")
    pipelineDescriptor.fragmentFunction = library.makeFunction(name: sampleTexture ? "texture_check" : "fragment_check")
    pipelineDescriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
    let pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = target
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1,0,1,0)
    guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { throw CheckError("render encoder unavailable") }
    encoder.setRenderPipelineState(pipeline)
    encoder.setViewport(MTLViewport(originX: 0,originY: 0,width: Double(width),height: Double(height),znear: 0,zfar: 1))
    if sampleTexture { encoder.setFragmentTexture(source, index: 0) }
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
    guard let blit = command.makeBlitCommandEncoder() else { throw CheckError("readback blit unavailable") }
    blit.copy(from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0,y: 0,z: 0),
              sourceSize: MTLSize(width: width,height: height,depth: 1), to: readback,
              destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes*height)
    blit.endEncoding()
    try finish(command)
    let actual = readback.contents().bindMemory(to: UInt8.self, capacity: rowBytes*height)
    for y in 0..<height { for x in 0..<width {
        let expected = pixel(x,y)
        for c in 0..<4 {
            guard abs(Int(actual[y*rowBytes+x*4+c])-Int(expected[c])) <= 1 else {
                throw CheckError("pixel mismatch x=\(x), y=\(y), channel=\(c)")
            }
        }
    } }
    return ["stage": sampleTexture ? "texture_read" : "offscreen_render", "status": "PASS",
            "width": width,"height": height,"channel_tolerance": 1,"command_status": command.status.rawValue]
}

var result: [String: Any] = ["tool": "Rembrandt660M MetalCheck", "status": "NOT_TESTED",
                            "os": ProcessInfo.processInfo.operatingSystemVersionString]
func emit() {
    if let data = try? JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys]) {
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    }
}
do {
    let args = Array(CommandLine.arguments.dropFirst())
    let devices = MTLCopyAllDevices()
    result["devices"] = devices.map { ["name": $0.name, "registry_id": String(format: "0x%llx", $0.registryID)] }
    if args.isEmpty || args == ["--list"] {
        result["status"] = "ENUMERATION_ONLY"; emit(); exit(0)
    }
    guard args.count == 5, args[0] == "--run", args[1] == "--registry-id", args[3] == "--stage",
          ["compute","render","texture"].contains(args[4]) else {
        throw CheckError("usage: --list | --run --registry-id 0xHEX --stage compute|render|texture")
    }
    let idText = args[2].hasPrefix("0x") ? String(args[2].dropFirst(2)) : args[2]
    guard let id = UInt64(idText, radix: 16), let device = devices.first(where: { $0.registryID == id }),
          let queue = device.makeCommandQueue() else { throw CheckError("selected device/queue unavailable") }
    result["selected_name"] = device.name
    result["selected_registry_id"] = String(format: "0x%llx", device.registryID)
    let library = try device.makeLibrary(source: shader, options: nil)
    var stages: [[String: Any]] = []
    stages.append(try compute(device, queue, library)); result["stages"] = stages
    if args[4] != "compute" { stages.append(try render(device, queue, library, sampleTexture: false)); result["stages"] = stages }
    if args[4] == "texture" { stages.append(try render(device, queue, library, sampleTexture: true)); result["stages"] = stages }
    result["status"] = "PASS"; emit()
} catch {
    result["status"] = "FAIL"; result["error"] = String(describing: error); emit(); exit(1)
}
