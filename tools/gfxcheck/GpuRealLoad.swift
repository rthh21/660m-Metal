// Read-only realistic GPU loads for the soft-max sweep: offscreen render (fragment ALU + texture + ROP)
// and an MPS SGEMM. Prints throughput only; compare across caps.
// Usage: GpuRealLoad render <aluIters> | gemm <n>
import Metal
import MetalPerformanceShaders
import Foundation

let a = CommandLine.arguments
let mode = a.count > 1 ? a[1] : "render"
let p = a.count > 2 ? Int(a[2])! : 16
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!

func measure(_ seconds: Double, _ body: () -> Double) -> [Double] {
  var r: [Double] = []
  for _ in 0..<3 { _ = body() }
  let t0 = DispatchTime.now().uptimeNanoseconds
  while Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9 < seconds { r.append(body()) }
  return r.sorted()
}

if mode == "render" {
  let src = """
  #include <metal_stdlib>
  using namespace metal;
  struct V { float4 pos [[position]]; float2 uv; };
  vertex V vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    V o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = p; return o;
  }
  fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                     constant int &iters [[buffer(0)]]) {
    float4 c = t.sample(s, in.uv * 3.0);
    float2 z = in.uv;
    for (int i = 0; i < iters; i++) {
      z = float2(z.x * z.x - z.y * z.y, 2 * z.x * z.y) + in.uv * 0.5;
      c += t.sample(s, z * 0.25 + c.xy * 0.01) * 0.01 + float4(sin(z.x), cos(z.y), z.x * z.y, 0) * 0.001;
    }
    return c;
  }
  """
  let lib = try! dev.makeLibrary(source: src, options: nil)
  let pd = MTLRenderPipelineDescriptor()
  pd.vertexFunction = lib.makeFunction(name: "vs"); pd.fragmentFunction = lib.makeFunction(name: "fs")
  pd.colorAttachments[0].pixelFormat = .rgba8Unorm
  pd.colorAttachments[0].isBlendingEnabled = true
  pd.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha; pd.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
  let pso = try! dev.makeRenderPipelineState(descriptor: pd)
  let w = 2560, h = 1600
  let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
  td.usage = [.renderTarget]; td.storageMode = .private
  let rt = dev.makeTexture(descriptor: td)!
  let sd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1024, height: 1024, mipmapped: false)
  let tex = dev.makeTexture(descriptor: sd)!
  var px = [UInt8](repeating: 0, count: 1024 * 1024 * 4); for i in 0..<px.count { px[i] = UInt8.random(in: 0...255) }
  tex.replace(region: MTLRegionMake2D(0, 0, 1024, 1024), mipmapLevel: 0, withBytes: px, bytesPerRow: 4096)
  let smp = dev.makeSamplerState(descriptor: { let d = MTLSamplerDescriptor(); d.minFilter = .linear; d.magFilter = .linear; d.sAddressMode = .repeat; d.tAddressMode = .repeat; return d }())!
  var iters = Int32(p)
  let r = measure(4) {
    let cb = q.makeCommandBuffer()!
    for _ in 0..<4 {
      let rp = MTLRenderPassDescriptor()
      rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = .clear; rp.colorAttachments[0].storeAction = .store
      let e = cb.makeRenderCommandEncoder(descriptor: rp)!
      e.setRenderPipelineState(pso); e.setFragmentTexture(tex, index: 0); e.setFragmentSamplerState(smp, index: 0)
      e.setFragmentBytes(&iters, length: 4, index: 0)
      e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
      e.endEncoding()
    }
    cb.commit(); cb.waitUntilCompleted()
    return 4 / (cb.gpuEndTime - cb.gpuStartTime)
  }
  print(String(format: "render %dx%d alu %d: median %.1f fps p10 %.1f p90 %.1f (n %d)", w, h, p, r[r.count/2], r[r.count/10], r[r.count*9/10], r.count))
} else {
  let n = p
  let desc = MPSMatrixDescriptor(rows: n, columns: n, rowBytes: n * 4, dataType: .float32)
  func mk() -> MPSMatrix { let b = dev.makeBuffer(length: n * n * 4, options: .storageModePrivate)!; return MPSMatrix(buffer: b, descriptor: desc) }
  let A = mk(), B = mk(), C = mk()
  let mm = MPSMatrixMultiplication(device: dev, transposeLeft: false, transposeRight: false, resultRows: n, resultColumns: n, interiorColumns: n, alpha: 1, beta: 0)
  let r = measure(4) {
    let cb = q.makeCommandBuffer()!
    mm.encode(commandBuffer: cb, leftMatrix: A, rightMatrix: B, resultMatrix: C)
    cb.commit(); cb.waitUntilCompleted()
    return 2 * Double(n) * Double(n) * Double(n) / (cb.gpuEndTime - cb.gpuStartTime) / 1e9
  }
  print(String(format: "sgemm %d: median %.0f Gflop/s p10 %.0f p90 %.0f (n %d)", n, r[r.count/2], r[r.count/10], r[r.count*9/10], r.count))
}
