// Read-only GPU clock probe for light (low current) load: does the real GFX clock follow the soft max?
// chain1: one dependent FMA chain per thread, few waves -> latency bound, low VALU duty, low current.
//         Its throughput is proportional to the real clock, so the ratio between two caps shows whether
//         the clock reaches the higher cap. chain16: 16 independent chains (ALU bound, high current).
// Usage: GpuClkLight <chain1|chain16> <threads> <threadgroup> -> median Gflop/s over ~3 s.
import Metal
import Foundation

let a = CommandLine.arguments
let kname = a.count > 1 ? a[1] : "chain1"
let threads = a.count > 2 ? Int(a[2])! : 384
let tg = a.count > 3 ? Int(a[3])! : 32
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void chain1(device float *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint id [[thread_position_in_grid]]) {
  float a0=id*1e-7f; const float m = 0.99999f, c = 1e-6f;
  for (uint i = 0; i < n; i++) {
    a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c);
    a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c);
    a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c);
    a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c); a0=fma(a0,m,c);
  }
  o[id] = a0;
}
kernel void chain16(device float *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint id [[thread_position_in_grid]]) {
  float a0=id*1e-7f, a1=a0+1, a2=a0+2, a3=a0+3, a4=a0+4, a5=a0+5, a6=a0+6, a7=a0+7;
  float a8=a0+8, a9=a0+9, a10=a0+10, a11=a0+11, a12=a0+12, a13=a0+13, a14=a0+14, a15=a0+15;
  const float m = 0.99999f, c = 1e-6f;
  for (uint i = 0; i < n; i++) {
    a0=fma(a0,m,c); a1=fma(a1,m,c); a2=fma(a2,m,c); a3=fma(a3,m,c);
    a4=fma(a4,m,c); a5=fma(a5,m,c); a6=fma(a6,m,c); a7=fma(a7,m,c);
    a8=fma(a8,m,c); a9=fma(a9,m,c); a10=fma(a10,m,c); a11=fma(a11,m,c);
    a12=fma(a12,m,c); a13=fma(a13,m,c); a14=fma(a14,m,c); a15=fma(a15,m,c);
  }
  o[id] = a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15;
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: kname)!)
let out = dev.makeBuffer(length: max(threads, 64) * 4, options: .storageModeShared)!

func run(_ iters: UInt32) -> Double {
  var n = iters
  let cb = q.makeCommandBuffer()!
  let e = cb.makeComputeCommandEncoder()!
  e.setComputePipelineState(pso)
  e.setBuffer(out, offset: 0, index: 0)
  e.setBytes(&n, length: 4, index: 1)
  e.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: tg, height: 1, depth: 1))
  e.endEncoding()
  cb.commit(); cb.waitUntilCompleted()
  return Double(threads) * Double(iters) * 32 / (cb.gpuEndTime - cb.gpuStartTime) / 1e9
}
// calibrate one job to ~10 ms
var it: UInt32 = 1000
for _ in 0..<6 { let cb0 = DispatchTime.now().uptimeNanoseconds; _ = run(it); let dt = Double(DispatchTime.now().uptimeNanoseconds - cb0) / 1e9; it = UInt32(max(100, min(2_000_000, Double(it) * 0.010 / max(dt, 1e-4)))) }
var r: [Double] = []
let t0 = DispatchTime.now().uptimeNanoseconds
while Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9 < 3 { r.append(run(it)) }
r.sort()
print(String(format: "%@ threads %d tg %d: n %d median %.2f p10 %.2f p90 %.2f Gflop/s", kname, threads, tg, r.count, r[r.count/2], r[r.count/10], r[r.count*9/10]))
