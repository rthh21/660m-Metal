<div align="center">

# 660m-Metal

### macOS Tahoe on an AMD Radeon 660M, with full Metal acceleration

![macOS](https://img.shields.io/badge/macOS-Tahoe%2026.7%20(25G229)-000000?logo=apple&logoColor=white)
![GPU](https://img.shields.io/badge/GPU-Radeon%20660M%20Rembrandt-ED1C24?logo=amd&logoColor=white)
![CPU](https://img.shields.io/badge/CPU-Ryzen%205%206600HS-ED1C24?logo=amd&logoColor=white)
![Metal](https://img.shields.io/badge/Metal-supported-success)
![Bootloader](https://img.shields.io/badge/OpenCore-1.0.8-blue)
![Lilu](https://img.shields.io/badge/Lilu-plugin-8A2BE2)
![License](https://img.shields.io/badge/license-GPL--2.0-lightgrey)

[![Stars](https://img.shields.io/github/stars/rthh21/660m-Metal?style=for-the-badge&logo=github&label=stars&color=f5c518&labelColor=1f2328)](https://github.com/rthh21/660m-Metal/stargazers)
[![Forks](https://img.shields.io/github/forks/rthh21/660m-Metal?style=for-the-badge&logo=github&color=8A2BE2&labelColor=1f2328)](https://github.com/rthh21/660m-Metal/forks)
[![Last commit](https://img.shields.io/github/last-commit/rthh21/660m-Metal?style=for-the-badge&logo=git&logoColor=white&color=2ea44f&labelColor=1f2328)](https://github.com/rthh21/660m-Metal/commits/main)

*Apple never shipped a driver for AMD Rembrandt. This project makes Apple's own Navi 23 driver stack run on it.*

</div>

---

**Contents:**
[What this is](#what-this-is) ·
[What works](#what-works) ·
[Install on another laptop](#install-on-another-laptop) ·
[Install on the Lenovo 82TL](#install-on-the-lenovo-82tl) ·
[External display](#external-display-through-a-usb-c-hub) ·
[GPU clock](#gpu-clock) ·
[Video decode](#hardware-video-decode) ·
[How it works](#how-it-works) ·
[Boot-args](#boot-args) ·
[Building](#building) ·
[EFI reference](#opencore-efi-reference-lenovo-82tl) ·
[Star history](#star-history) ·
[License](#license)

## What this is

**Rembrandt660M** is a [Lilu](https://github.com/acidanthera/Lilu) plugin that runs Apple's Navi 23 graphics stack
(`AMDRadeonX6000`, `AMDRadeonX6000Framebuffer`, HWLibs and the AMD user-mode driver) on the **Radeon 660M**.
The 660M is an RDNA2 APU (GC 10.3.3, DCN 3.1.2, PSP 13, SMU 13), and Apple's kexts don't support it.
The plugin patches the differences while the system runs: register layout, firmware, SMU power messages, the
display engine, panel timings and the UMD tiling. It doesn't replace Apple's kexts.

It was developed and tested bare metal on a **Lenovo laptop, machine type 82TL** (Ryzen 5 6600HS, 3072×1920
120 Hz panel), booted by OpenCore. The kext in `dist/` works on that laptop as it is. Other 660M laptops need a
rebuild with their own VBIOS values; [Install on another laptop](#install-on-another-laptop) walks through it.
This repository holds only the graphics implementation, not a full EFI.

---

## What works

| Feature | Status | Notes |
|---|:-:|---|
| **Metal / GPU acceleration** | Works | Every Metal, OpenCL, OpenGL, MPS and ray-tracing path tested passes |
| **Internal panel 3072×1920 @ 120 Hz** | Works | Bad EDID DisplayID block repaired over AUX (`edidfix`), timing generator fix (`tgfix`) |
| **Brightness slider + F5/F6 keys** | Works | Slider with `dist/RmbPanel660M.kext`; keys with `SSDT-BRTKEY` (Lenovo EC, see the [EFI reference](#opencore-efi-reference-lenovo-82tl)) |
| **External monitor through a USB-C hub** | Works | DP alt mode → HDMI, 1920×1080 @ 120 Hz extended, plugged at boot or later (see [below](#external-display-through-a-usb-c-hub)) |
| **Safari / text and icon rendering** | Works | AMD UMD `pipeBankXor` forced to 0 (`umdpatch`, `-rmbumd`) |
| **Fast boot to the desktop** | Works | 6.3 s DAL SMU mailbox timeout removed, login window about 11 s after the kernel starts |
| **Smooth cursor** | Works | Fixes the 2 s cursor steps (`xhciintr`: xHCI interrupter 1 on a single MSI) |
| **Hardware video decode (VCN)** | Works | H.264 and HEVC (8 and 10 bit) up to 4K on VCN 3.1, Safari playback with 0 frames dropped. Encode stays in software, VP9 and AV1 are not available (see [below](#hardware-video-decode)) |
| **GPU and memory clock** | Works | `gfxclk` keeps the GPU clock under the PMFW current limiter's trip point and raises FCLK to 1600 MHz while the GPU is busy (see [below](#gpu-clock)) |
| **VRAM** | Partial | 2 GB carve-out (the BIOS UMA size) |
| **Sleep** | No | The laptop only reaches DarkWake, not S3 |

The kext also carries two small non-graphics helpers that the same boot depends on: the xHCI interrupter fix
above and `wlanfix` (turns off 10-bit tags on the root port of an MT7922 Wi-Fi card before macOS first reads it).

> [!WARNING]
> The kext only works on **macOS 26.7 build 25G229**. Its hooks use fixed addresses in that kernel collection,
> so any other build (including a minor update) needs a new symbol manifest. Turn off automatic updates.

---

## Install on another laptop

This is the path for any laptop with a Radeon 660M other than the Lenovo 82TL. It has **not been tested on
other hardware yet**: expect to read logs. Plan on an afternoon.

### What you need

| | |
|---|---|
| **GPU** | Radeon 660M, PCI ID `1002:1681` (Ryzen 5 6600H/HS/U and similar). The Radeon 680M has the same PCI ID but twice the compute units and was never run with this kext |
| **macOS** | Tahoe 26.7 build **25G229** exactly |
| **A working hackintosh without acceleration** | OpenCore with the AMD Vanilla kernel patches, a USB map, keyboard and storage working. Without this kext macOS boots on the firmware framebuffer, slow but usable |
| **A Linux live USB** | Used once, to read the VBIOS and the memory size (any recent Ubuntu or Fedora) |
| **Build tools** | Xcode Command Line Tools, the 25G229 Kernel Debug Kit and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK). The laptop itself can build, running macOS without acceleration |

### Why the kext must be rebuilt

The kext has two safety checks that stop it on unknown hardware:

1. **The VBIOS hash.** Apple's driver needs two tables (a PSP directory and `vram_info`) that a Rembrandt VBIOS
   doesn't have. The kext reads the VBIOS from the ACPI `VFCT` table at boot, adds both tables, and only
   accepts a VBIOS whose length and hash are in `vbios_augment.hpp`. Your laptop has another VBIOS, so it needs
   other values.
2. **The PCI IDs.** The register mapping accepts only the Lenovo GPU (subsystem `17AA:3B1E`, revision `03`).

You will change those values and build. Nothing else in the source should need to change.

### Step 1: collect the GPU data in Linux

Boot the Linux live USB and run:

```sh
lspci -nnv -d 1002:1681 | head -3                     # note "(rev NN)" and "Subsystem: ... [vvvv:dddd]"
sudo cat /sys/firmware/acpi/tables/VFCT > VFCT.bin   # the VBIOS, as the kext will read it
sudo dmesg | grep -E "amdgpu.*(VRAM:|RAM width)"      # e.g. "VRAM: 2048M" and "RAM width 128bits"
```

Copy `VFCT.bin` somewhere macOS can read it, and write down the subsystem, the revision, the VRAM size in MB and
the RAM width in bits.

The VRAM size is the **UMA frame buffer size** set in the BIOS. If you change it later, redo steps 2 and 4.

> [!NOTE]
> `VFCT.bin` contains your laptop vendor's VBIOS. Keep it out of any public repository.

### Step 2: compute the VBIOS values

```sh
python3 tools/port_vbios.py VFCT.bin --vram-mb 2048 --bus-width 128
```

Use your own values from step 1. The tool runs the same augmentation as the kext and prints five lines:

```
constexpr size_t kRawLen = 0x....;
constexpr size_t kOutLen = 0x....;
constexpr uint64_t kRawFnv = 0x................ULL;
constexpr uint64_t kOutFnv = 0x................ULL;
constexpr uint32_t kVramMb = ..., kBusWidth = ...;
```

Replace the matching five lines at the top of `src/Rembrandt660M/vbios_augment.hpp`. On the 82TL the tool prints
exactly the values already there, which is how you can check the tool. If it stops with "master data table slot
... is already used" or "over 64 KiB", your VBIOS is laid out differently and the augmentation must be adapted
first. Open an issue with the tool's output.

### Step 3: accept your GPU's PCI IDs

```sh
grep -n "0x3b1e\|0x3B1E" src/Rembrandt660M/mmio.cpp src/Rembrandt660M/kern_start.cpp
```

This finds three checks. Each compares the subsystem vendor (`0x17aa`), the subsystem ID (`0x3b1e`) and the
revision (`3`). Change all three to your values from step 1:

| Where | What it gates |
|---|---|
| `mmio.cpp`, `lenovoGpu()` | Mapping the GPU registers. Without it the kext does nothing |
| `mmio.cpp`, the VRAM lookup below it | Access to the VRAM aperture |
| `kern_start.cpp`, `idOk` | Only the optional embedded fallback VBIOS (unused unless you build with `VBIOS_ROM=`) |

### Step 4: build

Follow [Building](#building). `make check` must print `RESULT: PASS`. No VBIOS file is embedded; the kext reads
yours from ACPI at boot.

### Step 5: set up OpenCore

Only the GPU-related parts are listed here; the rest is a normal AMD laptop EFI
([Dortania guide](https://dortania.github.io/OpenCore-Install-Guide/)). The
[EFI reference](#opencore-efi-reference-lenovo-82tl) shows the full 82TL setup.

1. **Kernel → Add**: `Lilu.kext` first, **your** `Rembrandt660M.kext` directly after it, then `VirtualSMC.kext`
   and the rest. Do not load `NootedRed` or any other Radeon kext next to it.
2. **DeviceProperties → Add** on the GPU. Find its path with `gfxutil`; on Rembrandt it is usually
   `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x0)`:

   | Key | Type | Value |
   |---|---|---|
   | `device-id` | Data | `FF730000` |
   | `compatible` | String | `pci1002,73ff` |
   | `model` | String | `AMD Radeon 660M` |
   | `LoadAccelerator`, `LoadController`, `LoadHWServices` | Boolean | true |
   | `CFG_USE_AGDC` | Data | `0000000000000000` |
   | `CFG_NO_BOOT_PWDN` | Data | `0100000000000000` |
   | `AAPL,iokit-ignore-ndrv` | Data | `01000000` |

   Do not add `ATY,bin_image`: on 25G229 it never reaches the driver, which is why the kext builds the image
   itself. Do not copy an `AAPL00,override-no-connect` from someone else's EFI; it is a panel's EDID.
3. **PlatformInfo**: SMBIOS `iMac20,1` with your own serials.
4. **NVRAM → boot-args**: `-lilubetaall -rmbumd -v keepsyms=1`. `-rmbumd` is required, without it Safari text
   and icons are scrambled. Drop `-v` once everything works.

Boot from a USB stick with this EFI first. Keep your previous EFI on the internal disk until the stick works.

### Step 6: check the first boot

The kext writes its log to `/private/var/log/rmb-msgbuf.log` (readable without sudo). After the boot:

```sh
grep -a "\[vbios\]" /private/var/log/rmb-msgbuf.log | tail -3
system_profiler SPDisplaysDataType | grep -E "Chipset|VRAM|Metal"
```

A good boot shows `ATY,bin_image set from ACPI VFCT (0x... bytes, fnv ...)` and `Metal: Supported`:

| What you see | Cause | What to do |
|---|---|---|
| `VFCT ... rejected (raw image is not the verified Lenovo VBIOS)` | Wrong values in `vbios_augment.hpp` | Redo step 2 with the `VFCT.bin` of this laptop and the same BIOS settings |
| No `[vbios]` line, no acceleration | The PCI ID checks don't match | Redo step 3; compare with `lspci` |
| Black screen or hang after the Apple logo | A display fix that doesn't fit your laptop | Boot with `-rem660off` (whole kext off) to get back in, then try the feature boot-args below one at a time |
| Scrambled text and icons | `-rmbumd` missing | Add it to boot-args |
| Panel stays at 60 Hz | EDID problem | `grep -a edidfix /private/var/log/rmb-msgbuf.log` |

When you ask for help, attach `rmb-msgbuf.log` and the output of the commands above.

### Step 7: laptop-specific features

| Part | On your laptop |
|---|---|
| Internal panel (eDP, link 0) | Generic, should work. `edidfix` only acts when an EDID block has a bad checksum |
| Hardware video decode | Generic. Run the two `defaults write` commands from [Hardware video decode](#hardware-video-decode) once |
| External displays (`extbind`) | DisplayPort over USB-C by default; hot plug on the first USB-C DP port. For a native HDMI port add `-rmbextbindhdmi` (untested) |
| GPU clock (`gfxclk`) | The `auto` table was measured on the 82TL's 6600HS. If the GPU hangs under load, write `pmfw cap=1600` to `/Users/Shared/rmb-gfxclk.ctl`, or boot with `-rmbnogfxclk` |
| `wlanfix` | Only acts on an MT7922 Wi-Fi card (`14c3:0616`). `-rmbnowlanfix` turns it off |
| `xhciintr` | Fixes cursor stutter with Apple's xHCI driver on a single MSI. `-rmbxhciintr1` turns it off |
| Brightness slider | Needs the panel injector with **your** panel's ID, see below |
| Brightness keys | Depend on the laptop's EC; the 82TL's `SSDT-BRTKEY` is Lenovo-specific |

**Brightness slider.** Apple's backlight driver looks up the panel by ID and leaves the slider off when it finds
no entry. `dist/RmbPanel660M.kext` is a codeless kext (only an `Info.plist`) that supplies one:

1. Read your panel's product ID in macOS and convert it to 4 hex digits:
   ```sh
   ioreg -lw0 | grep -m1 '"DisplayProductID"'   # e.g. 2728
   printf '%04x\n' 2728                         # -> 0aa8
   ```
2. In `dist/RmbPanel660M.kext/Contents/Info.plist`, rename the `ApplePanels` key `F10T0aa8` to `F10T<your id>`.
   `10` is the `_UID` of the `PNLF` device from `SSDT-PNLF`; change it if yours is different.
3. Add the kext to `Kernel → Add` (no executable, `PlistPath` = `Contents/Info.plist`) together with an
   `SSDT-PNLF` for AMD.

---

## Install on the Lenovo 82TL

The kext in `dist/` works on this laptop as it is.

1. Install **macOS 26.7 build 25G229** and turn off automatic updates.
2. Build an OpenCore EFI as shown in the [EFI reference](#opencore-efi-reference-lenovo-82tl), with
   `dist/Rembrandt660M.kext` and `dist/RmbPanel660M.kext`.
3. For hardware video decode, run the two `defaults write com.apple.AppleGVA` commands from
   [Hardware video decode](#hardware-video-decode) once.
4. Boot from a USB stick first and switch to the internal EFI only once that works.

---

## External display through a USB-C hub

The 82TL has no HDMI port. The monitor is connected through a USB-C hub (DisplayPort alt mode → HDMI converter)
on DCN link 1.

**The problem:** under the `iMac20,1` SMBIOS, `displaypolicyd` never commits a link to a framebuffer, so the hub's
monitor was detected but never got a framebuffer.

**The fix (`extbind`):** the internal panel works because its link is *fixed*: the framebuffer driver binds it to
FB:0 itself. `extbind` does the same for FB:1. It hooks
`AmdRadeonFramebuffer::initializeFixedDisplayBinding` and binds the connected external DP link to FB:1 the same
way the original code binds the panel to FB:0. It also marks the link as fixed, so plugging and unplugging go
through the driver's own connection-change path.

**Hot plug:** when nothing is connected at boot, FB:1 is bound in advance to the first USB-C DP port (link 1),
with the display marked as disconnected. Plugging the hub in later raises the port's HPD interrupt, the driver
redetects the link and sends a connection change to every framebuffer, and FB:1 comes online with the monitor's
EDID. Use the same USB-C port each time (link 1). `-rmbnoextprebind` goes
back to binding only a monitor that is connected at boot.

| | |
|---|---|
| **Crash guard** | Writes `pending` to `/private/var/log/rmb-extbind.state` before binding and `ok` after 60 s (30 s when nothing is connected). Each later plug-in writes `pending: hotplug N` and `ok` 30 s later or on unplug. A boot that finds `pending` writes `blocked` and skips the binding. Re-enable with `touch /Users/Shared/rmb-extdp-arm` |
| **Limits** | Hot plug works on one port only (the one bound at boot). Closing the lid may also blank the external display. The hub's DP link is HBR2 × 2 lanes (1080p120 works; about 1440p60 / 4K30 is the ceiling) |
| **Fail-safe** | `-rmbnoextbind` turns it off; `-rmbnoextprebind` binds only a monitor present at boot |
| **Native HDMI port** | Off by default and **untested** (the 82TL has no HDMI port). `-rmbextbindhdmi` lets `extbind` also bind a native HDMI port, for other 660M laptops |

---

## GPU clock

macOS has no PowerPlay for this APU, so nothing tells the PMFW (SMU 13.0.1) its GFX clock limits.
`gfxclk` sends them the way Linux does (`SetHardMinGfxClk`, `SetSoftMaxGfxClk`), from a kernel thread that
starts once the SMU session is live.

**What limits the clock:** under light load the GFX clock does reach the 1900 MHz maximum. Under heavy shader
load the PMFW current limiter trips as soon as the soft max is above a threshold. It then holds the clock at
about 60% of the soft max, so a 1900 MHz soft max gives *less* throughput than 1600 MHz:

| SGEMM 1024 (MPS) | soft max 1450 | 1550 | 1600 | 1650 | 1900 |
|---|:-:|:-:|:-:|:-:|:-:|
| GPU only | – | – | **1074** Gflop/s | 836 | 806 |
| + 1 busy CPU thread | 988 | **1045** | 831 | – | 806 |
| + 2 busy CPU threads | **980** | 799 | 723 | – | 806 |
| + 4 busy CPU threads | 581 (1400) | – | – | – | **757–805** |

The CPU cores and the GPU share the VDD rail, so the trip point drops as CPU load rises. With three or more
busy threads every soft max trips, and the highest one is the best. Trip and recovery follow a change of the
soft max or of the CPU load within 0.5 s.

**The fix (`auto` mode, default):** every 100 ms `gfxclk` counts the busy logical CPUs (`host_statistics`,
peak of the last 300 ms) and picks the soft max:

| Busy CPUs | < 0.6 | < 1.6 | < 2.6 | more |
|---|:-:|:-:|:-:|:-:|
| Soft max (MHz) | 1600 | 1600 | 1425 | 1900 |

The hard min stays at 200 MHz, so the PMFW still idles the GPU. Only the soft max moves, always inside the
PMFW DPM range.

Measured with `auto` (AC): SGEMM 1024 gives 1032–1074, 1032, 964 and 805 Gflop/s with 0, 1, 2 and 4+ busy CPU
threads, which matches the best fixed soft max in every case. The FMA probe runs at 1408 MHz effective, against
1042 with a fixed 1900 MHz soft max. The cost: a light, latency-bound shader peaks at 1600 instead of 1900 MHz
while the CPU is idle.

**Memory clock (FCLK boost):** left to the PMFW, the data fabric clock (FCLK) stays at 1200 MHz under GPU
load, although the PMFW's top level is 1600 MHz (LPDDR5-6400). In `auto` mode `gfxclk` samples
`GRBM_STATUS.GUI_ACTIVE` every 100 ms. Once 4 of the last 8 samples are busy it sends
`SetHardMinFclkByFreq 1600`, and it releases FCLK 15 s after fewer than 2 of 8 are busy (shorter holds dropped FCLK in the pauses between Geekbench workloads). The desktop keeps the GPU
busy 2–5 % of the time (two displays at 120 Hz), so FCLK is only held while the GPU really works.

| Geekbench 7 Metal (AC, two displays) | Score |
|---|:-:|
| `auto`, FCLK left to the PMFW (1200 MHz under load) | 16333 |
| `auto`, FCLK held at 1600 MHz | **19302** |

| | |
|---|---|
| **Control file** | `/Users/Shared/rmb-gfxclk.ctl`, read at boot and every second. Modes `auto` (default), `pmfw` (fixed `cap=`), `boost`, `max`. Keys `floor= cap= cap1= cap2= cap3=` (MHz), `fclkboost=` (MHz, `0` turns the FCLK boost off), `fclkmin=` (fixed FCLK floor, turns the boost off), `trace` (one log line per second) |
| **Crash guard** | Writes `pending` to `/private/var/log/rmb-gfxclk.state` before the first limit and after each policy change, `ok` 60 s later. A boot that finds `pending` only measures until the control file is written again |
| **Messages** | Only GetSmuVersion, GetEnabledSmuFeatures, GetGfxclkFrequency, SetSoftMaxGfxClk, SetHardMinGfxClk, GetFclkFrequency, SetHardMinFclkByFreq (and the SOCCLK pair, opt-in). All of them are messages Linux amdgpu sends too. No GfxOff, reset or power-limit message |
| **Kill switch** | `-rmbnogfxclk` starts no thread at all |
| **Probes** | `tools/gfxcheck/` (Metal FMA, SGEMM, render and bandwidth probes used for the tables above) |

---

## Hardware video decode

Out of the box the video engine (VCN) hangs the whole GPU, so earlier builds hid it and VideoToolbox decoded in
software. `vcn` fixes the three causes, each checked against the Linux driver:

1. **Firmware.** HWLibs asks for `ativvaxy_vcn3_1.dat` (VCN 3.1) and macOS has none, so the PSP never loads a VCN
   image. The kext serves Rembrandt's PSP-signed VCN firmware (from `yellow_carp_vcn.bin`) under that name.
2. **Power.** The PMFW keeps VCN and JPEG power gated until `PowerUpVcn` / `PowerUpJpeg`. Apple's PowerPlay sends
   nothing on this APU. The kext sends `PowerUpVcn`/`PowerDownVcn` and `PowerUpJpeg`/`PowerDownJpeg` exactly
   like Linux, when HWLibs ungates or gates the engine. Apple also gates a second VCN instance (Navi 2x has two);
   Rembrandt has one, so those calls are ignored.
3. **MMHUB.** The Navi 23 driver describes the multimedia hub with MMHUB 2.1 register offsets. Rembrandt has
   MMHUB 2.3, whose VM registers sit at other offsets on another segment, so the first invalidation never
   finished. The kext rewrites the hub's register table with the MMHUB 2.3 map (`tests/test_vcn.py` checks
   every offset against Linux `mmhub_2_3_0_offset.h`).

The accelerator also publishes Apple's Navi 23 HEVC decode capabilities.

**One user setting.** Under the `iMac20,1` SMBIOS, AppleGVA looks for an Intel iGPU for video and finds none.
Tell it to use the AMD decoder once per user (no sudo, undo with `defaults delete com.apple.AppleGVA`):

```
defaults write com.apple.AppleGVA gvaForceAMDAVCDecode -bool YES
defaults write com.apple.AppleGVA gvaForceAMDHEVCDecode -bool YES
```

Measured on the 82TL (VideoToolbox, decoder required to be hardware, every frame compared with what was encoded):

| Codec | Frames | Correct | Speed |
|---|:-:|:-:|:-:|
| H.264 1280×720 | 600 | 600 | 185 fps |
| H.264 1920×1080 | 120 | 120 | 171 fps |
| H.264 3840×2160 | 60 | 60 | 75 fps |
| HEVC Main 1280×720 | 300 | 300 | 207 fps |
| HEVC Main 1920×1080 | 120 | 120 | 126 fps |
| HEVC Main 3840×2160 | 60 | 60 | 64 fps |
| HEVC Main10 1920×1080 | 90 | 90 | 153 fps |
| 4 H.264 sessions at once | 4 × 120 | all | ~300 fps each |
| 3 HEVC sessions at once | 3 × 120 | all | – |
| Safari playback | – | 0 frames dropped | – |

The PMFW powers VCN up when a decode session starts and down about 3 s after it ends. 15 start/stop cycles in a
row: every PMFW message answered OK, no power failure.

**Limits.**

- **Encode** stays in software. The accelerator does not publish the encode capabilities, so a request for a
  hardware encoder fails cleanly and apps (FaceTime, QuickTime, screen recording) use the software encoder.
  Apple's Navi driver has an H.264/HEVC encoder, so this is possible future work.
- **VP9 and AV1** can't be decoded in hardware. VCN 3.1 supports them, but Apple's AMD video driver only knows
  H.264 and HEVC. Streams in those formats play with software decoding, or the site sends H.264 instead.
- **DRM playback** (Netflix, Apple TV+ in Safari) is not tested.

| | |
|---|---|
| **Control file** | `/Users/Shared/rmb-vcn.ctl`: `on` (default, also when the file is missing), `probe` (firmware, MMHUB and power fixes, but VideoToolbox stays in software), `off`. Read at boot; switching between `on` and the others while running shows or hides the decoder for apps started afterwards |
| **Crash guard** | Off by default (VCN proved stable). With the `-rmbvcnguard` boot-arg it writes `pending` to `/private/var/log/rmb-vcn.state` at boot and before VCN starts, `ok` once the boot is 90 s old and 60 s have passed since the last start; a boot that finds `pending` runs with VCN off and writes `blocked`. Write the control file again to re-arm |
| **Messages** | Only PowerUpVcn 0x07, PowerDownVcn 0x06, PowerUpJpeg 0x22, PowerDownJpeg 0x21 (argument 0), only while the SMU session is live |
| **Kill switch** | `-rmbnovcnfix` installs no hook and keeps VCN off |
| **Probe** | `tools/vtdecode/VtDecode.swift`: `VtDecode h264\|hevc hw [frames]` |

---

## How it works

```
OpenCore ──► Lilu ──► Rembrandt660M.kext
                         │
                         ├─ static RVA manifest (25G229) ── verified against the KDK by `make check`
                         ├─ AMDRadeonX6000 (accel)       ── GC/SDMA/PSP/SMU differences, firmware, power
                         ├─ AMDRadeonX6000Framebuffer    ── DCN 3.1.2 register remap, link/panel/EDID fixes
                         ├─ AMD UMD (user space)         ── pipeBankXor via code-signing page validation
                         └─ VBIOS from ACPI VFCT         ── augmented in memory, hash-checked (vbios_augment.hpp)
```

- **VBIOS.** On 25G229 Apple's driver can't read the VBIOS from ACPI and falls back to the ROM BAR, which fails.
  The kext reads the laptop's own `VFCT` table through the ACPI platform expert, adds the PSP directory and
  `vram_info` table Apple's driver needs, and sets `ATY,bin_image` only if the result matches the verified image.
  No VBIOS is stored in this repository or in the built kext.
- **Static RVA manifest.** Every hooked function is pinned to its address in the exact 25G229 kernel
  collection (`tahoe_symbols_25G229.hpp`). `tools/verify_symbol_manifest.py` checks each name against the KDK
  and checks that every hook's first instructions are safe to patch with a short jump.
- **Import audit.** `tools/check_imports.py` checks that every symbol the kext imports resolves in the real
  kernel and in Lilu (`audit/import_authority.json`).
- **Source contracts.** `tests/` hold offline tests that check the order and guards of risky code. They don't
  need the hardware.
- **Kill switches.** Each feature has a boot-arg that turns it off, so a bad change never needs a new build to
  get back into macOS.

---

## Boot-args

| Boot-arg | Effect |
|---|---|
| `-rmbumd` | **Turn on** the UMD `pipeBankXor` fix (needed for correct text and icons) |
| `-rmbnoextbind` | Turn off the external display binding through the USB-C hub |
| `-rmbnoextprebind` | Bind the USB-C port only when a monitor is connected at boot (no hot plug) |
| `-rmbextbindhdmi` | Also bind a native HDMI port (off by default, **untested**, for laptops with HDMI) |
| `-rmbnoextreplay` | Turn off the replay of a connect event swallowed at boot |
| `-rmbnogfxclk` | Turn off the GPU clock policy (the PMFW keeps its boot limits) |
| `-rmbnoedidfix` | Turn off the EDID repair (panel falls back to 60 Hz) |
| `-rmbnotgfix` | Turn off the timing generator fix |
| `-rmbxhciintr1` | Turn off the xHCI interrupter fix (cursor stutters again) |
| `-rmbnovcnfix` | Turn off hardware video decode (no VCN hooks, software codecs) |
| `-rmbvcnguard` | Turn on the VCN crash guard (a boot after a crash during video starts with VCN off) |
| `-rmbvcn` | Keep the video properties even when the VCN crash guard turned VCN off (diagnostic only) |
| `-rmbretimer` | Fixed VS/PE retimer emulation (for hardware with an LTTPR, not the 82TL) |
| `-rmbusbc4lane` | Ask for 4 DP lanes on USB-C (the 82TL hub only has 2) |

---

## Building

| Input | Where to get it | Default location |
|---|---|---|
| Xcode Command Line Tools | `xcode-select --install` | |
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | `git clone` | `deps/MacKernelSDK` |
| Kernel Debug Kit 26.7 **25G229** | [developer.apple.com/download](https://developer.apple.com/download/all/) (free Apple account) | `/Library/Developer/KDKs/KDK_26.7_25G229.kdk` |
| Rembrandt GPU firmware | [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git), `amdgpu/yellow_carp_*.bin`, decompressed into one folder | `deps/yellow_carp_fw` |
| Lilu 1.7.3 headers and runtime | Already in `build/` | |

With the inputs at their default locations:

```sh
cd src/Rembrandt660M
export PATH="/Library/Developer/CommandLineTools/usr/bin:$PATH"
make firmware   # splits the yellow_carp firmware into fw/
make            # -> Rembrandt660M.kext
make check      # symbol manifest + import audit, must print RESULT: PASS
```

Other locations are passed on the command line, for example `make SDK=... KDK=... FWSRC=...`.

Tests (offline; the byte-level cases that need the KDK or the Boot KC are skipped when those are missing):

```sh
for f in tests/test_*.py; do python3 "$f"; done
```

---

## OpenCore EFI reference (Lenovo 82TL)

The full setup of the 82TL, for reference. On another laptop take only the GPU parts
([step 5](#step-5-set-up-opencore)) and build the rest for your hardware.

Start from a standard AMD laptop OpenCore EFI (Dortania guide, OpenCore 1.0.x) and set the following.

**Kernel → Add**, in this order at the top:

1. `Lilu.kext` (1.7.3 or newer)
2. `dist/Rembrandt660M.kext`, **directly after Lilu**
3. `VirtualSMC.kext`, then the rest, including `dist/RmbPanel660M.kext` (brightness slider, codeless)

Everything else is ordinary laptop support and has nothing to do with the GPU: `AmdTscSync`, `RestrictEvents`,
`AppleMCEReporterDisabler`, a USB map (`USBToolBox` + `UTBMap`), `VoodooPS2Controller` (keyboard only),
`VoodooI2C` + `VoodooI2CHID` (touchpad), `ECEnabler` + `SMCBatteryManager`, `AMDRyzenCPUPowerManagement` +
`SMCAMDProcessor`, `AppleALC` (or `SimpleHDA`), `YogaSMC`, and MT7922 Wi-Fi/Bluetooth kexts. Do not load
`NootedRed` or any other Radeon kext next to this one.

**Kernel → Patch.** The AMD Vanilla patches (`AMD_Vanilla`) with the 6600HS core count (6) in the four
`cpuid_cores_per_package` patches. On 26.x also add the non-monotonic time panic patches for
`thread_invoke`/`thread_dispatch` that match your kernel version.

**Kernel → Quirks.** `ProvideCurrentCpuInfo`, `DisableIoMapper`, `DisableLinkeditJettison`,
`PowerTimeoutKernelPanic`, `DisableRtcChecksum`. **Kernel → Emulate:** `DummyPowerManagement`.

**Booter → Quirks.** `AvoidRuntimeDefrag`, `EnableSafeModeSlide`, `ProvideCustomSlide`, `RebuildAppleMemoryMap`,
`SetupVirtualMap`, `SyncRuntimePermissions`, `ProvideMaxSlide` = 128.

**DeviceProperties → Add**, on the GPU at `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x0)`:

| Key | Type | Value |
|---|---|---|
| `device-id` | Data | `FF730000` |
| `compatible` | String | `pci1002,73ff` |
| `model` | String | `AMD Radeon 660M` |
| `LoadAccelerator`, `LoadController`, `LoadHWServices` | Boolean | true |
| `CFG_USE_AGDC` | Data | `0000000000000000` |
| `CFG_NO_BOOT_PWDN` | Data | `0100000000000000` |
| `AAPL,iokit-ignore-ndrv` | Data | `01000000` |

`ATY,bin_image` is not needed on the 82TL: the kext builds the augmented VBIOS from the ACPI `VFCT` table and
checks its hash. `AAPL00,override-no-connect` is optional; if you set it, use the
panel's EDID as Linux reads it (`/sys/class/drm/card*-eDP-1/edid`). `edidfix` repairs the bad DisplayID block
either way.

For the speakers add `layout-id` = `65000000` on `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x6)`, or the `alcid=101`
boot-arg.

**ACPI → Add.** `SSDT-EC`, `SSDT-USBX`, `SSDT-PLUG-ALT`, `SSDT-HPET` (with the matching IRQ renames),
`SSDT-PNLF-AMD` and `SSDT-ALS0`, plus two written for this laptop: `SSDT-I2C-POLL` (touchpad in polling mode) and
`SSDT-BRTKEY` (F5/F6 brightness keys, see below).

**Brightness.** Apple's driver has no entry for the panel, so the slider stays off unless the codeless
`dist/RmbPanel660M.kext` injects a panel table for the panel's ID (`F10T0aa8`, BOE NE145F8M-N61). `SSDT-BRTKEY` makes the EC send the F5/F6
queries (it sets `OSTY` and renames `_REG` to `XREG`).

**PlatformInfo.** SMBIOS `iMac20,1` with your own serials, `UpdateSMBIOSMode` = `Custom` and
`CustomSMBIOSGuid` (so Windows on the same disk is not affected).

**NVRAM → boot-args.** `-lilubetaall -rmbumd alcid=101 revblock=cpum`, plus `-v keepsyms=1` for the first
boots. `-rmbumd` is required: without it Safari text and icons are scrambled.

**UEFI → Drivers.** `OpenRuntime.efi`, `HfsPlus.efi`, `ResetNvramEntry.efi` (and `OpenCanopy.efi` for a
graphical picker).

---

## Repository layout

| Path | What's inside |
|---|---|
| [`dist/`](dist) | Built `Rembrandt660M.kext` and the codeless panel injector `RmbPanel660M.kext` |
| [`src/Rembrandt660M/`](src/Rembrandt660M) | Plugin source (`display.cpp`, `smu.cpp`, `dmub.cpp`, `umdpatch.cpp`, …) |
| [`build/`](build) | Lilu 1.7.3 headers and runtime used to build and ABI-check the kext |
| [`tests/`](tests) | Offline source and contract tests |
| [`tools/`](tools) | `port_vbios.py` (VBIOS values for another laptop), manifest and import verifiers, Metal and GPU-clock probes (`tools/gfxcheck`, `tools/metal`, `tools/metaltest`), VideoToolbox decode probe (`tools/vtdecode`) |
| [`scripts/`](scripts) | `fw-extract.py` (splits the linux-firmware images for the build) |
| [`audit/`](audit) | Import audit result (`import_authority.json`, `undefined_symbols.txt`) and firmware hashes |

---

## Star history

<div align="center">

<a href="https://www.star-history.com/#rthh21/660m-Metal&Date">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=rthh21/660m-Metal&type=Date&theme=dark" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=rthh21/660m-Metal&type=Date" />
    <img alt="Star history of rthh21/660m-Metal" src="https://api.star-history.com/svg?repos=rthh21/660m-Metal&type=Date" width="720" />
  </picture>
</a>

⭐ *If this got macOS running on your 660M, a star helps other Rembrandt owners find it.* ⭐

</div>

---

## Credits

- [acidanthera](https://github.com/acidanthera): Lilu, MacKernelSDK and OpenCore
- RembrandtGfx, the Radeon 680M plugin for the GPD Win Max 2 this kext started from (GPL-2.0), and
  [macos-kvm-680m](https://github.com/caoshiyuan0211-blip/macos-kvm-680m) before it
- The Linux `amdgpu` driver, the reference for every Rembrandt register and firmware detail

## License

The source code is GPL-2.0. See [`LICENSE`](LICENSE).

The built kext contains AMD GPU firmware (GC, SDMA, DMCUB and VCN) split from `amdgpu/yellow_carp_*.bin` in
[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git). It is
redistributed under AMD's license, which ships next to it as `Contents/Resources/LICENSE-AMD-firmware.txt` (source
copy: [`src/Rembrandt660M/YELLOW_CARP_LICENSE.txt`](src/Rembrandt660M/YELLOW_CARP_LICENSE.txt)). The firmware is
not GPL.

No Lenovo or AMD VBIOS is included. The kext reads the VBIOS from the laptop's own ACPI tables at boot.
