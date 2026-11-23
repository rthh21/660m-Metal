# Changelog

All releases target **macOS 26.7 build 25G229** only. Each version was tested on a Lenovo 82TL
(Ryzen 5 6600HS, Radeon 660M) before release.

## 1.1.0 (2026-11-23)

### Added

- **Experimental S3 sleep.** The kext powers the GPU down at sleep (PSP rings and TAs torn down, KIQ submits
  failed at once instead of timing out) and brings it back at wake (VBIOS POST first, stale TMR reloaded, kernel
  GART entries restored). Needs `SSDT-S3` and `SSDT-XHCWAKE`, both in `dist/acpi/` with their `.dsl`
  sources. See [Sleep (S3)](README.md#sleep-s3). Off switch: `-rmbnos3`.
- The panel stays at 120 Hz after a wake. Before, the PMFW came back at its default display clock and the panel
  refreshed at 72 Hz until the next reboot.
- `tools/s3guard`: a LaunchDaemon that turns sleep off and restarts cleanly if the GPU doesn't come back from a
  sleep, and turns sleep off at boot if the previous boot ended in the middle of one.
- New boot-args `-rmbnos3`, `-rmbnos3psp` and `-rmbnotmrfix`.

### Changed

- The ready-made kext in `dist/` is rebuilt from the current source. It is the first `dist/` build with these
  changes, which were only in the source before:
  - the end of the previous boot's log is saved to `/private/var/log/rmb-msgbuf.prev.log`;
  - the backlight smoother for the brightness keys and slider;
  - the VRAM cache trim (`vtrim`);
  - the display clock raised for the panel's 120 Hz mode, which before really refreshed at 72 Hz.
- The kext reports version 1.1.0 (before: 0.1.0).
- The timing generator fix is back to the one-OTG version. `tgfix` v2 (in 1.0) took the timing generator from the
  DAL. On the 82TL a display freeze and two GPU restarts followed it, and the cause wasn't isolated, so 1.1.0 ships
  the version that ran without a GPU hang since 2026-11-15.

### Known issues

- With an external monitor connected the kernel log gets about 750 `Invalid TG (-1)` lines a minute. They are
  harmless; that value isn't used for frame timing.
- Sleep: a USB-C hub wakes the laptop a few seconds into every sleep. Wake from real S3 by the keyboard, the power
  button or the lid isn't confirmed yet; wake by the RTC alarm is.

### Build

- `dist/Rembrandt660M.kext` is built with `VBIOS_ROM=` (no Lenovo/AMD VBIOS embedded; it reads the VBIOS from the
  ACPI VFCT table at boot). `make check`: 407 of 407 imports resolved.
- Apart from the version, the build date, the UUID and the VBIOS fallback, the code is byte-identical to the kext
  that ran on the 82TL during the tests.

## 1.0 (2026-11-15)

First release.

- Metal 3 acceleration on the Radeon 660M (Geekbench Metal around 19500).
- Hardware decode for H.264 and HEVC up to 4K; Safari and YouTube play smoothly.
- Internal panel at its full 3072×1920 resolution and 120 Hz.
- External monitor over a USB-C hub at 1920×1080 120 Hz, with hot plug.
- Brightness slider and F5/F6 keys.
