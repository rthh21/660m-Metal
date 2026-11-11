// Read-only GPU memory bandwidth probe: float4 copy between two private buffers (read + write counted).
// Usage: GpuStream <MB per buffer>
import Metal
import Foundation
let a = CommandLine.arguments
let mb = a.count > 1 ? Int(a[1])! : 256
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void copy4(device const float4 *a [[buffer(0)]], device float4 *b [[buffer(1)]], uint id [[thread_position_in_grid]]) { b[id] = a[id] * 1.0001f; }
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "copy4")!)
let bytes = mb << 20
let A = dev.makeBuffer(length: bytes, options: .storageModePrivate)!, B = dev.makeBuffer(length: bytes, options: .storageModePrivate)!
let n = bytes / 16
func run() -> Double {
  let cb = q.makeCommandBuffer()!; let e = cb.makeComputeCommandEncoder()!
  e.setComputePipelineState(pso); e.setBuffer(A, offset: 0, index: 0); e.setBuffer(B, offset: 0, index: 1)
  e.dispatchThreads(MTLSize(width: n, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
  e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
  return Double(2 * bytes) / (cb.gpuEndTime - cb.gpuStartTime) / 1e9
}
for _ in 0..<3 { _ = run() }
var r: [Double] = []; let t0 = DispatchTime.now().uptimeNanoseconds
while Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9 < 3 { r.append(run()) }
r.sort(); print(String(format: "stream %d MB: median %.1f GB/s p10 %.1f p90 %.1f (n %d)", mb, r[r.count/2], r[r.count/10], r[r.count*9/10], r.count))
