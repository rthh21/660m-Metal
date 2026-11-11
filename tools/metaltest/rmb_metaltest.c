// rmb_metaltest — prebuilt x86_64 Metal self-test for the Lenovo Radeon 660M (Rembrandt660M).
// User-space only: no kext, no MMIO, no install. Built offline in WSL without a macOS SDK
// (stub dylibs, see build.sh), so no Xcode/CLT is needed on the target. NOT TESTED until run on
// the Lenovo. Steps (each one prints PASS/FAIL, the tool stops at the first failure):
//   1. MTLCopyAllDevices / MTLCreateSystemDefaultDevice: name, registryID, families.
//   2. runtime MSL compile (AMD Metal compiler) + compute: c[i] = 2*a[i] + b[i], 1M floats, every
//      element checked on the CPU.
//   3. render: 64x64 RGBA8 private texture, clear blue, one red triangle (lower-left half),
//      blit to a shared buffer, every non-diagonal pixel checked.
// Each command buffer is polled for up to 10 s; a timeout means the GPU did not retire the work.
// Exit code: 0 all PASS, 1 no device, 2 compile/pipeline error, 3 timeout, 4 GPU error status,
// 5 wrong results.

typedef void *id;
typedef void *SEL;
typedef unsigned long NSUInteger;
typedef signed char BOOL;
typedef struct { NSUInteger width, height, depth; } MTLSize;
typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { double red, green, blue, alpha; } MTLClearColor;

extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern void objc_msgSend(void);
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
extern id MTLCreateSystemDefaultDevice(void);
extern id MTLCopyAllDevices(void);
extern int printf(const char *, ...);
extern int fflush(void *);
extern int usleep(unsigned int);

#define S(n) sel_registerName(n)
#define MSG(ret, ...) ((ret(*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

static id m0(id o, const char *s) { return MSG(id)(o, S(s)); }
static id m1(id o, const char *s, id a) { return MSG(id, id)(o, S(s), a); }
static NSUInteger mu(id o, const char *s) { return MSG(NSUInteger)(o, S(s)); }
static void setu(id o, const char *s, NSUInteger v) { MSG(void, NSUInteger)(o, S(s), v); }
static BOOL responds(id o, const char *s) { return MSG(BOOL, SEL)(o, S("respondsToSelector:"), S(s)); }
static const char *cstr(id s) { return s ? MSG(const char *)(s, S("UTF8String")) : "(nil)"; }
static id nsstr(const char *c) { return MSG(id, const char *)((id)objc_getClass("NSString"), S("stringWithUTF8String:"), c); }
static const char *errstr(id e) { return e ? cstr(m0(e, "localizedDescription")) : "(no NSError)"; }
static void out(void) { fflush(0); }

static const char *kSrc =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void rmb_add(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],\n"
    "                    device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    "    c[i] = a[i] * 2.0f + b[i];\n"
    "}\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "constant float2 kTri[3] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0) };\n"
    "vertex VOut rmb_vs(uint vid [[vertex_id]]) { VOut o; o.pos = float4(kTri[vid], 0.0, 1.0); return o; }\n"
    "fragment float4 rmb_fs(VOut in [[stage_in]]) { return float4(1.0, 0.0, 0.0, 1.0); }\n";

// 4 = completed, 5 = error. Returns status, or 99 on timeout.
static NSUInteger waitCb(id cb, const char *what) {
	for (int ms = 0; ms < 10000; ms++) {
		NSUInteger st = mu(cb, "status");
		if (st >= 4) {
			if (st == 5)
				printf("[%s] FAIL: command buffer status 5 (error): %s\n", what, errstr(m0(cb, "error")));
			else if (responds(cb, "GPUStartTime"))
				printf("[%s] GPU time %.3f ms\n", what,
				       (MSG(double)(cb, S("GPUEndTime")) - MSG(double)(cb, S("GPUStartTime"))) * 1000.0);
			out();
			return st;
		}
		usleep(1000);
	}
	printf("[%s] FAIL: TIMEOUT 10 s, status still %lu (GPU did not retire the work)\n", what, mu(cb, "status"));
	out();
	return 99;
}

static int listDevices(void) {
	id all = MTLCopyAllDevices();
	NSUInteger n = all ? mu(all, "count") : 0;
	printf("[devices] MTLCopyAllDevices count = %lu\n", n);
	for (NSUInteger i = 0; i < n; i++) {
		id d = MSG(id, NSUInteger)(all, S("objectAtIndex:"), i);
		printf("[devices]   %lu: %s registryID 0x%llx lowPower %d headless %d\n", i, cstr(m0(d, "name")),
		       (unsigned long long)mu(d, "registryID"), (int)MSG(BOOL)(d, S("isLowPower")),
		       (int)MSG(BOOL)(d, S("isHeadless")));
	}
	out();
	return (int)n;
}

int main(void) {
	void *pool = objc_autoreleasePoolPush();
	printf("rmb_metaltest 1 (Rembrandt660M, x86_64, no SDK build)\n");
	int n = listDevices();
	id dev = MTLCreateSystemDefaultDevice();
	if (!dev) {
		printf("[device] FAIL: MTLCreateSystemDefaultDevice = nil (count %d) -> no Metal device\n", n);
		out();
		return 1;
	}
	printf("[device] PASS: default = %s registryID 0x%llx maxWorkingSet %llu MB maxBuffer %llu MB\n",
	       cstr(m0(dev, "name")), (unsigned long long)mu(dev, "registryID"),
	       responds(dev, "recommendedMaxWorkingSetSize") ? (unsigned long long)mu(dev, "recommendedMaxWorkingSetSize") >> 20 : 0ULL,
	       responds(dev, "maxBufferLength") ? (unsigned long long)mu(dev, "maxBufferLength") >> 20 : 0ULL);
	if (responds(dev, "supportsFamily:")) {
		// MTLGPUFamilyMac2 = 2002, MTLGPUFamilyCommon3 = 3003, MTLGPUFamilyMetal3 = 5001
		printf("[device] families: Mac2 %d Common3 %d Metal3 %d\n", (int)MSG(BOOL, long)(dev, S("supportsFamily:"), 2002),
		       (int)MSG(BOOL, long)(dev, S("supportsFamily:"), 3003), (int)MSG(BOOL, long)(dev, S("supportsFamily:"), 5001));
	}
	out();

	id q = m0(dev, "newCommandQueue");
	id err = 0;
	id lib = MSG(id, id, id, id *)(dev, S("newLibraryWithSource:options:error:"), nsstr(kSrc), 0, &err);
	if (!lib) {
		printf("[compile] FAIL: newLibraryWithSource: %s\n", errstr(err));
		out();
		return 2;
	}
	id fAdd = m1(lib, "newFunctionWithName:", nsstr("rmb_add"));
	id fVs = m1(lib, "newFunctionWithName:", nsstr("rmb_vs"));
	id fFs = m1(lib, "newFunctionWithName:", nsstr("rmb_fs"));
	err = 0;
	id cps = fAdd ? MSG(id, id, id *)(dev, S("newComputePipelineStateWithFunction:error:"), fAdd, &err) : 0;
	if (!q || !cps || !fVs || !fFs) {
		printf("[compile] FAIL: queue %p compute pipeline %p vs %p fs %p: %s\n", q, cps, fVs, fFs, errstr(err));
		out();
		return 2;
	}
	printf("[compile] PASS: MSL compiled, compute pipeline maxThreadsPerThreadgroup %lu\n",
	       mu(cps, "maxTotalThreadsPerThreadgroup"));
	out();

	// ---- compute ----
	const NSUInteger N = 1u << 20;
	id bA = MSG(id, NSUInteger, NSUInteger)(dev, S("newBufferWithLength:options:"), N * 4, 0);
	id bB = MSG(id, NSUInteger, NSUInteger)(dev, S("newBufferWithLength:options:"), N * 4, 0);
	id bC = MSG(id, NSUInteger, NSUInteger)(dev, S("newBufferWithLength:options:"), N * 4, 0);
	if (!bA || !bB || !bC) {
		printf("[compute] FAIL: buffer allocation\n");
		out();
		return 2;
	}
	float *a = (float *)MSG(void *)(bA, S("contents"));
	float *b = (float *)MSG(void *)(bB, S("contents"));
	float *c = (float *)MSG(void *)(bC, S("contents"));
	for (NSUInteger i = 0; i < N; i++) {
		a[i] = (float)i;
		b[i] = 1.0f;
		c[i] = -7.0f;
	}
	NSUInteger tpg = mu(cps, "maxTotalThreadsPerThreadgroup");
	tpg = tpg >= 256 ? 256 : (tpg >= 64 ? 64 : 32);
	id cb = m0(q, "commandBuffer");
	id ce = m0(cb, "computeCommandEncoder");
	m1(ce, "setComputePipelineState:", cps);
	MSG(void, id, NSUInteger, NSUInteger)(ce, S("setBuffer:offset:atIndex:"), bA, 0, 0);
	MSG(void, id, NSUInteger, NSUInteger)(ce, S("setBuffer:offset:atIndex:"), bB, 0, 1);
	MSG(void, id, NSUInteger, NSUInteger)(ce, S("setBuffer:offset:atIndex:"), bC, 0, 2);
	MTLSize groups = {N / tpg, 1, 1}, threads = {tpg, 1, 1};
	MSG(void, MTLSize, MTLSize)(ce, S("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
	m0(ce, "endEncoding");
	m0(cb, "commit");
	NSUInteger st = waitCb(cb, "compute");
	if (st == 99)
		return 3;
	if (st != 4)
		return 4;
	NSUInteger bad = 0, firstBad = N;
	for (NSUInteger i = 0; i < N; i++)
		if (c[i] != (float)i * 2.0f + 1.0f) {
			if (firstBad == N)
				firstBad = i;
			bad++;
		}
	if (bad) {
		printf("[compute] FAIL: %lu of %lu wrong, first at %lu: got %f expected %f\n", bad, N, firstBad,
		       (double)c[firstBad], (double)((float)firstBad * 2.0f + 1.0f));
		out();
		return 5;
	}
	printf("[compute] PASS: %lu of %lu results exact (threadgroup %lu)\n", N, N, tpg);
	out();

	// ---- render ----
	id td = MSG(id, NSUInteger, NSUInteger, NSUInteger, BOOL)((id)objc_getClass("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 70 /* RGBA8Unorm */, 64, 64, 0);
	setu(td, "setUsage:", 1 | 4);       // ShaderRead | RenderTarget
	setu(td, "setStorageMode:", 2);     // Private
	id tex = m1(dev, "newTextureWithDescriptor:", td);
	id rpd = m0(m0((id)objc_getClass("MTLRenderPipelineDescriptor"), "alloc"), "init");
	m1(rpd, "setVertexFunction:", fVs);
	m1(rpd, "setFragmentFunction:", fFs);
	setu(MSG(id, NSUInteger)(m0(rpd, "colorAttachments"), S("objectAtIndexedSubscript:"), 0), "setPixelFormat:", 70);
	err = 0;
	id rps = MSG(id, id, id *)(dev, S("newRenderPipelineStateWithDescriptor:error:"), rpd, &err);
	id rb = MSG(id, NSUInteger, NSUInteger)(dev, S("newBufferWithLength:options:"), 64 * 64 * 4, 0);
	if (!tex || !rps || !rb) {
		printf("[render] FAIL: texture %p render pipeline %p buffer %p: %s\n", tex, rps, rb, errstr(err));
		out();
		return 2;
	}
	unsigned char *px = (unsigned char *)MSG(void *)(rb, S("contents"));
	for (NSUInteger i = 0; i < 64 * 64 * 4; i++)
		px[i] = 0x5a;
	id pass = m0((id)objc_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
	id ca = MSG(id, NSUInteger)(m0(pass, "colorAttachments"), S("objectAtIndexedSubscript:"), 0);
	m1(ca, "setTexture:", tex);
	setu(ca, "setLoadAction:", 2);   // Clear
	setu(ca, "setStoreAction:", 1);  // Store
	MTLClearColor blue = {0.0, 0.0, 1.0, 1.0};
	MSG(void, MTLClearColor)(ca, S("setClearColor:"), blue);
	cb = m0(q, "commandBuffer");
	id re = m1(cb, "renderCommandEncoderWithDescriptor:", pass);
	m1(re, "setRenderPipelineState:", rps);
	MSG(void, NSUInteger, NSUInteger, NSUInteger)(re, S("drawPrimitives:vertexStart:vertexCount:"), 3 /* Triangle */, 0, 3);
	m0(re, "endEncoding");
	id be = m0(cb, "blitCommandEncoder");
	MTLOrigin o0 = {0, 0, 0};
	MTLSize sz = {64, 64, 1};
	MSG(void, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, NSUInteger)(be,
	    S("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
	    tex, 0, 0, o0, sz, rb, 0, 64 * 4, 64 * 64 * 4);
	m0(be, "endEncoding");
	m0(cb, "commit");
	st = waitCb(cb, "render");
	if (st == 99)
		return 3;
	if (st != 4)
		return 4;
	// Pixel centre in NDC: x = (col+0.5)/32-1, y = 1-(row+0.5)/32. Inside the triangle when x+y < 0.
	// Pixels whose centre lies within one pixel of the diagonal are skipped (rasterisation rules).
	NSUInteger red = 0, blueN = 0, wrong = 0, checked = 0, wr = 0, wc = 0;
	for (NSUInteger row = 0; row < 64; row++)
		for (NSUInteger col = 0; col < 64; col++) {
			long s2 = (long)(2 * col + 1) - (long)(2 * row + 1);  // 64*(x+y) in pixel units
			if (s2 > -2 && s2 < 2)
				continue;
			unsigned char *p = px + (row * 64 + col) * 4;
			int isRed = p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255;
			int isBlue = p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255;
			checked++;
			red += isRed;
			blueN += isBlue;
			if ((s2 < 0 && !isRed) || (s2 > 0 && !isBlue)) {
				if (!wrong) {
					wr = row;
					wc = col;
				}
				wrong++;
			}
		}
	if (wrong) {
		unsigned char *p = px + (wr * 64 + wc) * 4;
		printf("[render] FAIL: %lu of %lu pixels wrong (red %lu blue %lu), first row %lu col %lu = %02x %02x %02x %02x\n",
		       wrong, checked, red, blueN, wr, wc, p[0], p[1], p[2], p[3]);
		out();
		return 5;
	}
	printf("[render] PASS: %lu pixels checked (red %lu, blue %lu)\n", checked, red, blueN);
	printf("RESULT: PASS (Metal device, MSL compile, compute and render verified on %s)\n", cstr(m0(dev, "name")));
	out();
	objc_autoreleasePoolPop(pool);
	return 0;
}
