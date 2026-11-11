// Host test: in-kernel VFCT->ATY,bin_image builder == offline tools/make_lenovo_vbios.py output.
#include "../src/Rembrandt660M/vbios_augment.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
static std::vector<uint8_t> load(const char *p) {
	std::ifstream f(p, std::ios::binary);
	assert(f && p);
	return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char **argv) {
	assert(argc == 3);
	auto vfct = load(argv[1]), want = load(argv[2]);
	const char *why = "";
	size_t il = 0;
	auto img = rmbvbios::findVfctImage(vfct.data(), vfct.size(), &il, &why);
	assert(img && il == rmbvbios::kRawLen);
	std::vector<uint8_t> out(rmbvbios::kOutLen);
	size_t n = rmbvbios::augment(img, il, out.data(), out.size(), &why);
	if (!n) { std::printf("FAIL: %s\n", why); return 1; }
	assert(n == want.size() && out == want);
	// Negative cases: fail closed.
	std::vector<uint8_t> bad(img, img + il);
	bad[0x100] ^= 1;
	assert(!rmbvbios::augment(bad.data(), bad.size(), out.data(), out.size(), &why));
	assert(!rmbvbios::augment(img, il - 1, out.data(), out.size(), &why));
	auto v2 = vfct; v2[0] = 'X';
	assert(!rmbvbios::findVfctImage(v2.data(), v2.size(), &il, &why));
	auto v3 = vfct; v3[0x4C + 14] = 0x82;  // device 0x1682
	assert(!rmbvbios::findVfctImage(v3.data(), v3.size(), &il, &why));
	std::printf("PASS vbios_augment: %zu bytes identical to %s, 4 negative cases rejected\n", n, argv[2]);
	return 0;
}
