// TexUpload: checks that CPU-uploaded texture data reaches the GPU unchanged on the Radeon 660M.
// Paths: (A) replaceRegion into a managed texture -> GPU blit to buffer
//        (B) replaceRegion -> compute shader read() -> buffer        (tiled sampling path)
//        (C) buffer -> blit copyFromBuffer into private texture -> compute read() -> buffer
//        (D) IOSurface written by CPU -> MTLTexture -> compute read() -> buffer
//        (E) GPU render into managed texture -> synchronize -> getBytes (readback to CPU)
// Every byte is compared. Prints the first mismatching texel and a mismatch map summary.
// Read-only for the system: no kext, EFI, NVRAM or MMIO access.
import Foundation
import Metal
import IOSurface
import MachO

func patchXor0InProcess() {
  let pat: [UInt8] = [0x48,0x63,0x04,0x87,0x48,0x01,0xf8,0x8b,0x76,0x04,0x83,0xe6,0x07,0x8b,0x04,0xb0,0x80,0xc1,0x02,0xd3,0xe0,0x89,0x42,0x04]
  let rep: [UInt8] = [0x31,0xc0,0x0f,0x1f,0x40,0x00,0x66,0x90]; let repOff = 13
  var patched = 0
  for i in 0..<_dyld_image_count() {
    let name = String(cString: _dyld_get_image_name(i))
    guard name.contains("AMD"), let hdr = _dyld_get_image_header(i) else { continue }
    let slide = _dyld_get_image_vmaddr_slide(i)
    var cmd = UnsafeRawPointer(hdr).advanced(by: MemoryLayout<mach_header_64>.size)
    for _ in 0..<hdr.pointee.ncmds {
      let lc = cmd.load(as: load_command.self)
      if lc.cmd == LC_SEGMENT_64 {
        let seg = cmd.load(as: segment_command_64.self)
        let segname = withUnsafeBytes(of: seg.segname) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        if segname == "__TEXT" {
          let start = UnsafeMutablePointer<UInt8>(bitPattern: UInt(Int(seg.vmaddr) + slide))!
          let size = Int(seg.vmsize)
          var o = 0
          while o + pat.count <= size {
            if start[o] == 0x48 && memcmp(start + o, pat, pat.count) == 0 {
              let at = start + o + repOff
              let page = UInt(bitPattern: at) & ~UInt(vm_page_size - 1)
              var kr = mach_vm_protect(mach_task_self_, mach_vm_address_t(page), mach_vm_size_t(vm_page_size * 2), 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)
              if kr == KERN_SUCCESS { memcpy(at, rep, rep.count)
                kr = mach_vm_protect(mach_task_self_, mach_vm_address_t(page), mach_vm_size_t(vm_page_size * 2), 0, VM_PROT_READ | VM_PROT_EXECUTE) }
              patched += 1
              print("xor0: \(name.split(separator: "/").last ?? "") +0x\(String(o + repOff, radix: 16)) kr=\(kr)")
            }
            o += 1
          }
        }
      }
      cmd = cmd.advanced(by: Int(lc.cmdsize))
    }
  }
  print("xor0: patched \(patched) site(s) in-process")
}

let dev = MTLCreateSystemDefaultDevice()!
print("device: \(dev.name)  lowPower=\(dev.isLowPower) unified=\(dev.hasUnifiedMemory)")
// --xor0: in-process only (this test's own copy-on-write pages, nothing system-wide): patch addrlib
// Gfx10 HwlComputePipeBankXor in the loaded AMD UMD images so every surface gets pipeBankXor 0.
if CommandLine.arguments.contains("--xor0") { patchXor0InProcess() }
let q = dev.makeCommandQueue()!

let src = """
#include <metal_stdlib>
using namespace metal;
kernel void rd_u(texture2d<uint, access::read> t [[texture(0)]], device uint4 *o [[buffer(0)]],
                 uint2 g [[thread_position_in_grid]]) {
  if (g.x >= t.get_width() || g.y >= t.get_height()) return;
  o[g.y * t.get_width() + g.x] = t.read(g);
}
kernel void rd_f(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]],
                 uint2 g [[thread_position_in_grid]]) {
  if (g.x >= t.get_width() || g.y >= t.get_height()) return;
  o[g.y * t.get_width() + g.x] = t.read(g);
}
struct V { float4 p [[position]]; };
vertex V vs(uint i [[vertex_id]]) { float2 q[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; V v; v.p = float4(q[i],0,1); return v; }
fragment float4 fs(V v [[stage_in]]) { uint2 p = uint2(v.p.xy); return float4(float(p.x % 256)/255.0, float(p.y % 256)/255.0, float((p.x*7+p.y*13) % 256)/255.0, 1.0); }
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let psoU = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd_u")!)
let psoF = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd_f")!)

func pattern(_ n: Int, seed: Int) -> [UInt8] {
  var b = [UInt8](repeating: 0, count: n); var x = UInt32(truncatingIfNeeded: 2463534242 &+ seed)
  for i in 0..<n { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = UInt8(truncatingIfNeeded: x) }
  return b
}

struct Fmt { let pf: MTLPixelFormat; let name: String; let bpp: Int; let comps: Int; let isInt: Bool }
let fmts = [
  Fmt(pf: .rgba8Uint, name: "rgba8Uint", bpp: 4, comps: 4, isInt: true),
  Fmt(pf: .r8Uint, name: "r8Uint", bpp: 1, comps: 1, isInt: true),
  Fmt(pf: .rg8Uint, name: "rg8Uint", bpp: 2, comps: 2, isInt: true),
  Fmt(pf: .rgba16Uint, name: "rgba16Uint", bpp: 8, comps: 4, isInt: true),
  Fmt(pf: .bgra8Unorm, name: "bgra8Unorm", bpp: 4, comps: 4, isInt: false),
  Fmt(pf: .r8Unorm, name: "r8Unorm(A8-like)", bpp: 1, comps: 1, isInt: false),
]
let sizes = [(16,16), (37,23), (64,64), (128,128), (256,256), (300,200), (512,512), (1024,1024), (2048,2048)]

var fails = 0, total = 0
func report(_ tag: String, _ f: Fmt, _ w: Int, _ h: Int, _ exp: [UInt8], _ got: (Int, Int) -> [UInt8]) {
  total += 1
  var bad = 0; var first = ""
  var badRows = Set<Int>(); var badCols = Set<Int>()
  for y in 0..<h { for x in 0..<w {
    let e = Array(exp[(y*w+x)*f.bpp ..< (y*w+x+1)*f.bpp]); let g = got(x, y)
    if e != g { bad += 1; badRows.insert(y); badCols.insert(x)
      if first.isEmpty { first = "first bad (\(x),\(y)) exp \(e) got \(g)" } }
  } }
  if bad == 0 { print("PASS \(tag) \(f.name) \(w)x\(h)") }
  else { fails += 1; print("FAIL \(tag) \(f.name) \(w)x\(h): \(bad)/\(w*h) texels wrong, rows \(badRows.count), cols \(badCols.count); \(first)") }
}

// decode texel x,y from a GPU output buffer produced by rd_u / rd_f
func decoder(_ f: Fmt, _ buf: MTLBuffer, _ w: Int) -> (Int, Int) -> [UInt8] {
  if f.isInt {
    let p = buf.contents().bindMemory(to: UInt32.self, capacity: w * 4 * 4096)
    return { x, y in
      var r = [UInt8](); let base = (y*w+x)*4
      for c in 0..<f.comps { let v = p[base+c]
        if f.bpp / f.comps == 2 { r.append(UInt8(v & 0xff)); r.append(UInt8((v >> 8) & 0xff)) } else { r.append(UInt8(v & 0xff)) } }
      return r }
  } else {
    let p = buf.contents().bindMemory(to: Float.self, capacity: w * 4 * 4096)
    return { x, y in
      let base = (y*w+x)*4
      let v = (0..<4).map { UInt8(max(0, min(255, (p[base+$0] * 255.0).rounded()))) }
      if f.comps == 1 { return [v[0]] }
      if f.pf == .bgra8Unorm { return [v[2], v[1], v[0], v[3]] } // read() returns RGBA, memory is BGRA
      return v }
  }
}

func gpuRead(_ t: MTLTexture, _ f: Fmt) -> MTLBuffer {
  let w = t.width, h = t.height
  let out = dev.makeBuffer(length: w*h*16, options: .storageModeShared)!
  let cb = q.makeCommandBuffer()!; let ce = cb.makeComputeCommandEncoder()!
  ce.setComputePipelineState(f.isInt ? psoU : psoF); ce.setTexture(t, index: 0); ce.setBuffer(out, offset: 0, index: 0)
  ce.dispatchThreads(MTLSize(width: w, height: h, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
  ce.endEncoding(); cb.commit(); cb.waitUntilCompleted()
  if let e = cb.error { print("  GPU error: \(e)") }
  return out
}

for f in fmts { for (w, h) in sizes {
  let data = pattern(w*h*f.bpp, seed: w*7+h*3+f.bpp)
  // (A) managed texture + replaceRegion -> blit to buffer
  let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f.pf, width: w, height: h, mipmapped: false)
  td.storageMode = .managed; td.usage = [.shaderRead]
  let t = dev.makeTexture(descriptor: td)!
  data.withUnsafeBytes { t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w*f.bpp) }
  let lin = dev.makeBuffer(length: w*h*f.bpp, options: .storageModeShared)!
  do { let cb = q.makeCommandBuffer()!; let be = cb.makeBlitCommandEncoder()!
    be.copy(from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: w, height: h, depth: 1),
            to: lin, destinationOffset: 0, destinationBytesPerRow: w*f.bpp, destinationBytesPerImage: w*h*f.bpp)
    be.endEncoding(); cb.commit(); cb.waitUntilCompleted() }
  let lp = lin.contents().bindMemory(to: UInt8.self, capacity: w*h*f.bpp)
  report("A-upload-blit", f, w, h, data) { x, y in Array(UnsafeBufferPointer(start: lp + (y*w+x)*f.bpp, count: f.bpp)) }
  // (B) same texture sampled by a shader
  report("B-upload-shader", f, w, h, data, decoder(f, gpuRead(t, f), w))
  // (C) buffer -> private texture via blit -> shader
  let sb = data.withUnsafeBytes { dev.makeBuffer(bytes: $0.baseAddress!, length: data.count, options: .storageModeShared)! }
  let pd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f.pf, width: w, height: h, mipmapped: false)
  pd.storageMode = .private; pd.usage = [.shaderRead]
  let pt = dev.makeTexture(descriptor: pd)!
  do { let cb = q.makeCommandBuffer()!; let be = cb.makeBlitCommandEncoder()!
    be.copy(from: sb, sourceOffset: 0, sourceBytesPerRow: w*f.bpp, sourceBytesPerImage: w*h*f.bpp, sourceSize: MTLSize(width: w, height: h, depth: 1),
            to: pt, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
    be.endEncoding(); cb.commit(); cb.waitUntilCompleted() }
  report("C-bufblit-shader", f, w, h, data, decoder(f, gpuRead(pt, f), w))
} }

// (D) IOSurface (linear, CPU-written) -> texture -> shader; this is the CoreAnimation / WebKit backing-store path
for (w, h) in sizes {
  let f = fmts[4] // bgra8Unorm
  let props: [IOSurfacePropertyKey: Any] = [.width: w, .height: h, .bytesPerElement: 4, .pixelFormat: 0x42475241 /* 'BGRA' */]
  guard let s = IOSurface(properties: props) else { print("FAIL D IOSurface create \(w)x\(h)"); fails += 1; continue }
  let bpr = s.bytesPerRow
  let data = pattern(w*h*4, seed: w*11+h)
  s.lock(options: [], seed: nil)
  let base = s.baseAddress.assumingMemoryBound(to: UInt8.self)
  for y in 0..<h { data.withUnsafeBytes { memcpy(base + y*bpr, $0.baseAddress! + y*w*4, w*4) } }
  s.unlock(options: [], seed: nil)
  let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
  td.usage = [.shaderRead]; td.storageMode = .managed
  guard let t = dev.makeTexture(descriptor: td, iosurface: s, plane: 0) else { print("FAIL D texture from IOSurface \(w)x\(h)"); fails += 1; continue }
  report("D-iosurface-shader(bpr \(bpr))", f, w, h, data, decoder(f, gpuRead(t, f), w))
}

// (E) GPU render -> managed texture -> CPU getBytes
do {
  let rpd = MTLRenderPipelineDescriptor(); rpd.vertexFunction = lib.makeFunction(name: "vs"); rpd.fragmentFunction = lib.makeFunction(name: "fs")
  rpd.colorAttachments[0].pixelFormat = .rgba8Unorm
  let rps = try! dev.makeRenderPipelineState(descriptor: rpd)
  for (w, h) in sizes {
    let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
    td.storageMode = .managed; td.usage = [.renderTarget, .shaderRead]
    let t = dev.makeTexture(descriptor: td)!
    let pass = MTLRenderPassDescriptor(); pass.colorAttachments[0].texture = t; pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].storeAction = .store
    let cb = q.makeCommandBuffer()!; let re = cb.makeRenderCommandEncoder(descriptor: pass)!
    re.setRenderPipelineState(rps); re.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3); re.endEncoding()
    let be = cb.makeBlitCommandEncoder()!; be.synchronize(resource: t); be.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    var got = [UInt8](repeating: 0, count: w*h*4)
    got.withUnsafeMutableBytes { t.getBytes($0.baseAddress!, bytesPerRow: w*4, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0) }
    var exp = [UInt8](repeating: 0, count: w*h*4)
    for y in 0..<h { for x in 0..<w { let i = (y*w+x)*4; exp[i] = UInt8(x % 256); exp[i+1] = UInt8(y % 256); exp[i+2] = UInt8((x*7+y*13) % 256); exp[i+3] = 255 } }
    report("E-render-getBytes", Fmt(pf: .rgba8Unorm, name: "rgba8Unorm", bpp: 4, comps: 4, isInt: true), w, h, exp) { x, y in Array(got[(y*w+x)*4 ..< (y*w+x+1)*4]) }
  }
}
print("SUMMARY: \(total - fails)/\(total) PASS, \(fails) FAIL")
exit(fails == 0 ? 0 : 5)
