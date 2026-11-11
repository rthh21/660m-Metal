//
//  RembrandtGfx shared declarations.
//

#pragma once

#include <Headers/kern_patcher.hpp>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>

// Kernel printf reaches the serial port (boot-arg serial=3) for the whole boot; IOLog stops being
// mirrored there once os_log takes over, and kprintf only works early.
extern "C" int printf(const char *fmt, ...) __printflike(1, 2);
#define LOG(fmt, ...) printf("Rembrandt660M: " fmt "\n", ##__VA_ARGS__)

#if defined(KERNEL)
#include <IOKit/IOLib.h>
#define REM_IOLOG(fmt, ...) IOLog("REM660: " fmt, ##__VA_ARGS__)
#else
#define REM_IOLOG(fmt, ...) do {} while (0)
#endif
// -rmbverbose: bulk bring-up tracing (DAL log, DAL write log, DMUB ring dumps, register
// snapshots). Off by default: ~1 MB of synchronous serial printf slows the whole boot.
bool rmbVerbose();
void safeModesLog();
// display.cpp: inject one DMUB inbox command (reply = slot contents after the fw consumed it); DAL link of the
// external DP/HDMI stream (0 = none).
bool dmubInjectCmd(const uint32_t *cmd, uint32_t *reply);
uint32_t extLink();
bool extLinkDp();  // the external stream's link is DP (not HDMI/DVI)
// disklog.cpp: small state files (display.cpp extreplay crash guard). Read: whole file into buf (NUL-ended;
// buf may be nullptr), *mtime in s; false if it cannot be opened. Write: replace the content, full fsync.
bool rmbFileRead(const char *path, char *buf, size_t n, int64_t *mtime);
bool rmbFileWrite(const char *path, const char *s);
// smu.cpp: GFX clock service for gfxclk.cpp (reviewed GFX DPM messages only; raw PMFW response, 0 = not sent).
bool smuGfxClkLive();
uint32_t smuGfxClkMsg(uint32_t msg, uint32_t &arg);
uint32_t smuGfxClkTable(uint32_t table, void *output, size_t bytes, uint32_t *state);
uint32_t smuGfxClkTableAnyKind(uint32_t table, void *output, size_t bytes, uint64_t vramLo, uint64_t vramHi, uint64_t *mc,
	uint32_t *kind);
// gfxclk.cpp: GFX clock policy thread (-rmbnogfxclk = off).
void gfxClkStart(KernelPatcher &patcher);
// smu.cpp: VCN/JPEG power for vcn.cpp (PowerDownVcn 0x06, PowerUpVcn 0x07, PowerDownJpeg 0x21, PowerUpJpeg 0x22,
// argument 0; raw PMFW response, 0 = not sent).
uint32_t smuVcnMsg(uint32_t msg);
// vcn.cpp: opt-in hardware video (ctl /Users/Shared/rmb-vcn.ctl = off | probe | on; see vcn.cpp).
void vcnStart();
void vcnRouteHwLibs(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
void vcnRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
bool vcnShowVideo();  // the accelerator keeps its video properties this boot (waits <= 5 s for the boot decision)
// display.cpp: publish (true) or hide (false) the accelerator's video properties (VideoToolbox hardware codecs).
void vcnVideoProps(bool show);

// Generic six-argument shape: SysV passes the first six integer args in registers, so wrapping
// a function with fewer args this way is safe.
using Fn6 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// GPU register aperture (BAR5, 512 KB), mapped on first use. Offsets are dword indices.
uint32_t mmioRead(uint32_t reg);
void mmioWrite(uint32_t reg, uint32_t value);
void applyGcGolden();
void haltGfxEngines(const char *why);
void smuExitReset(const char *tag);
void startGoldenGuard();
void diskLogStart(KernelPatcher &patcher);
void diskLogSleep(bool sleeping);
// Between WillSleep and wake: true. diskLogSync writes the log out now (waits ≤ ms).
bool diskLogSleeping();
// Sleep entry as a priority sleep/wake client sees it: WillSleep from full wake, but from dark
// wake only a capability change that drops the CPU (no WillSleep; 10-27 BM run: USB4 SLP, KIQ
// skip and wakepost all missed). S3 resume into dark wake = capability change that brings the CPU
// back, with no HasPoweredOn until full wake.
inline bool cpuCapChange(UInt32 type, void *arg, bool gain) {
	auto p = static_cast<const IOPMSystemCapabilityChangeParameters *>(arg);
	if (type != kIOMessageSystemCapabilityChange || !p)
		return false;
	if (!(p->changeFlags & (gain ? kIOPMSystemCapabilityDidChange : kIOPMSystemCapabilityWillChange)))
		return false;
	bool from = p->fromCapabilities & kIOPMSystemCapabilityCPU, to = p->toCapabilities & kIOPMSystemCapabilityCPU;
	return gain ? !from && to : from && !to;
}
inline bool sleepEntering(UInt32 type, void *arg) {
	return type == kIOMessageSystemWillSleep || cpuCapChange(type, arg, false);
}
// WillSleep..startKIQ/wake: Apple's KIQ submits fail at once instead of waiting 5 s on the MEC.
extern volatile bool gKiqSleepSkip;
void diskLogSync(unsigned ms);
void syncSdmaAddrConfig(const char *when);
void dmubPollerStart();
void dmubResetForLoad();
void inboxReset();
// VBIOSSMC (Rembrandt display SMU mailbox) message; returns the response, arg reg holds the granted value.
uint32_t vbiosSmcSend(uint32_t msg, uint32_t param);
void edpPanelPowerLost();
// System shutdown/sleep is coming: let DAL turn the eDP panel off for real (nullptr: stop).
void edpAllowPowerOff(const char *why);
// Send the panel power-off DAL sent earlier (and we NULLed) ourselves, before shutdown resets the GPU.
void edpPowerOffNow(const char *why);
// Post-S3: re-trigger a panel connect if WindowServer leaves the display on with no OTG running.
extern bool gWakeWatchArmed;
extern bool gWakePost;  // kern_start.cpp: force VBIOS POST at the next power_up_event
void panelWakeWatch();
void panelRetrain();
// PWRSEQ0 long-S3 death diagnostics (display.cpp): boot snapshot, arm after wake vbs_post, poll from dmub poller.
void pwrseqDiagBoot();
void pwrseqWakeArm();
void pwrseqWakeTick();
// eDP link watchdog (display.cpp): checks the sink's lane lock over AUX0 every 2 s, CPU retrain if lost.
void edpLinkWatchStart();
void auxDalTouch(uint32_t hw);
extern bool gEdpLinkPause;
extern volatile bool gEdpLinkKick;
// Ring of the last DAL register writes (reg = DAL index, hw = real index), for post-mortems.
void traceWrite(uint32_t reg, uint32_t hw, uint32_t val);
void traceDump(const char *why);
// eDP PHY A register block (RDPCSTX0_RDPCS_TX_CNTL, dword index); reads 0 while the PHY is gated.
constexpr uint32_t PhyA = 0x5df0;
// -rmbphypoll: CPU reads of RDPCSTX0 around every DAL access and every poller tick. Off by default:
// the RDPCS bus is shared with the DMUB firmware's PHY CR accesses (Linux never reads it from the CPU),
// and a cold PHY (S3 wake) died right after one of these reads.
bool phyPollOn();
// RDPCSTX0 for log lines: the real value with -rmbphypoll, else 0xeeeeeeee (not read).
uint32_t phyPeek();
// AppleGraphicsDeviceControl load address (its __TEXT), for AGDC statics.
void displaySetAgdcBase(mach_vm_address_t base);
// Find AppleGraphicsDeviceControl in the kmod list, accept it only if its in-memory LC_UUID is the
// 25G229 one and sWindowServerReady (+0x6914) lies in its __DATA; then displaySetAgdcBase(). kern_start.cpp.
bool resolveAgdcBase();
// kern_start.cpp: head of the kernel kmod list (nullptr before the patcher is ready).
struct kmod_info;
kmod_info *kmodList();
// iogdiag.cpp: IOGraphics extSetCursorPosition / controller gate hold / USB HID report counters, logged as
// "iogdiag:", "gatediag:", "hiddiag:" every 2 s. Exact LC_UUID + prologue per image. -rmbnoiogdiag = off.
void iogDiagRoute(KernelPatcher &patcher);
void displayRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
// AppleBacklight (AppleIntelPanelA under PNLF): panel table so it publishes "brightness".
bool displayRouteBacklight(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
// bldiag (display.cpp): AppleBacklight image range for the fb attribute caller log; nothing routed there.
void displayNoteBacklightImage(mach_vm_address_t address, size_t size);
bool setDisplayBacklight(KernelPatcher &patcher, mach_vm_address_t address, size_t size);
bool mmioAvailable();
class IOService;
bool mmioAdoptProvider(IOService *provider);
// Disable + drain the IH rings left running by a previous VM run (INTx stuck asserted otherwise).
void quiesceIh(const char *why);
void ihQuiesceAtBoot();
int gpuIrqState(char *buf, int n);
// get-wptr wrapper: calls, IH ring entries clflushed, entries that were stale in the CPU cache.
void ihFlushStats(uint32_t &calls, uint32_t &entries, uint32_t &stale);
// IH entries seen by the ihflush wrapper: per client id (low 5 bits) and DCE (client 4) per src id.
extern volatile uint32_t gIhClient[32], gIhDceSrc[256];
// NVMe + xHCI controllers (saved at PCI publish): PCI status and MSI / MSI-X target CPU + vector.
int pciIrqTargets(char *buf, int n);
// Line logged AND kept for the disk-stall panic text, which prints the kept lines first (the
// NVRAM panic record holds only ~2.5 KB, newest-lines selection cut the decisive ones before).
void panicKeep(const char *fmt, ...) __printflike(1, 2);
// Panic at once with the kept lines first ("disk log write stuck 0 s" header).
[[noreturn]] void keepPanic();
// Register write seen by our hooks; recorded only on CPU 0 inside the eoi0 window.
void eoi0Note(uint32_t reg, uint32_t val);
// 64-bit read from CPU-visible VRAM (BAR0) at a VRAM offset; ~0 if unavailable.
uint64_t vramRead64(uint64_t off);
// 32-bit write to CPU-visible VRAM (BAR0 window only).
bool vramWrite32(uint64_t off, uint32_t value);

// SMU 13.0.1 shim: replaces HWLibs' SMU SWIP entry points (sw_init/hw_init/hw_exit/sw_exit).
void smuRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// GC firmware substitution (Navi 23 microcode descriptors -> embedded Rembrandt images).
void firmwareRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// VRAM probes (framebuffer memory size, accelerator AMDHWMemory).
void probeRouteFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
bool probeRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// GCVM fault watchdog (logs page-table walks for new faults).
void vmWatchStart();
// -rmbnosubmit: page-table / IH address census against the kernel RAM map (bare-metal stray DMA hunt).
void ptScanInit(KernelPatcher &patcher);
void ptScan(const char *why);
void ptScanVmid(uint32_t vmid, bool quietIfSame = false);
uint64_t ptVmidBase(uint32_t vmid);
void mmhubDump(const char *why);  // 0 when the context is disabled
void gartCensus(const char *when);
// Save the GART at S3 entry; on wake put back PTEs that were valid then and are zero now.
void gartSave();
// Carve-out hashes across S3 (vramcrc.cpp): tags sleep, psp-sleep, wake start, after vbs_post, dark wake,
// full wake; vramCrcTrack keeps pipe 0 cursor/scanout addresses while the panel is on.
void vramCrcStep(const char *tag);
void vramCrcTrack();
void gartRestore(const char *when);

// Drops the vmware-svga IONDRVFramebuffer's system-sleep veto (guest ACPI S3).
void sleepRoute(KernelPatcher &patcher);
void usb4SleepStart();
// wlandiag.cpp: read-only MT7922 (14c3:0616) / GPP7 config + IOPCIFamily gate state at publish, +2/+10/+60 s.
// -rmbnowlandiag = off.
void wlanDiagStart();
// wlanfix.cpp: IOPCIBridge::initializeNub route; for the MT7922 nub: 10-bit tag requester off on its root port
// (before the first read), ASPM / L1SS off, secondary bus reset on the root port when it still does not answer
// config, devcap FLR bit hidden (wlanDevcapFilter*, called from the routed IOPCIDevice config reads).
// -rmbnowlanfix = off, -rmbwlanaspm = keep ASPM, -rmbwlannosbr = no bus reset, -rmbwlantag10 = keep 10-bit tags.
void wlanFixRoute(KernelPatcher &patcher);
uint32_t wlanFixState();  // 0 not run, 1 running, 2 done
void wlanFixReport(const char *tag);  // state line + port 10-bit tag re-check (clears bit 12 if set again)
uint32_t wlanDevcapFilter32(const void *dev, uint64_t off, uint32_t v);
uint16_t wlanDevcapFilter16(const void *dev, uint64_t off, uint16_t v);
void umdRoute(KernelPatcher &patcher);
void nvCopyPatch(mach_vm_address_t address, size_t size);  // nvcopy.cpp: NullMoth copy loop

// Required original SMU callbacks must be real before any hook installation.
bool smuRestoreReady();
uint32_t smuRestoreBlockers();
uint32_t smuDpmControl(void *,uint32_t,const void *,void *);
void smuDisallowGfxOff();
void smuHdpFlush();
void smuPreGpuCommandSequence();

bool restoredRouteGroup(KernelPatcher &,size_t,KernelPatcher::RouteRequest *,size_t,mach_vm_address_t,size_t);
mach_vm_address_t findSymbol(KernelPatcher &patcher, size_t groupIndex, const char *symbol, mach_vm_address_t address = 0, size_t size = 0);
mach_vm_address_t getKextBase(size_t groupIndex);
void logVbiosProperty(IOService *provider);
void ensureVbiosProperty(IOService *provider);
void rmbCheckpoint(const char *tag);
// Core Services trace window (kern_start.cpp): true only on the thread inside
// AmdRadeonControllerNavi23::createControllerCoreServices. Every register access it makes is logged
// and written to disk BEFORE the access, so a hard MMIO hang leaves the exact register as last line.
bool coreTraceActive();
void coreTraceAccess(const char *kind, uint32_t reg, uint32_t val, const void *ra);
void coreTraceResult(const char *kind, uint32_t reg, uint32_t val);
// Synchronous checkpoint inside the window, plain IOLog outside it (no IOSleep in foreign contexts).
void rmbStep(const char *tag);
bool routeStaticRequests(KernelPatcher &patcher, size_t groupIndex, KernelPatcher::RouteRequest *reqs, size_t count, mach_vm_address_t address = 0, size_t size = 0);

bool restoredRoutingFailed();
bool restoredPreStartCertified();
bool restoredBootstrapReady();
bool restoredTargetProvider(void *);
void restoredRoutingAbort();
void scanKmods();
void scanKmods(KernelPatcher *patcher);
bool isAmdKextLoadIndex(size_t loadIndex);
bool isKextHooked(size_t i);
void syncPciDeviceProperties(bool triggerRematch = false);
void syncSinglePciDeviceProps(void *dev, bool triggerRematch = false);
bool hookPciDeviceFromInstance(IOService *provider);
bool hookPciDeviceFromInstance(KernelPatcher &patcher, IOService *provider);
bool routePciDevid(KernelPatcher &patcher, size_t index = 0, mach_vm_address_t address = 0, size_t size = 0);
void pciEnableMemoryAndBusMaster(IOService *provider);
void ensureGpuReadyForStart(IOService *provider);
extern bool gControllerStarted;
class IOService;
extern IOService *gAcceleratorService;
extern volatile bool gAcceleratorStarted;
extern volatile bool gAccelProbeSeen;
// Boot stage tracker printed in every presdiag line (the only text left on screen when the GPU
// never lights up). Per slot: '-' not reached, 'E' entered (still inside = hung there), '1' ok, '0' failed.
enum StageSlot { StWrangler, StNavi2, StCtrl, StHwsProbe, StHwsStart, StAccProbe, StAccStart, StHWLibs, StCount };
extern volatile char gStage[StCount + 1];
inline void stageSet(StageSlot s, char c) { gStage[s] = c; }
extern volatile bool gHwEnginesStarted;
void publishAccelerator(IOService *svc);
void retractAccelerator(IOService *svc = nullptr);

