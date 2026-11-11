// Render file icons through IconServices (NSWorkspace) at cached-looking and odd pixel sizes -> PNGs.
import AppKit
let out = CommandLine.arguments[1]
let paths = Array(CommandLine.arguments.dropFirst(2))
let sizes: [Int] = [32, 64, 128, 256, 512, 1024, 77, 199, 333, 701]
for (pi, p) in paths.enumerated() {
  let icon = NSWorkspace.shared.icon(forFile: p)
  for s in sizes {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: s, pixelsHigh: s, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    icon.draw(in: NSRect(x: 0, y: 0, width: s, height: s), from: .zero, operation: .copy, fraction: 1)
    NSGraphicsContext.restoreGraphicsState()
    let f = "\(out)/icon\(pi)_\(s).png"
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: f))
  }
  print("icon \(pi): \(p)")
}
