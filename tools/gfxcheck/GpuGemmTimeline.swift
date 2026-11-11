// Read-only: continuous MPS SGEMM for <seconds>, prints Gflop/s per 0.5 s bin. Used to see how fast the
// PMFW limiter trips/recovers when the soft max or the CPU load changes during the run.
import Metal
import MetalPerformanceShaders
import Foundation
let a = CommandLine.arguments
let secs = a.count > 1 ? Double(a[1])! : 10
let n = 1024
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let desc = MPSMatrixDescriptor(rows: n, columns: n, rowBytes: n * 4, dataType: .float32)
func mk() -> MPSMatrix { MPSMatrix(buffer: dev.makeBuffer(length: n * n * 4, options: .storageModePrivate)!, descriptor: desc) }
let A = mk(), B = mk(), C = mk()
let mm = MPSMatrixMultiplication(device: dev, transposeLeft: false, transposeRight: false, resultRows: n, resultColumns: n, interiorColumns: n, alpha: 1, beta: 0)
let t0 = DispatchTime.now().uptimeNanoseconds
var bin = 0; var acc: [Double] = []
var line = ""
while true {
  let el = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9
  if el >= secs { break }
  let b = Int(el / 0.5)
  if b != bin { if !acc.isEmpty { acc.sort(); line += String(format: " %.0f", acc[acc.count/2]) }; acc = []; bin = b }
  let cb = q.makeCommandBuffer()!
  mm.encode(commandBuffer: cb, leftMatrix: A, rightMatrix: B, resultMatrix: C)
  cb.commit(); cb.waitUntilCompleted()
  acc.append(2 * Double(n * n * n) / (cb.gpuEndTime - cb.gpuStartTime) / 1e9)
}
print("gemm per 0.5 s:" + line)
