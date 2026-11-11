// Read-only duty-cycle probe: heavy FMA jobs of <job ms> separated by <gap ms> idle. Prints the
// per-job effective MHz (same calibration as GpuClkSustained) to see whether the PMFW limiter
// trips on short bursts or only on sustained load. Usage: GpuDuty <job ms> <gap ms> <seconds>
import Metal
import Foundation
let a = CommandLine.arguments
let jobMs = a.count > 1 ? Double(a[1])! : 4, gapMs = a.count > 2 ? Double(a[2])! : 4, secs = a.count > 3 ? Double(a[3])! : 4
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
func run(_ iters: UInt32) -> Double {
  var n = iters
  let cb = q.makeCommandBuffer()!; let e = cb.makeComputeCommandEncoder()!
  e.setComputePipelineState(pso); e.setBuffer(out, offset: 0, index: 0); e.setBytes(&n, length: 4, index: 1)
  e.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
  e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
  return Double(threads) * Double(iters) * 32 / (cb.gpuEndTime - cb.gpuStartTime) / 768 / 1e6
}
_ = run(64)
let iters = UInt32(jobMs * 1e-3 * 1400e6 * 768 / Double(threads * 32))  // ~job ms at 1400 MHz
var r: [Double] = []
let t0 = DispatchTime.now().uptimeNanoseconds
while Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9 < secs {
  r.append(run(iters))
  if gapMs > 0 { usleep(UInt32(gapMs * 1000)) }
}
r.sort()
print(String(format: "duty job %.0f ms gap %.0f ms: n %d median %.0f p10 %.0f p90 %.0f MHz", jobMs, gapMs, r.count, r[r.count/2], r[r.count/10], r[r.count*9/10]))
