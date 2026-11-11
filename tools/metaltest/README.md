# rmb_metaltest + RMB_CHECK.command (20261109)

Test Metal precompilat x86_64 pentru Lenovo Radeon 660M, construit offline în WSL fără SDK macOS
(`build.sh`: clang + LLVM `ld64.lld` cu stub-uri `.tbd` care conțin doar install-name și simbolurile
importate). Nu necesită Xcode/CLT pe laptop. Nu atinge kext-ul, MMIO, NVRAM sau EFI.

- `rmb_metaltest`: enumerare dispozitive Metal → compilare MSL la runtime → compute 1M float
  (`c = 2a + b`, fiecare element verificat pe CPU) → render 64×64 RGBA8 (clear albastru + triunghi
  roșu, readback prin blit, fiecare pixel din afara diagonalei verificat). Timeout 10 s per command
  buffer. Exit: 0 PASS, 1 fără device, 2 compilare/pipeline, 3 timeout, 4 status eroare GPU, 5 rezultate greșite.
- `RMB_CHECK.command`: copiază `/private/var/log/rmb-msgbuf.log` pe stick, salvează ioreg,
  system_profiler, log WindowServer/Metal, rapoarte panic, apoi rulează testul.

Stare: NOT TESTED pe macOS (nu există macOS în acest workspace). `stubs/*.c` nu sunt folosite
(cctools ld64 local nu are libtapi și refuză un libSystem stub ca dylib).
`tools/metal/MetalCheck.swift` rămâne varianta Swift (necesită swiftc pe macOS).
