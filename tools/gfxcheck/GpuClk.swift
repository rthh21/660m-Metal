// Read-only GPU clock probe: effective GFX clock = FP32 FMA throughput / (lanes * 2).
// 660M: 6 CU * 64 lanes = 384 lanes, spec max 1900 MHz. Result is a lower bound (loop overhead).
import Metal
import Foundation

let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void burn(device float *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint id [[thread_position_in_grid]]) {
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
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "burn")!)
let threads = 49152
let out = dev.makeBuffer(length: threads * 4, options: .storageModeShared)!
let lanes = 384.0

struct R { var gpu: Double; var wall: Double; var mhz: Double }

func run(_ iters: UInt32) -> R {
  var n = iters
  let cb = q.makeCommandBuffer()!
  let e = cb.makeComputeCommandEncoder()!
  e.setComputePipelineState(pso)
  e.setBuffer(out, offset: 0, index: 0)
  e.setBytes(&n, length: 4, index: 1)
  e.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
  e.endEncoding()
  let t0 = DispatchTime.now().uptimeNanoseconds
  cb.commit(); cb.waitUntilCompleted()
  let wall = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9
  var gpu = cb.gpuEndTime - cb.gpuStartTime
  if gpu <= 0 { gpu = wall }
  let flops = Double(threads) * Double(iters) * 16 * 2
  return R(gpu: gpu, wall: wall, mhz: flops / gpu / (lanes * 2) / 1e6)
}
func f(_ x: Double, _ d: Int = 0) -> String { String(format: "%.\(d)f", x) }

print("device: \(dev.name)")
_ = run(64)  // warm pipeline

// Work unit ~2 ms at 1900 MHz: 2e-3*1900e6*768 / (49152*32) ≈ 1855 iterations.
let unit: UInt32 = 1855

// 1) Cold: idle 3 s, then 40 back-to-back units. Shows idle clock and ramp time.
Thread.sleep(forTimeInterval: 3)
print("\n== 1) dupa 3 s idle, 40 joburi la rand (fiecare ~2 ms la 1900 MHz)")
let tStart = DispatchTime.now().uptimeNanoseconds
var line = ""
for i in 0..<40 {
  let r = run(unit)
  let t = Double(DispatchTime.now().uptimeNanoseconds - tStart) / 1e6
  line += "#\(i) t=\(f(t))ms \(f(r.mhz))MHz gpu=\(f(r.gpu*1e3,2))ms wall=\(f(r.wall*1e3,2))ms\n"
}
print(line, terminator: "")

// 2) Sustained: ~1.5 s of back-to-back units.
print("\n== 2) sarcina sustinuta ~1.5 s")
var mhzs: [Double] = []; var walls: [Double] = []; var gpus: [Double] = []
let s0 = DispatchTime.now().uptimeNanoseconds
while Double(DispatchTime.now().uptimeNanoseconds - s0) < 1.5e9 {
  let r = run(unit); mhzs.append(r.mhz); walls.append(r.wall); gpus.append(r.gpu)
}
let sorted = mhzs.sorted()
print("joburi \(mhzs.count): min \(f(sorted.first!)) / median \(f(sorted[sorted.count/2])) / max \(f(sorted.last!)) MHz efectiv")
let ov = zip(walls, gpus).map { ($0 - $1) * 1e3 }.sorted()
print("overhead wall-gpu per job: median \(f(ov[ov.count/2],2)) ms, max \(f(ov.last!,2)) ms")

// 3) UI-like: idle 3 s, then 120 Hz frames with a small job (~1 ms at max) for 0.5 s.
Thread.sleep(forTimeInterval: 3)
print("\n== 3) dupa 3 s idle: cadre la 120 Hz, job mic (~1 ms la 1900 MHz), 60 cadre")
let small: UInt32 = unit / 2
let period = 1e9 / 120
let f0 = DispatchTime.now().uptimeNanoseconds
var fl = ""
var late = 0
for i in 0..<60 {
  let target = f0 + UInt64(Double(i) * period)
  while DispatchTime.now().uptimeNanoseconds < target { usleep(200) }
  let r = run(small)
  if r.wall * 1e3 > 8.33 { late += 1 }
  if i < 16 || i % 8 == 0 { fl += "cadru \(i): \(f(r.mhz))MHz gpu=\(f(r.gpu*1e3,2))ms wall=\(f(r.wall*1e3,2))ms\n" }
}
print(fl, terminator: "")
print("cadre peste 8.33 ms (wall): \(late)/60")
