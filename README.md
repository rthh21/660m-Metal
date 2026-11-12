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
[Install guide](#install-guide-step-by-step) ·
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
rebuild with their own VBIOS values; the [Install guide](#install-guide-step-by-step) walks through it from the start.
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
| **VRAM** | Partial | Carve-out of the BIOS UMA size (2 GB and 4 GB run on the 82TL) |
| **Sleep** | No | The laptop only reaches DarkWake, not S3 |

The kext also carries two small non-graphics helpers that the same boot depends on: the xHCI interrupter fix
above and `wlanfix` (turns off 10-bit tags on the root port of an MT7922 Wi-Fi card before macOS first reads it).

> [!WARNING]
> The kext only works on **macOS 26.7 build 25G229**. Its hooks use fixed addresses in that kernel collection,
> so any other build (including a minor update) needs a new symbol manifest. Turn off automatic updates.

---

## Install guide (step by step)

This guide is for someone who has never installed macOS on a PC. It goes from a laptop running Windows to
macOS with full graphics acceleration. Read it once to the end before you start, then do the steps in order.

- Copy every command exactly. Replace text in `<angle brackets>` with your own value, without the brackets.
- Each step says what you should see. If you see something else, stop and go to
  [If something goes wrong](#if-something-goes-wrong). Don't skip ahead.
- **Be honest with yourself about the risk.** The kext has only run on one laptop, a Lenovo 82TL. On any other
  computer you are the first person to try it. It may not work, and you may need to read logs and open an issue.

> [!IMPORTANT]
> Installing macOS erases a whole disk or partition. **Back up everything** before step 6. If Windows uses
> BitLocker, write down its recovery key first (**Settings → Privacy & security → Device encryption**, or
> [aka.ms/myrecoverykey](https://aka.ms/myrecoverykey)): after the BIOS changes in step 2, Windows will ask for it.

### What you need

| | |
|---|---|
| **A computer with a Radeon 660M** | Checked in [step 1](#step-1-check-your-gpu) |
| **Two USB sticks** | 32 GB or more for the macOS installer, 8 GB or more for Linux. Both are erased |
| **A Mac for about an hour** | To download the exact macOS version (step 5). Without a Mac it still works, it just takes longer |
| **Internet in macOS** | A USB Ethernet adapter is the easy way. The laptop's Wi-Fi may not work in macOS at first |
| **A free Apple ID** | Only if you have to build the kext (step 7) |
| **Time** | Plan a full day the first time |

### Step 1: Check your GPU

In Windows:

1. Right-click the Start button and open **Device Manager**.
2. Open **Display adapters** and double-click the **AMD Radeon** entry.
3. Go to the **Details** tab and choose **Hardware Ids** in the list. The first line looks like this:
   ```
   PCI\VEN_1002&DEV_1681&SUBSYS_3B1E17AA&REV_03
   ```
   - `DEV_1681` is the AMD Rembrandt GPU. **If your line doesn't have `VEN_1002&DEV_1681`, this project is not
     for your computer. Stop here.**
   - `SUBSYS_3B1E17AA` and `REV_03` identify your laptop's version of it. Write the whole line down.
4. Open **Settings → System → About** and look at **Processor**. `DEV_1681` is used by two GPUs:
   - **Radeon 660M**: Ryzen 5 6600U, 6600H, 6600HS, 7535U, 7535HS and similar. This is what the project is for.
   - **Radeon 680M**: Ryzen 7 / Ryzen 9 6800U, 6800H, 6900HS, 7735U, 7735HS and similar. Same chip with twice the
     compute units. It has never been tried with this kext.

Other AMD graphics (Vega, Radeon 780M and newer) are different chips and won't work with this kext.

### Step 2: BIOS settings

Open the BIOS setup. On Lenovo laptops press **F2** at power-on, or use the small Novo button and choose
**BIOS Setup**. On other brands try Del, F2 or F10. Names differ between brands; look for these:

| Setting | Set to | Why |
|---|---|---|
| **Secure Boot** | Disabled | OpenCore, the boot loader used below, isn't signed for Secure Boot |
| **Boot mode** / **CSM** / **Legacy** | UEFI only, CSM off | OpenCore is a UEFI program |
| **UMA Frame Buffer Size** (also called iGPU memory, VRAM or UMA size) | Suggested: 4 GB with 16 GB of RAM or more, 2 GB with 8 GB | This becomes the GPU memory in macOS. Both 2 GB and 4 GB have run on the 82TL |

Many laptops don't show a UMA option. Then leave it as it is. Save and exit (usually **F10**).

Set these **before** step 3, so Linux reads the GPU with the settings you will use.

### Step 3: Read the GPU data in Linux

The kext needs a copy of your GPU's VBIOS and four values. macOS can't show them before the driver works, but
Linux can, without installing anything.

1. Download **Ubuntu Desktop** from [ubuntu.com/download/desktop](https://ubuntu.com/download/desktop).
2. Write it to the 8 GB stick with [balenaEtcher](https://etcher.balena.io). This erases the stick.
3. Restart the laptop and open the boot menu. On Lenovo laptops press **F12** at power-on, or use the Novo button
   and choose **Boot Menu**. On other brands try F12, F11, F9 or Esc. Pick the USB stick.
4. Choose **Try Ubuntu**, not Install. Ubuntu then runs from the stick and changes nothing on your disk. Connect
   to Wi-Fi from the top-right corner.
5. Plug in the second USB stick. Open a terminal with **Ctrl+Alt+T** and run these lines one at a time:
   ```sh
   cd ~
   { lspci -nnv -d 1002:1681 | head -3; sudo dmesg | grep -E "ATOM BIOS|amdgpu.*(VRAM:|RAM width)"; } | tee gpu-info.txt
   sudo cat /sys/firmware/acpi/tables/VFCT > VFCT.bin
   ls -l VFCT.bin
   ```
   On the Lenovo 82TL it prints this (shortened, timestamps removed):
   ```
   04:00.0 VGA compatible controller [0300]: ... [1002:1681] (rev 03) ...
           Subsystem: Lenovo ... [17aa:3b1e]
   amdgpu 0000:04:00.0: [drm] ATOM BIOS: 113-REMBRANDT-X37
   amdgpu 0000:04:00.0: VRAM: 2048M 0x000000F400000000 - 0x000000F47FFFFFFF (2048M used)
   amdgpu 0000:04:00.0: [drm] RAM width 128bits LPDDR5
   -rw-rw-r-- 1 ubuntu ubuntu 44676 ... VFCT.bin
   ```
   `lspci` may call the GPU "Radeon 680M". That's only a family name; `1002:1681` is what counts.

   Write down four values: the **subsystem** (`17aa:3b1e` above), the **revision** (`03`), the **VRAM** in MB
   (`2048`) and the **RAM width** in bits (`128`).
   - `VFCT.bin: No such file or directory`: your firmware doesn't publish its VBIOS this way, and the kext can't
     work. Stop and open an issue.
   - No `amdgpu` lines: Linux didn't start its GPU driver. Try the newest Ubuntu.
6. Still in the terminal, download this project and run its VBIOS tool with **your** VRAM and RAM width:
   ```sh
   wget https://github.com/rthh21/660m-Metal/archive/refs/heads/main.zip
   python3 -m zipfile -e main.zip .
   python3 660m-Metal-main/tools/port_vbios.py VFCT.bin --vram-mb <VRAM> --bus-width <RAM width> | tee -a gpu-info.txt
   ```
   It ends with five lines that start with `constexpr`. You need them in steps 4 and 7.
7. Open the **Files** app, go to **Home**, and drag `gpu-info.txt` and `VFCT.bin` onto the second stick.
   Eject it and shut down Ubuntu.

> [!NOTE]
> `VFCT.bin` is your laptop maker's VBIOS. Keep it to yourself. Don't upload it anywhere, not even in an issue.

### Step 4: Ready-made kext or your own build?

The ready-made kext in [`dist/`](dist) contains the values of one VBIOS only: `113-REMBRANDT-X37`, from the BIOS
of the Lenovo 82TL it was built on. At every boot it checks the VBIOS and the GPU's PCI IDs, and it does nothing
on any other. That safety check is why most computers need their own build. Compare your `gpu-info.txt` with
this table:

| Value | Must be exactly |
|---|---|
| The `kRawLen` line | `constexpr size_t kRawLen = 0xAE00;` |
| The `kRawFnv` line | `constexpr uint64_t kRawFnv = 0x45263ce37d8098a3ULL;` |
| Subsystem (from `lspci`) | `17aa:3b1e` |
| Revision (from `lspci`) | `03` |

- **All four match:** use the ready-made `dist/Rembrandt660M.kext` and skip step 7.
- **Anything differs:** build your own kext in step 7. It takes about an hour, and nothing in it is hard.

This also applies to another Lenovo 82TL. A Lenovo BIOS update can change the VBIOS, and the ready-made kext
then refuses it. **After every BIOS update, redo steps 3 and 4.**

The `kVramMb` line doesn't decide anything here. macOS uses the real size set in the BIOS: the 82TL runs a kext
built with `kVramMb = 2048` with 4 GB set in the BIOS, and macOS shows 4 GB. Changing the UMA size didn't change
the VBIOS on the 82TL either.

If the tool stopped with an error instead of printing the five lines:

| Error | Meaning |
|---|---|
| `no 1002:1681 image in the VFCT table` | The VFCT doesn't hold this GPU's VBIOS. The kext can't work |
| `master data table slot ... is already used` or `augmented image is over 64 KiB` | Your VBIOS is laid out differently. The kext needs changes first: open an issue with `gpu-info.txt` |

### Step 5: Get macOS 26.7 build 25G229

The kext only works on macOS Tahoe **26.7, build 25G229**. It changes Apple's driver code at addresses that exist
only in that exact build. Apple also offers newer versions (26.7.1 to 26.7.3 when this was written); the kext is
not made for them.

> [!WARNING]
> The usual hackintosh method downloads macOS Recovery (`macrecovery.py`), and Recovery always installs the
> **newest** version. Don't use it for your final install. You need the full 26.7 installer.

**With a Mac**, open Terminal:

1. Check that Apple still offers the build:
   ```sh
   softwareupdate --list-full-installers | grep 25G229
   ```
   It must print `* Title: macOS Tahoe, Version: 26.7, Size: 17951133KiB, Build: 25G229, ...`. If it prints
   nothing, Apple has withdrawn it and the kext needs an update for a newer build: open an issue.
2. Download it (about 18 GB). It appears as **Install macOS Tahoe** in Applications:
   ```sh
   softwareupdate --fetch-full-installer --full-installer-version 26.7
   ```
3. Plug in the 32 GB stick. Open **Disk Utility** and choose **View → Show All Devices**. Select the stick itself
   (the top line, not the volume under it) and click **Erase**. Use name `MyVolume`, format
   **Mac OS Extended (Journaled)** and scheme **GUID Partition Map**.
4. Write the installer to the stick. It asks for your password, then for `Y`, and takes about 20 minutes:
   ```sh
   sudo "/Applications/Install macOS Tahoe.app/Contents/Resources/createinstallmedia" --volume /Volumes/MyVolume
   ```

**Without a Mac:** make a normal Tahoe installer from Windows with Dortania's
[Making the installer in Windows](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/windows-install.html).
It installs the newest Tahoe, which is fine as a **temporary** system. Install it with step 6, then run the
"With a Mac" steps inside it to make a 25G229 stick, and install again from that stick, erasing the disk.

### Step 6: Install macOS without acceleration

First you get macOS running **without** the GPU driver. The picture works, but everything is slow and animations
stutter. That's expected; the GPU comes in step 8.

Follow the [Dortania OpenCore Install Guide](https://dortania.github.io/OpenCore-Install-Guide/) from start to
end. Where it asks you to choose, take the **AMD** pages (Ryzen, 17h and 19h). Use these choices, which are what
runs on the 82TL:

| Part | What to use |
|---|---|
| **OpenCore** | 1.0.8 RELEASE |
| **Kexts** | `Lilu` 1.7.2 or newer, `VirtualSMC`, `AmdTscSync`, `RestrictEvents`, `AppleMCEReporterDisabler`, `USBToolBox` + `UTBDefault`, `VoodooPS2Controller` (keyboard), `VoodooI2C` + `VoodooI2CHID` (touchpad), `ECEnabler` + `SMCBatteryManager` (battery), `AMDRyzenCPUPowerManagement` + `SMCAMDProcessor`. Audio and Wi-Fi depend on your hardware. **No graphics kext**: no `NootedRed`, no `WhateverGreen` |
| **ACPI** | `SSDT-EC`, `SSDT-USBX`, `SSDT-PLUG-ALT` and `SSDT-HPET`, made for your laptop with [SSDTTime](https://github.com/corpnewt/SSDTTime) as Dortania explains |
| **config.plist** | Start from OpenCore's `Sample.plist` as Dortania says, then set what the [EFI reference](#opencore-efi-reference-lenovo-82tl) lists: quirks, the AMD kernel patches with **your** CPU core count, the extra kernel patches, `iMac20,1` with your own serials from [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS), and the boot-args (without `-rmbumd` for now) |

Then:

1. Copy your `EFI` folder to the EFI partition of the installer stick, as Dortania shows.
2. Boot from the stick (boot menu, step 3) and choose **Install macOS Tahoe**. In **Disk Utility** erase the
   target disk or partition as **APFS**, then install. Erasing a whole disk deletes Windows on it: to keep
   Windows, read Dortania's [Multiboot guide](https://dortania.github.io/OpenCore-Multiboot/) first. The laptop
   restarts several times. Each time, pick the stick in the boot menu and the **macOS Installer** entry in
   OpenCore, until the entry has your disk's name.
3. After the setup assistant, turn off updates: **System Settings → General → Software Update**, click ⓘ next to
   **Automatic updates** and turn everything off. **Never click "Update Now".**
4. Check the build in Terminal. It must print `25G229`:
   ```sh
   sw_vers -buildVersion
   ```
5. Make the laptop boot without the stick: copy the `EFI` folder to the internal disk's EFI partition, as in
   Dortania's [Moving OpenCore from USB to macOS drive](https://dortania.github.io/OpenCore-Post-Install/universal/oc2hdd.html).

You now have a working, slow macOS. Keep this EFI as it is: it's your way back if step 8 goes wrong.

### Step 7: Build the kext

Skip this step if step 4 said the ready-made kext fits. Everything here runs in Terminal on the laptop, in macOS.

1. **Install the Xcode Command Line Tools.** Run this and click **Install** in the window that opens:
   ```sh
   xcode-select --install
   ```
2. **Install the Kernel Debug Kit.** Go to [developer.apple.com/download/all](https://developer.apple.com/download/all/)
   and sign in with your Apple ID. Search for **Kernel Debug Kit 26.7 build 25G229**, download the `.dmg`, open
   it and run the `.pkg` inside. Check that it's installed:
   ```sh
   ls /Library/Developer/KDKs/
   ```
   It must list `KDK_26.7_25G229.kdk`.
3. **Download the source and what it needs.** The GPU firmware comes from Linux's official firmware collection,
   at the version the kext was checked against (tag `20261019`). `make firmware` refuses any other version:
   ```sh
   mkdir -p ~/rmb && cd ~/rmb
   git clone https://github.com/rthh21/660m-Metal.git
   cd 660m-Metal
   git clone https://github.com/acidanthera/MacKernelSDK.git deps/MacKernelSDK
   mkdir -p deps/yellow_carp_fw
   for f in me pfp ce mec mec2 rlc sdma toc dmcub vcn; do
     curl -fL -o deps/yellow_carp_fw/yellow_carp_$f.bin \
       "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/amdgpu/yellow_carp_$f.bin?h=20261019"
   done
   ls -l deps/yellow_carp_fw
   ```
   The last command must list ten `yellow_carp_*.bin` files, none of them 0 bytes.
4. **Put in your VBIOS values.** Plug in the stick with `gpu-info.txt` and open the file to edit:
   ```sh
   open -e src/Rembrandt660M/vbios_augment.hpp
   ```
   Near the top are five lines that start with `constexpr` (`kRawLen` to `kVramMb`). Replace them with the five
   lines from your `gpu-info.txt`, save (**Cmd+S**) and close TextEdit.
5. **Put in your PCI IDs.** In the first line, replace the three values with yours from step 3: lower case,
   without `0x`, for example `SV=1043 SD=1e12 REV=c1` for subsystem `1043:1e12`, revision `c1`. Then run all
   the lines:
   ```sh
   SV=<subsystem vendor> SD=<subsystem id> REV=<revision>
   sed -i '' -e "s/0x17aa/0x$SV/g" -e "s/0x3b1e/0x$SD/g" \
     -e "s/\"revision-id\") == 3/\"revision-id\") == 0x$REV/" \
     -e "s/\"revision-id\") != 3/\"revision-id\") != 0x$REV/" src/Rembrandt660M/mmio.cpp
   grep -n -E "subsystem-(vendor-)?id|revision-id" src/Rembrandt660M/mmio.cpp
   ```
   The `grep` shows the two checks that decide whether the kext touches the GPU. Both must now contain your three
   values.
6. **Build:**
   ```sh
   cd src/Rembrandt660M
   export PATH="/Library/Developer/CommandLineTools/usr/bin:$PATH"
   make firmware
   make
   make check
   ```
   `make check` must end with `RESULT: PASS`. Your kext is now `~/rmb/660m-Metal/src/Rembrandt660M/Rembrandt660M.kext`.
   If a command stops with an error, open an issue and paste the whole output.

### Step 8: Turn on acceleration

Make the change on a **test stick** first. If something goes wrong, you take the stick out and the laptop boots
as before.

1. **Make a test stick.** In Disk Utility erase a USB stick as **MS-DOS (FAT)**, scheme **GUID Partition Map**.
   Then mount the EFI partition you put OpenCore on in step 6.5. `diskutil list` shows it as the line with
   type `EFI` under your internal disk, for example `disk0s1`:
   ```sh
   diskutil list
   sudo diskutil mount <identifier, e.g. disk0s1>
   ```
   Copy the whole `EFI` folder from it to the stick.
2. **Add the kext.** Copy `Rembrandt660M.kext` (yours from step 7, or `dist/Rembrandt660M.kext`) into
   `EFI/OC/Kexts` **on the stick**.
3. **Register it.** Open the stick's `EFI/OC/config.plist` in [ProperTree](https://github.com/corpnewt/ProperTree)
   and use **File → OC Snapshot** on the stick's `OC` folder. In **Kernel → Add**, check that the new entry comes
   **directly after Lilu** and that `Enabled` is `True`.
4. **Find the GPU's path.** Download [gfxutil](https://github.com/acidanthera/gfxutil/releases) and unzip it. In
   Terminal, drag the `gfxutil` file onto the window, type ` | grep -i 1002:1681` after it and press Enter. If
   macOS blocks it, allow it in **System Settings → Privacy & Security** and run it again. Copy the part that
   starts with `PciRoot`. On the 82TL it is `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x0)`.
5. **Describe the GPU to macOS.** In ProperTree, under **DeviceProperties → Add**, add a child of type
   Dictionary whose key is that path. Give it these children:

   | Key | Type | Value |
   |---|---|---|
   | `device-id` | Data | `FF730000` |
   | `compatible` | String | `pci1002,73ff` |
   | `model` | String | `AMD Radeon 660M` |
   | `LoadAccelerator`, `LoadController`, `LoadHWServices` | Boolean | True |
   | `CFG_USE_AGDC` | Data | `0000000000000000` |
   | `CFG_NO_BOOT_PWDN` | Data | `0100000000000000` |
   | `AAPL,iokit-ignore-ndrv` | Data | `01000000` |

   Don't add `ATY,bin_image`: on 25G229 it never reaches the driver, which is why the kext builds the image
   itself. Don't copy an `AAPL00,override-no-connect` from someone else's EFI; it is their panel's EDID.
6. **Turn on the UMD fix.** In **NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args**, add
   `-lilubetaall -rmbumd` to what is there. Keep `-v keepsyms=1` for now. Without `-rmbumd`, text and icons in
   Safari are scrambled.
7. Save the file. Restart, choose the test stick in the boot menu, then macOS in OpenCore.

If the boot hangs, hold the power button for 10 seconds, take the stick out and start again: the laptop boots
from the internal disk as before. Then read [If something goes wrong](#if-something-goes-wrong).

### Step 9: Check the result

Open Terminal and run:

```sh
system_profiler SPDisplaysDataType | grep -E "Chipset|VRAM|Metal"
grep -a "\[vbios\]" /private/var/log/rmb-msgbuf.log | tail -3
```

On the 82TL with 4 GB set in the BIOS it prints:

```
      Chipset Model: AMD Radeon 660M
      VRAM (Total): 4 GB
      Metal Support: Metal 3
```

and a line `[vbios] ATY,bin_image set from ACPI VFCT (0xb600 bytes, fnv ...)`. Animations are now smooth.

If it works, make it permanent. Mount the internal EFI partition (step 8.1) and copy its current `EFI` folder
somewhere safe, for example your Documents folder. Then replace it with the `EFI` folder from the test stick.
Keep the test stick: it boots the same system if the internal EFI ever breaks.

### Step 10: After it works

1. **Video decoding.** Let videos use the GPU. Run once; there is no sudo, and `defaults delete com.apple.AppleGVA`
   undoes it:
   ```sh
   defaults write com.apple.AppleGVA gvaForceAMDAVCDecode -bool YES
   defaults write com.apple.AppleGVA gvaForceAMDHEVCDecode -bool YES
   ```
2. **Sleep.** Sleep doesn't work with this kext. macOS still tries and ends up in a half-asleep state
   (DarkWake), and waking from it once froze the 82TL with an external monitor connected. Turn the attempt off;
   the screen still turns off when idle:
   ```sh
   sudo pmset -a sleep 0
   ```
3. Remove `-v` from boot-args once everything works.
4. Read [Laptop-specific features](#laptop-specific-features) for the brightness slider, external monitors and
   the GPU clock.
5. **Never update macOS.** The kext is made for 25G229 only. If macOS updated anyway, add `-rem660off` to
   boot-args (it turns the kext off) to get back in without acceleration. After a **BIOS** update, redo steps 3
   and 4 before booting macOS.

### If something goes wrong

The kext writes its log to `/private/var/log/rmb-msgbuf.log` (readable without sudo).

| What you see | Cause | What to do |
|---|---|---|
| Black screen, or the boot hangs after the Apple logo | A display fix doesn't fit your laptop, or the kext is wrong | Take the test stick out and boot from the internal disk; you are back in the working system. Add `-rem660off` to the stick's boot-args: if that boots, the kext is the cause. Then try the feature boot-args ([Boot-args](#boot-args)) one at a time |
| `VFCT ... rejected (raw image is not the verified Lenovo VBIOS)` in the log | The kext doesn't know this VBIOS: the ready-made kext on another laptop or BIOS, or wrong values in step 7.4 | Redo step 3 with the current BIOS settings, then step 7 |
| No `[vbios]` lines in the log, macOS still slow | The kext didn't load, or the PCI ID checks don't match | `kmutil showloaded \| grep -i rembrandt` must list it. Check the order in **Kernel → Add** (directly after Lilu), then step 7.5 |
| Scrambled text and icons | `-rmbumd` is missing | Add it to boot-args |
| Panel stays at 60 Hz | EDID problem | `grep -a edidfix /private/var/log/rmb-msgbuf.log` |
| The laptop froze and you had to force it off | Unknown | Boot again and open an issue saying what you were doing. A kext you built yourself (step 7) saves the last ~2 minutes before the freeze to `/private/var/log/rmb-msgbuf.prev.log` at the next boot; attach it. The kext in `dist/` doesn't save it yet: copy `rmb-msgbuf.log` right away instead |

To ask for help, open an issue with `gpu-info.txt`, `rmb-msgbuf.log` and what you did. **Never attach
`VFCT.bin`.**

### Laptop-specific features

| Part | On your laptop |
|---|---|
| Internal panel (eDP, link 0) | Generic, should work. `edidfix` only acts when an EDID block has a bad checksum |
| Hardware video decode | Generic. Run the two commands from [step 10](#step-10-after-it-works) once |
| External displays (`extbind`) | DisplayPort over USB-C by default; hot plug on the first USB-C DP port. For a native HDMI port add `-rmbextbindhdmi` (untested). See [External display](#external-display-through-a-usb-c-hub) |
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

### Known problems

- **Sleep** doesn't work. Use `pmset -a sleep 0` ([step 10](#step-10-after-it-works)).
- **Freezes with an external monitor.** The 82TL froze hard twice on 2026-11-12 with a monitor on the USB-C hub:
  once in normal use (the screens turned green and black), once right after the screens woke up. The cause isn't
  known yet. After such a freeze the external display stays off on purpose; see the crash guard in
  [External display](#external-display-through-a-usb-c-hub).
- **Tested on one laptop only.** Every other computer is untested.

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
| `-rmbnoblramp` | Turn off the backlight smoother (brightness moves in steps again) |
| `rmbblramp=<ms>` | Time constant of the backlight smoother, default 50, at most 2000; `0` turns it off |

---

## Building

| Input | Where to get it | Default location |
|---|---|---|
| Xcode Command Line Tools | `xcode-select --install` | |
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | `git clone` | `deps/MacKernelSDK` |
| Kernel Debug Kit 26.7 **25G229** | [developer.apple.com/download](https://developer.apple.com/download/all/) (free Apple account) | `/Library/Developer/KDKs/KDK_26.7_25G229.kdk` |
| Rembrandt GPU firmware | [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) tag `20261019`, `amdgpu/yellow_carp_*.bin`, uncompressed, in one folder ([step 7.3](#step-7-build-the-kext) has the commands). `make firmware` accepts only the hashes in `audit/firmware_local_sha256.json` | `deps/yellow_carp_fw` |
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
([step 8](#step-8-turn-on-acceleration)) and build the rest for your hardware.

Start from a standard AMD laptop OpenCore EFI (Dortania guide, OpenCore 1.0.x) and set the following.

**Kernel → Add**, in this order at the top:

1. `Lilu.kext` (1.7.2 or newer; the 82TL runs 1.7.2)
2. `dist/Rembrandt660M.kext`, **directly after Lilu**
3. `VirtualSMC.kext`, then the rest, including `dist/RmbPanel660M.kext` (brightness slider, codeless)

Everything else is ordinary laptop support and has nothing to do with the GPU: `AmdTscSync`, `RestrictEvents`,
`AppleMCEReporterDisabler`, `USBToolBox` + `UTBDefault`, `VoodooPS2Controller` (keyboard only),
`VoodooI2C` + `VoodooI2CHID` (touchpad), `ECEnabler` + `SMCBatteryManager`, `AMDRyzenCPUPowerManagement` +
`SMCAMDProcessor`, `AppleALC` (or `SimpleHDA`), `YogaSMC`, and MT7922 Wi-Fi/Bluetooth kexts. Do not load
`NootedRed` or any other Radeon kext next to this one.

**Kernel → Patch.** The AMD Vanilla patches (`AMD_Vanilla`) with the 6600HS core count (6) in the four
`cpuid_cores_per_package` patches. After them, four more, all with `Identifier` = `kernel`, `Arch` = `x86_64`
and an empty `Base`. The first three are the usual non-monotonic time patches for AMD; the last one is for this
kernel only:

| Comment | Find | Replace | Mask | ReplaceMask | Count | MinKernel | MaxKernel |
|---|---|---|---|---|---|---|---|
| thread_quantum_expire, thread_unblock, thread_invoke (Visual) | `48000000020000480000580000000F0000000000` | `0000000000000000000000000000669066906690` | `FF00000FFFFFFFFF0000FF000000FF0000000000` | `0000000000000000000000000000FFFFFFFFFFFF` | 3 | 21.0.0 | 25.99.99 |
| thread_invoke, thread_dispatch, up to 26.3 (Visual) | `480000800400000F0000000000` | `00000000000000669066906690` | `480000F0FFFFFFFF0000000000` | `00000000000000FFFFFFFFFFFF` | 2 | 21.0.0 | 25.3.99 |
| thread_invoke, thread_dispatch, 26.4 and newer (laobamac) | `480000900400000F0000000000` | `00000000000000669066906690` | `480000F0FFFFFFFF0000000000` | `00000000000000FFFFFFFFFFFF` | 2 | 25.4.0 | 25.99.99 |
| fq_codel dequeue, 25G229 kernel only (rmb): `now < pkt_enq_time` counts as zero delay instead of a panic | `4D89C14929C97274488B46200FB6` | `4D89C14929C97252488B46200FB6` | | | 1 | 25.0.0 | 25.99.99 |

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
boots. `-rmbumd` is required: without it Safari text and icons are scrambled. The 82TL actually boots with
`keepsyms=1 debug=0x100 npci=0x3000 -lilubetaall -no_compat_check revblock=cpum dk=0 -rmbdbg alcid=101 -rmbumd`;
of these only `-lilubetaall` and `-rmbumd` concern the GPU.

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
