// BrgtSet: set the panel's 'brgt' display parameter (0..1024, served by Rembrandt660M from BL_PWM) through
// IOKit, the same way IODisplaySetIntegerParameter does, and read it back. Used to check the PWM path on Tahoe.
//   BrgtSet <value> [<seconds> <restore>]   e.g. BrgtSet 512 6 1024 = 50 % for 6 s, then back to 100 %
import Foundation
import IOKit

func panel() -> io_service_t {
    let s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleBacklightDisplay"))
    if s == 0 { print("no AppleBacklightDisplay"); exit(2) }
    return s
}

func brgt(_ s: io_service_t) -> String {
    guard let p = IORegistryEntryCreateCFProperty(s, "IODisplayParameters" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? [String: Any], let b = p["brgt"] as? [String: Any] else { return "brgt missing" }
    return "brgt value=\(b["value"] ?? "?") min=\(b["min"] ?? "?") max=\(b["max"] ?? "?")"
}

func set(_ s: io_service_t, _ v: Int32) {
    let r = IORegistryEntrySetCFProperty(s, "brgt" as CFString, NSNumber(value: v))
    print(String(format: "%@ set brgt %d -> 0x%x; now %@", ISO8601DateFormatter().string(from: Date()), v, r, brgt(s)))
}

let a = CommandLine.arguments
guard a.count >= 2, let v = Int32(a[1]) else { print("usage: BrgtSet <0..1024> [seconds restore]"); exit(1) }
let s = panel()
print("before: \(brgt(s))")
set(s, v)
if a.count >= 4, let t = Double(a[2]), let back = Int32(a[3]) {
    Thread.sleep(forTimeInterval: t)
    set(s, back)
}
IOObjectRelease(s)
