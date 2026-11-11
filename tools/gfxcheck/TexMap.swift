// TexMap: upload a texture whose texels hold their own (x,y), read it back through the GPU,
// and report the coordinate mapping (dx = x ^ srcX, dy = y ^ srcY) for every failing surface.
// A single constant (dx,dy) per 64KB block = pipe/bank XOR mismatch, not a pitch/stride bug.
import Foundation
import Metal
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let cases: [(MTLPixelFormat, Int, String, Int, Int)] = [
  (.rgba16Uint, 8, "rgba16Uint", 64, 64), (.rgba16Uint, 8, "rgba16Uint", 96, 96), (.rgba16Uint, 8, "rgba16Uint", 128, 128),
  (.rgba16Uint, 8, "rgba16Uint", 256, 256), (.rgba16Uint, 8, "rgba16Uint", 300, 200), (.rgba16Uint, 8, "rgba16Uint", 512, 512),
  (.rgba16Uint, 8, "rgba16Uint", 1024, 1024),
]
for (pf, bpp, name, w, h) in cases {
  for rep in 0..<3 {
    var data = [UInt16](repeating: 0, count: w*h*4)
    for y in 0..<h { for x in 0..<w { let i = (y*w+x)*4; data[i] = UInt16(x); data[i+1] = UInt16(y); data[i+2] = 0xA5A5; data[i+3] = UInt16(rep) } }
    let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: pf, width: w, height: h, mipmapped: false)
    td.storageMode = .managed; td.usage = [.shaderRead]
    let t = dev.makeTexture(descriptor: td)!
    data.withUnsafeBytes { t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w*bpp) }
    let lin = dev.makeBuffer(length: w*h*bpp, options: .storageModeShared)!
    let cb = q.makeCommandBuffer()!; let be = cb.makeBlitCommandEncoder()!
    be.copy(from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: w, height: h, depth: 1),
            to: lin, destinationOffset: 0, destinationBytesPerRow: w*bpp, destinationBytesPerImage: w*h*bpp)
    be.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    let p = lin.contents().bindMemory(to: UInt16.self, capacity: w*h*4)
    var hist = [String: Int](); var foreign = 0, zero = 0, ok = 0
    for y in 0..<h { for x in 0..<w { let i = (y*w+x)*4
      if p[i+2] != 0xA5A5 { if p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 { zero += 1 } else { foreign += 1 }; continue }
      let sx = Int(p[i]), sy = Int(p[i+1]); if sx == x && sy == y { ok += 1 }
      let k = "dx=\(x ^ sx) dy=\(y ^ sy)"; hist[k, default: 0] += 1 } }
    let top = hist.sorted { $0.value > $1.value }.prefix(6).map { "\($0.key):\($0.value)" }.joined(separator: "  ")
    print("\(name) \(w)x\(h) #\(rep) (\(w*h*bpp/1024) KB): ok \(ok)/\(w*h) zero \(zero) foreign \(foreign) | \(top)")
  }
}
