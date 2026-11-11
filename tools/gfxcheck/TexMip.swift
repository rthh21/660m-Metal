// Mipmap consistency on this GPU: every level of a mip chain must round-trip between the CPU paths
// (managed replaceRegion / getBytes) and the GPU paths (blit, compute texture.read(lod), generateMipmaps).
import Metal
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void rd(texture2d<uint, access::read> t [[texture(0)]], device uint *o [[buffer(0)]],
               constant uint &lod [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
  uint w = t.get_width(lod), h = t.get_height(lod);
  if (g.x >= w || g.y >= h) return;
  uint4 v = t.read(g, lod);
  o[g.y * w + g.x] = v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd")!)
var pass = 0, fail = 0
func check(_ name: String, _ got: UnsafePointer<UInt32>, _ exp: [UInt32], tol: Int = 0) {
  var bad = 0, first = -1
  for i in 0..<exp.count {
    let a = got[i], b = exp[i]
    var ok = a == b
    if !ok && tol > 0 {
      ok = true
      for k in 0..<4 {
        let s = UInt32(k * 8)
        let ca = Int((a >> s) & 255), cb = Int((b >> s) & 255)
        if abs(ca - cb) > tol { ok = false }
      }
    }
    if !ok { bad += 1; if first < 0 { first = i } }
  }
  if bad == 0 { pass += 1 } else { fail += 1; print("FAIL \(name): \(bad)/\(exp.count) wrong, first at \(first)") }
}
func pattern(_ w: Int, _ h: Int, _ lvl: Int) -> [UInt32] {
  var r = [UInt32](repeating: 0, count: w * h)
  for i in 0..<(w * h) {
    let x = i % w, y = i / w
    let a = UInt32(x & 255), b = UInt32(y & 255) << 8
    let c = UInt32((x >> 8) | ((y >> 8) << 4)) << 16, d = UInt32(0x10 * (lvl + 1)) << 24
    r[i] = a | b | c | d
  }
  return r
}
let sizes: [(Int, Int)] = [(64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024), (200, 120), (300, 300), (2048, 2048)]
for (W, H) in sizes {
  let levels = Int(log2(Double(max(W, H)))) + 1
  // A: managed replaceRegion on every level -> GPU blit of every level -> buffer; and compute read(lod)
  let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Uint, width: W, height: H, mipmapped: true)
  d.storageMode = .managed; d.usage = [.shaderRead]
  let t = dev.makeTexture(descriptor: d)!
  var exp: [[UInt32]] = []
  for l in 0..<levels {
    let w = max(1, W >> l), h = max(1, H >> l); var p = pattern(w, h, l); exp.append(p)
    t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: l, withBytes: &p, bytesPerRow: w * 4)
  }
  for l in 0..<levels {
    let w = max(1, W >> l), h = max(1, H >> l)
    let b1 = dev.makeBuffer(length: w * h * 4, options: .storageModeShared)!
    let b2 = dev.makeBuffer(length: w * h * 4, options: .storageModeShared)!
    let cb = q.makeCommandBuffer()!
    let bl = cb.makeBlitCommandEncoder()!
    bl.copy(from: t, sourceSlice: 0, sourceLevel: l, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: w, height: h, depth: 1),
            to: b1, destinationOffset: 0, destinationBytesPerRow: w * 4, destinationBytesPerImage: w * h * 4)
    bl.endEncoding()
    let ce = cb.makeComputeCommandEncoder()!
    var lod = UInt32(l)
    ce.setComputePipelineState(pso); ce.setTexture(t, index: 0); ce.setBuffer(b2, offset: 0, index: 0); ce.setBytes(&lod, length: 4, index: 1)
    ce.dispatchThreads(MTLSize(width: w, height: h, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
    ce.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    check("A-blit \(W)x\(H) L\(l) (\(w)x\(h))", b1.contents().assumingMemoryBound(to: UInt32.self), exp[l])
    check("A-read \(W)x\(H) L\(l) (\(w)x\(h))", b2.contents().assumingMemoryBound(to: UInt32.self), exp[l])
  }
  // C: managed bgra8Unorm level 0 from CPU -> GPU generateMipmaps -> synchronize -> getBytes per level vs CPU 2x2 box
  let dc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: true)
  dc.storageMode = .managed; dc.usage = [.shaderRead]
  let tc = dev.makeTexture(descriptor: dc)!
  var l0 = [UInt32](repeating: 0, count: W * H)
  for i in 0..<(W * H) {
    let x = i % W, y = i / W
    let a = UInt32((x * 4) & 255), b = UInt32((y * 4) & 255) << 8
    let c = UInt32(((x / 16 + y / 16) & 1) * 255) << 16, d = UInt32(0xff) << 24
    l0[i] = a | b | c | d
  }
  tc.replace(region: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, withBytes: &l0, bytesPerRow: W * 4)
  let cb = q.makeCommandBuffer()!; let bl = cb.makeBlitCommandEncoder()!
  bl.generateMipmaps(for: tc); bl.synchronize(resource: tc); bl.endEncoding(); cb.commit(); cb.waitUntilCompleted()
  var prev = l0, pw = W, ph = H
  for l in 1..<min(levels, 6) {
    let w = max(1, W >> l), h = max(1, H >> l)
    var e = [UInt32](repeating: 0, count: w * h)
    for y in 0..<h { for x in 0..<w {
      var acc = [Int](repeating: 0, count: 4)
      for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
        let sx = min(pw - 1, x * 2 + dx), sy = min(ph - 1, y * 2 + dy), v = prev[sy * pw + sx]
        for k in 0..<4 { acc[k] += Int((v >> (k * 8)) & 255) }
      }
      var v: UInt32 = 0
      for k in 0..<4 { v |= UInt32((acc[k] + 2) / 4) << UInt32(k * 8) }
      e[y * w + x] = v
    } }
    var got = [UInt32](repeating: 0, count: w * h)
    tc.getBytes(&got, bytesPerRow: w * 4, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: l)
    // odd sizes use a different filter footprint; only compare power-of-two chains exactly(ish)
    if W & (W - 1) == 0 && H & (H - 1) == 0 { got.withUnsafeBufferPointer { check("C-genmip \(W)x\(H) L\(l)", $0.baseAddress!, e, tol: 3) } }
    prev = e; pw = w; ph = h
  }
}
print("SUMMARY: \(pass)/\(pass + fail) PASS, \(fail) FAIL")
