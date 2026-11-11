// Request icons exactly like a Retina view would: N points at scale 2, in Aqua and DarkAqua -> PNGs.
import AppKit
let out = CommandLine.arguments[1]
let paths = Array(CommandLine.arguments.dropFirst(2))
for (pi, p) in paths.enumerated() {
  for appName in [NSAppearance.Name.aqua, .darkAqua] {
    NSAppearance(named: appName)!.performAsCurrentDrawingAppearance {
      for pt in [16, 20, 32, 64, 128] {
        let icon = NSWorkspace.shared.icon(forFile: p)
        icon.size = NSSize(width: pt, height: pt)
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pt * 2, pixelsHigh: pt * 2, bitsPerSample: 8, samplesPerPixel: 4,
                                   hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        rep.size = NSSize(width: pt, height: pt)  // 2x backing
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        icon.draw(in: NSRect(x: 0, y: 0, width: pt, height: pt), from: .zero, operation: .copy, fraction: 1)
        NSGraphicsContext.restoreGraphicsState()
        let f = "\(out)/icon\(pi)_\(appName == .aqua ? "light" : "dark")_\(pt)pt@2x.png"
        try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: f))
      }
    }
  }
  print("icon \(pi): \(p)")
}
