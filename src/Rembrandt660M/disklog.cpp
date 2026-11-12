//
//  Kernel message buffer → /private/var/log/rmb-msgbuf.log, fsynced every 0.5 s, appended across
//  boots (a short hung boot's log survives the next one; reset past 256 KB, after the last 768 KB
//  of the previous boot are copied to rmb-msgbuf.prev.log).
//  Bare metal has no serial port and a hung boot is powered off before logd flushes, so this
//  file is the only record of RembrandtGfx/AMD driver messages. Read it from Linux with
//  apfs-fuse (Data volume). `-rmbnodisklog` turns it off.
//

#include <Headers/kern_api.hpp>
#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode_if.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

namespace {

// bsd/sys/msgbuf.h
struct MsgBuf { int magic, size, bufx, bufr; char *bufc; };
MsgBuf **gMsgbufp;

// 2026-11-08: the Windows APFS reader used to pull this file (UFS Explorer) saved a 603 KB log fine but
// wrote 0 bytes once the appended file grew past ~0.9 MB (INFERRED: unregistered-version save-size
// limit). Restart the file when the previous content exceeds 256 KB, so it normally holds the current
// boot only (~300 KB for 2 min); a short hung boot (<256 KB) is still kept for the next one.
constexpr off_t MaxLog = 256 << 10;
constexpr const char *LogPath = "/private/var/log/rmb-msgbuf.log";
// 2026-11-12: a 3 h boot froze (GPU hang, screens green/black) and the restart above wiped the only
// record of its last minutes. Before restarting, the end of the previous file goes to PrevPath.
// The log grows ~400 KB/min on this machine (Wi-Fi scan + vblank spam), so 768 KB ≈ the last 2 min;
// kept under the ~0.9 MB limit of the Windows reader.
constexpr int PrevKeep = 768 << 10;
constexpr const char *PrevPath = "/private/var/log/rmb-msgbuf.prev.log";

// Stall detector (bare metal: an NVMe write timeout froze all disk I/O, the kernel kept running
// but nothing after it reached the file). gBusyAt = mach time a write+flush started (0 = idle),
// gFlushed = msgbuf index written so far. `-rmbnostallpanic` turns it off.
volatile uint64_t gBusyAt;
volatile int gFlushed;
bool gStallPanic = false;  // watchdog panics removed 10-31 (user)
// For the panic header: uptime (ms) when the log file opened, and completed flushes.
volatile uint64_t gOpenMs;
volatile unsigned gFlushes;
// System sleep (bare metal): from WillSleep until HasPoweredOn no disk writes (NVMe goes to D3
// under a blocked write), no stall panic. BM-only; the VM sleep path never had it.
volatile bool gSleepPause;
// mach time of WillSleep (stops during S3); sleep entry still running 60 s later = hung → panic
volatile uint64_t gSleepAt;
// mach time aliveThread first saw system_inshutdown; still running 40 s later = shutdown hung after
// our log closed (10-31: macOS Shut Down hung with no trace) → panic with the unsaved lines.
volatile uint64_t gShutAt;
volatile unsigned gSyncReq, gSyncDone;
// xnu system_inshutdown: set by reboot_kernel before "Killing all processes". Our open log file kept
// the Data volume busy → every BM shutdown hung at "unmount(0) /System/Volumes/Data" (10-27).
int *gInShutdown;
bool shuttingDown() { return gInShutdown && *gInShutdown; }

void put(vnode_t vp, vfs_context_t ctx, off_t &off, const char *p, int len) {
	if (len <= 0)
		return;
	int resid = 0;
	if (!vn_rdwr(UIO_WRITE, vp, const_cast<char *>(p), len, off, UIO_SYSSPACE, IO_UNIT | IO_NOAUTH,
		vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)))
		off += len - resid;
}

// Copy the last PrevKeep bytes of LogPath (size bytes long), from a whole line on, to PrevPath.
// Runs before LogPath is truncated, so a hang in here loses nothing.
void savePrevTail(vfs_context_t ctx, off_t size) {
	off_t from = size > PrevKeep ? size - PrevKeep : 0;
	int len = static_cast<int>(size - from);
	char *buf = len > 0 ? static_cast<char *>(IOMalloc(static_cast<vm_size_t>(len))) : nullptr;
	if (!buf)
		return;
	vnode_t src = nullptr, dst = nullptr;
	int resid = 0;
	if (!vnode_open(LogPath, FREAD, 0, 0, &src, ctx)) {
		if (vn_rdwr(UIO_READ, src, buf, len, from, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
				vfs_context_proc(ctx)))
			resid = len;
		vnode_close(src, FREAD, ctx);
	} else {
		resid = len;
	}
	int got = len - resid, s = 0;
	if (from)
		while (s < got && buf[s++] != '\n') {
		}
	if (got > s && !vnode_open(PrevPath, O_CREAT | FWRITE, 0644, 0, &dst, ctx)) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_data_size, 0);
		vnode_setattr(dst, &va, ctx);
		char hdr[128];
		int h = snprintf(hdr, sizeof(hdr), "======== disklog: end of the previous boot (last %d of %lld KB) ========\n",
			(got - s) >> 10, static_cast<long long>(size >> 10));
		off_t o = 0;
		put(dst, ctx, o, hdr, h < static_cast<int>(sizeof(hdr)) ? h : static_cast<int>(sizeof(hdr)) - 1);
		put(dst, ctx, o, buf + s, got - s);
		VNOP_FSYNC(dst, MNT_WAIT, ctx);
		VNOP_IOCTL(dst, F_FULLFSYNC, nullptr, 0, ctx);
		vnode_close(dst, FWRITE, ctx);
	}
	IOFree(buf, static_cast<vm_size_t>(len));
}

void diskLogThread(void *, wait_result_t) {
	vfs_context_t ctx = vfs_context_create(nullptr);
	vnode_t vp = nullptr;
	off_t off = 0;
	int last = 0;
	char *lastBuf = nullptr;
	unsigned ticks = 0;
	for (;;) {
		// Sleep entry: every 10 ms, so the lines just before a hang reach the disk.
		for (int i = 0; i < (gSleepPause ? 1 : 50) && gSyncReq == gSyncDone && !shuttingDown(); i++)
			IOSleep(10);
		bool closing = shuttingDown();
		if (closing && !vp)
			return;
		unsigned req = gSyncReq;
		MsgBuf *mb = *gMsgbufp;
		if (!mb || !mb->bufc || mb->size <= 0) {
			gSyncDone = req;
			continue;
		}
		// Heartbeat: if the file ends without one of these near its last line, the machine hung.
		if (vp && ++ticks % 20 == 0)
			printf("disklog: alive %u s\n", ticks / 2);
		int x = mb->bufx;
		gBusyAt = mach_absolute_time();
		if (vp && mb->bufc != lastBuf) {
			// msgbuf= resize (log_setsize) moved the buffer and copied the old text to its start.
			last = x < last ? x : last;
			lastBuf = mb->bufc;
		}
		if (!vp) {
			// No path lookups during the early root/cryptex mount switch (~1 s): a namei racing it
			// panicked once ("Taking non-sleepable RW lock with preemption enabled", 10-30 13:15).
			// msgbuf keeps the early lines, so opening later loses nothing.
			uint64_t nowNs;
			absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
			if (nowNs < 5000000000ULL) {
				gBusyAt = 0;
				gSyncDone = req;
				continue;
			}
			// Fails (EROFS/ENOENT) until the Data volume is mounted over /private/var.
			if (vnode_open(LogPath, O_CREAT | FWRITE, 0644, 0, &vp, ctx)) {
				vp = nullptr;
				gBusyAt = 0;
				gSyncDone = req;
				continue;
			}
			struct vnode_attr va;
			VATTR_INIT(&va);
			VATTR_WANTED(&va, va_data_size);
			bool sized = !vnode_getattr(vp, &va, ctx);
			off = sized ? static_cast<off_t>(va.va_data_size) : MaxLog + 1;
			if (off > MaxLog) {
				if (sized)
					savePrevTail(ctx, off);
				off = 0;
				VATTR_INIT(&va);
				VATTR_SET(&va, va_data_size, 0);
				vnode_setattr(vp, &va, ctx);
			}
			static const char sep[] = "\n\n======== disklog: new boot ========\n";
			put(vp, ctx, off, sep, sizeof(sep) - 1);
			if (mb->bufc[x])  // wrapped: oldest text starts at the write pointer
				put(vp, ctx, off, mb->bufc + x, mb->size - x);
			put(vp, ctx, off, mb->bufc, x);
			lastBuf = mb->bufc;
			uint64_t ns;
			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			gOpenMs = ns / 1000000;
		} else if (x == last && !closing) {
			gBusyAt = 0;
			gSyncDone = req;
			continue;
		} else if (x == last) {
		} else if (x > last) {
			put(vp, ctx, off, mb->bufc + last, x - last);
		} else {
			put(vp, ctx, off, mb->bufc + last, mb->size - last);
			put(vp, ctx, off, mb->bufc, x);
		}
		last = x;
		VNOP_FSYNC(vp, MNT_WAIT, ctx);
		// Plain fsync leaves the data in the NVMe write cache; a power-off then loses it (first
		// bare-metal disklog run kept only the first 128 KB write).
		VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);
		gFlushed = x;
		gFlushes++;
		gBusyAt = 0;
		gSyncDone = req;
		if (closing && vp) {
			static const char bye[] = "======== disklog: shutdown, log closed ========\n";
			put(vp, ctx, off, bye, sizeof(bye) - 1);
			VNOP_FSYNC(vp, MNT_WAIT, ctx);
			VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);
			vnode_close(vp, FWRITE, ctx);
			vfs_context_rele(ctx);
			return;
		}
	}
}

bool noise(const char *l, int n) {
	static const char *const pats[] = {"display_change_handler", "vendor_doDeviceAttribute", "IOPresentment",
		"IOP: ", "rmb: alive", "Sandbox", "apfs_trim", "set_id_mode", "AppleKeyStore", "IOBluetooth"};
	for (auto p : pats)
		for (int i = 0, m = static_cast<int>(strlen(p)); i + m <= n; i++)
			if (!strncmp(l + i, p, m))
				return true;
	return false;
}

char gKeep[2000];
int gKeepLen;

}  // namespace

bool rmbFileRead(const char *path, char *buf, size_t n, int64_t *mtime) {
	if (buf && n)
		buf[0] = 0;
	vfs_context_t ctx = vfs_context_create(nullptr);
	vnode_t vp = nullptr;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx)) {
		vfs_context_rele(ctx);
		return false;
	}
	if (mtime) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_WANTED(&va, va_modify_time);
		*mtime = vnode_getattr(vp, &va, ctx) ? 0 : va.va_modify_time.tv_sec;
	}
	if (buf && n > 1) {
		int resid = 0;
		if (!vn_rdwr(UIO_READ, vp, buf, static_cast<int>(n - 1), 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx),
				&resid, vfs_context_proc(ctx)))
			buf[n - 1 - resid] = 0;
	}
	vnode_close(vp, FREAD, ctx);
	vfs_context_rele(ctx);
	return true;
}

bool rmbFileWrite(const char *path, const char *s) {
	vfs_context_t ctx = vfs_context_create(nullptr);
	vnode_t vp = nullptr;
	if (vnode_open(path, O_CREAT | FWRITE, 0644, 0, &vp, ctx)) {
		vfs_context_rele(ctx);
		return false;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_SET(&va, va_data_size, 0);
	int e = vnode_setattr(vp, &va, ctx), resid = 0;
	if (!e)
		e = vn_rdwr(UIO_WRITE, vp, const_cast<char *>(s), static_cast<int>(strlen(s)), 0, UIO_SYSSPACE,
			IO_UNIT | IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	VNOP_FSYNC(vp, MNT_WAIT, ctx);
	VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);
	vnode_close(vp, FWRITE, ctx);
	vfs_context_rele(ctx);
	return !e && !resid;
}

void panicKeep(const char *fmt, ...) {
	char line[800];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	n = n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1;
	printf("RembrandtGfx: %s\n", line);
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	int room = static_cast<int>(sizeof(gKeep)) - gKeepLen;
	int k = snprintf(gKeep + gKeepLen, room, "[%llu.%02llu] %s\n", ns / 1000000000ULL, ns / 10000000ULL % 100, line);
	if (k < room)
		gKeepLen += k;
	else
		gKeep[gKeepLen] = 0;  // full: keep the earlier lines whole
}

namespace {

// Sleep entry stuck: from the io=0x80 PM log ("pmT 0xTHR NAME EVT a b") after the last "trace point 0x14"
// (drivers powering down) list the calls that started but never ended: 38/-38 ProgramHardware
// (setPowerState), 39/-39 InformDriverPreChange, 17 WillChange vs 29 ChangeDone = the driver the
// sleep waits for. Disk is already off there, so this is the only place it can be read.
int pmOpen(MsgBuf *mb, char *out, int size) {
	struct Ent { char name[32]; int prog, pre, chg; };
	static Ent ents[96];
	int ne = 0, sz = mb->size, x = mb->bufx;
	auto at = [&](int i) { return mb->bufc[((x + i) % sz + sz) % sz]; };  // i = 0 .. sz-1, oldest first
	static const char key[] = "trace point 0x14";
	int start = -1;
	for (int i = sz - 16; i >= 0 && start < 0; i--) {
		int j = 0;
		while (j < 16 && at(i + j) == key[j])
			j++;
		if (j == 16)
			start = i;
	}
	if (start < 0)
		return snprintf(out, size, "pmopen: no trace point 0x14 in msgbuf\n");
	static char line[160];
	int lines = 0;
	for (int i = start; i < sz;) {
		int n = 0;
		while (i < sz && at(i) != '\n') {
			if (n < static_cast<int>(sizeof(line)) - 1)
				line[n++] = at(i);
			i++;
		}
		i++;
		line[n] = 0;
		char *p = const_cast<char *>(strstr(line, "pm"));
		if (!p || p[2] < '0' || p[2] > '9')
			continue;
		// tokens from the end: b, a, evt; name = between thread token and evt
		char *tok[64];
		int nt = 0;
		for (char *q = p; *q && nt < 64;) {
			while (*q == ' ')
				*q++ = 0;
			if (*q)
				tok[nt++] = q;
			while (*q && *q != ' ')
				q++;
		}
		if (nt < 6 || strncmp(tok[1], "0x", 2))
			continue;
		int evt = 0, neg = tok[nt - 3][0] == '-';
		for (const char *d = tok[nt - 3] + neg; *d >= '0' && *d <= '9'; d++)
			evt = evt * 10 + (*d - '0');
		if (neg)
			evt = -evt;
		if (evt != 38 && evt != -38 && evt != 39 && evt != -39 && evt != 17 && evt != 29)
			continue;
		char name[32];
		int k = 0;
		for (int t = 2; t < nt - 3; t++)
			for (const char *c = tok[t]; *c && k < 30; c++)
				name[k++] = *c;
		name[k] = 0;
		lines++;
		int e = 0;
		while (e < ne && strcmp(ents[e].name, name))
			e++;
		if (e == ne) {
			if (ne == 96)
				continue;
			memcpy(ents[ne].name, name, k + 1);
			ents[ne].prog = ents[ne].pre = ents[ne].chg = 0;
			ne++;
		}
		Ent &en = ents[e];
		if (evt == 38 || evt == -38)
			en.prog += evt > 0 ? 1 : -1;
		else if (evt == 39 || evt == -39)
			en.pre += evt > 0 ? 1 : -1;
		else
			en.chg += evt == 17 ? 1 : -1;
	}
	int o = snprintf(out, size, "pmopen: %d pm lines, %d drivers after 0x14; open (prog=setPowerState pre=willChange chg):\n",
		lines, ne);
	for (int e = 0; e < ne && o < size - 1; e++)
		if (ents[e].prog > 0 || ents[e].pre > 0 || ents[e].chg > 0)
			o += snprintf(out + o, size - o, " %s prog %d pre %d chg %d\n", ents[e].name, ents[e].prog, ents[e].pre,
				ents[e].chg);
	return o < size ? o : size - 1;
}

// The disk writer has been stuck > 15 s: panic with the kernel lines it could not save, so they
// land in the NVRAM panic record (AAPL,PanicInfo*, readable from Linux efivars).
[[noreturn]] void stallPanic(MsgBuf *mb, unsigned stuckS) {
	// The NVRAM panic record keeps only ~2.5 KB of the message (10-25 12:47 boot cut it there and
	// lost everything after the xHCI spam) → our own lines go FIRST, then "--", then other lines.
	static char raw[48 << 10], out[2300];
	int x = mb->bufx, from = gFlushed, n = 0;
	int avail = x >= from ? x - from : mb->size - from + x;
	if (avail > static_cast<int>(sizeof(raw)))
		from = (x - static_cast<int>(sizeof(raw)) + mb->size) % mb->size, avail = sizeof(raw);
	for (int i = 0; i < avail; i++)
		raw[n++] = mb->bufc[(from + i) % mb->size];
	// Kept snapshot lines first, then the newest lines that fit, oldest first.
	static char ours[sizeof(out)], other[sizeof(out)];
	auto pick = [&](char *dst, int size, bool own) {
		int o = size - 1;
		dst[o] = 0;
		for (int e = n; e > 0 && o > 0;) {
			int s = e - 1;
			while (s > 0 && raw[s - 1] != '\n')
				s--;
			int len = e - s;
			bool mine = false;
			for (int i = s; !mine && i + 12 <= e; i++)
				mine = !strncmp(raw + i, "RembrandtGfx", 12) || (gSleepAt && !strncmp(raw + i, "PMRD: ", 6));
			if (len > 1 && mine == own && !noise(raw + s, len)) {
				if (len > o)
					break;
				o -= len;
				memcpy(dst + o, raw + s, len);
			}
			e = s;
		}
		return dst + o;
	};
	int k = 0;
	if (gSleepAt)
		k = pmOpen(mb, out, 700);
	int keep = gKeepLen < static_cast<int>(sizeof(out)) - k - 400 ? gKeepLen : static_cast<int>(sizeof(out)) - k - 400;
	memcpy(out + k, gKeep, keep);
	k += keep;
	memcpy(out + k, "--\n", 4);
	k += 3;
	int oursRoom = static_cast<int>(sizeof(out)) - k - 200;  // leave ~200 B for other drivers' lines
	if (gShutAt && oursRoom > 900)
		oursRoom = 900;  // shutdown: xnu/IOKit lines (unmount, PM halt) matter as much as ours
	const char *mine = pick(ours, oursRoom > 1 ? oursRoom : 1, true);
	int m = static_cast<int>(strlen(mine));
	memcpy(out + k, mine, m);
	k += m;
	memcpy(out + k, "--\n", 4);
	k += 3;
	const char *rest = pick(other, static_cast<int>(sizeof(out)) - k, false);
	memcpy(out + k, rest, strlen(rest) + 1);
	for (char *p = out; *p; p++)
		if ((*p < 0x20 && *p != '\n') || *p > 0x7e)
			*p = '?';
	if (gShutAt)
		panic("rmb: shutdown stuck %u s after system_inshutdown (%u flushes, %d B unsaved); log:\n%s", stuckS,
		    gFlushes, avail, out);
	if (gSleepAt)
		panic("rmb: sleep entry stuck %u s (%u flushes, %d B unsaved); log:\n%s", stuckS, gFlushes, avail, out);
	panic("rmb: disk log write stuck %u s (opened %llu ms, %u flushes, %d B unsaved); log:\n%s", stuckS,
		static_cast<unsigned long long>(gOpenMs), gFlushes, avail, out);
}

}  // namespace

// WillSleep: write out everything logged so far (≤3 s); the writer keeps going so the sleep entry
// is on disk up to the NVMe power-down. Stall panic off until HasPoweredOn restarts its clock.
void diskLogSleep(bool sleeping) {
	if (!gMsgbufp)
		return;
	if (sleeping) {
		gSleepAt = mach_absolute_time();
		printf("RembrandtGfx: sleep: disklog flush, writer on%s\n", gStallPanic ? ", sleep-entry panic at 35 s" : "");
		unsigned req = ++gSyncReq;
		for (int i = 0; i < 300 && static_cast<int>(gSyncDone - req) < 0; i++)
			IOSleep(10);
		gSleepPause = true;
		printf("RembrandtGfx: sleep: disklog %s\n", gSyncDone == req ? "flushed" : "flush NOT done in 3 s");
	} else {
		if (gBusyAt)
			gBusyAt = mach_absolute_time();
		gSleepPause = false;
		gSleepAt = 0;
		printf("RembrandtGfx: wake: stall panic back on\n");
	}
}

bool diskLogSleeping() { return gSleepPause; }

void diskLogSync(unsigned ms) {
	if (!gMsgbufp)
		return;
	unsigned req = ++gSyncReq;
	for (unsigned i = 0; i < ms / 10 && static_cast<int>(gSyncDone - req) < 0; i++)
		IOSleep(10);
}

// Panic now with the kept lines (eoi0: CPU 0 never came back, xnu's NMIPI would lose them).
void keepPanic() { stallPanic(*gMsgbufp, 0); }


void diskLogStart(KernelPatcher &patcher) {
	if (checkKernelArgument("-rmbnodisklog"))
		return;
	gMsgbufp = reinterpret_cast<MsgBuf **>(patcher.solveSymbol(KernelPatcher::KernelID, "_msgbufp"));
	patcher.clearError();
	if (!gMsgbufp) {
		LOG("disklog: _msgbufp not found");
		return;
	}
	gInShutdown = reinterpret_cast<int *>(patcher.solveSymbol(KernelPatcher::KernelID, "_system_inshutdown"));
	patcher.clearError();
	if (!gInShutdown)
		LOG("disklog: _system_inshutdown not found, log stays open at shutdown");
	thread_t t;
	if (kernel_thread_start(diskLogThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	LOG("disklog: writing kernel log to %s", LogPath);
}
