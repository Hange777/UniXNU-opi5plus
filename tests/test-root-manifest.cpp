// Host test for package/SdHost/RootManifest.h: build the manifest `unixnu build userdata` writes, check
// that the parser reproduces its windows, then break one rule at a time and check it refuses the page.
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -I package/SdHost tests/test-root-manifest.cpp -o /tmp/test-root-manifest
//   /tmp/test-root-manifest [path/to/userdata-root.img]
//
// The synthetic page uses the numbers of the image this repository's dev board was built against
// (iPadOS 17.7.11, 21H461): a 32,203,324,928-byte partition (LBA 32768..62929886 of a 512-byte-sector
// card), an 8,738 MiB image, OS at 1 MiB and SystemOS at 0x16a600000. The optional argument parses the
// first 4 KiB of a real image, which is what proves the kernel will accept the artefact that was built
// rather than only the one this test built.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "RootManifest.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
	do {                                                                        \
		if (!(condition)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
			++failures;                                                         \
		}                                                                       \
	} while (0)

using Page = std::vector<unsigned char>;

constexpr uint64_t kPartitionBytes = 32203324928ull;  // LBA 32768..62929886 of the board's card
constexpr uint64_t kImageBytes = 9382658048ull;       // the image `unixnu build userdata` wrote
constexpr uint64_t kOsOffset = 0x100000ull;           // 1 MiB, the manifest block rounded up
constexpr uint64_t kOsBytes = 6077580800ull;
constexpr uint64_t kSystemOsOffset = 0x16a600000ull;
constexpr uint64_t kSystemOsBytes = 3303014400ull;

// ---------------------------------------------------------------- SHA-256
// A compact FIPS 180-4 implementation. The manifest's own digest is the only thing the parser needs it
// for; pulling a library in would make this test depend on something the kernel does not have.
struct Sha256 {
	uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
	                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	uint64_t length = 0;
	unsigned char block[64] = {};
	unsigned used = 0;

	static uint32_t rotate(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); }

	void compress(const unsigned char *chunk) {
		static const uint32_t k[64] = {
		    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (unsigned i = 0; i < 16; ++i) {
			w[i] = (static_cast<uint32_t>(chunk[4 * i]) << 24) | (static_cast<uint32_t>(chunk[4 * i + 1]) << 16) |
			       (static_cast<uint32_t>(chunk[4 * i + 2]) << 8) | static_cast<uint32_t>(chunk[4 * i + 3]);
		}
		for (unsigned i = 16; i < 64; ++i) {
			const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (unsigned i = 0; i < 64; ++i) {
			const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
			const uint32_t ch = (e & f) ^ (~e & g);
			const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
			const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temp2 = s0 + maj;
			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}
		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
		state[4] += e;
		state[5] += f;
		state[6] += g;
		state[7] += h;
	}

	void update(const unsigned char *data, size_t size) {
		length += size;
		while (size) {
			const size_t take = size < 64 - used ? size : 64 - used;
			std::memcpy(block + used, data, take);
			used += static_cast<unsigned>(take);
			data += take;
			size -= take;
			if (used == 64) {
				compress(block);
				used = 0;
			}
		}
	}

	void finish(unsigned char out[32]) {
		const uint64_t bits = length * 8;
		unsigned char pad = 0x80;
		update(&pad, 1);
		unsigned char zero = 0;
		while (used != 56) update(&zero, 1);
		unsigned char tail[8];
		for (unsigned i = 0; i < 8; ++i) tail[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
		update(tail, 8);
		for (unsigned i = 0; i < 8; ++i) {
			out[4 * i] = static_cast<unsigned char>(state[i] >> 24);
			out[4 * i + 1] = static_cast<unsigned char>(state[i] >> 16);
			out[4 * i + 2] = static_cast<unsigned char>(state[i] >> 8);
			out[4 * i + 3] = static_cast<unsigned char>(state[i]);
		}
	}
};

void digest(const unsigned char *data, size_t size, unsigned char out[32]) {
	Sha256 state;
	state.update(data, size);
	state.finish(out);
}

// ---------------------------------------------------------------- synthetic page
struct WindowSpec {
	const char *name;
	uint64_t offset;
	uint64_t size;
	uint32_t blockSize = 512;
	uint32_t flags = 1;
};

void putLe32(Page &page, size_t at, uint32_t value) {
	for (unsigned i = 0; i < 4; ++i) page[at + i] = static_cast<unsigned char>(value >> (8 * i));
}
void putLe64(Page &page, size_t at, uint64_t value) {
	for (unsigned i = 0; i < 8; ++i) page[at + i] = static_cast<unsigned char>(value >> (8 * i));
}

Page buildPage(const std::vector<WindowSpec> &windows, uint64_t imageBytes = kImageBytes,
               uint32_t version = 1, bool sealDigest = true) {
	Page page(sd::kRootManifestBytes, 0);
	std::memcpy(&page[0], "SHNGROOT", 8);
	putLe32(page, 8, version);
	putLe32(page, 12, static_cast<uint32_t>(windows.size()));
	putLe64(page, 16, imageBytes);
	for (size_t index = 0; index < windows.size(); ++index) {
		const size_t entry = sd::kRootHeaderBytes + sd::kRootEntryBytes * index;
		const WindowSpec &spec = windows[index];
		std::strncpy(reinterpret_cast<char *>(&page[entry]), spec.name, sd::kRootNameBytes - 1);
		putLe64(page, entry + 32, spec.offset);
		putLe64(page, entry + 40, spec.size);
		putLe32(page, entry + 48, spec.blockSize);
		putLe32(page, entry + 52, spec.flags);
		for (unsigned i = 0; i < 32; ++i) page[entry + 56 + i] = static_cast<unsigned char>(0x30 + i);
	}
	if (sealDigest) {
		digest(page.data(), sd::kRootDigestOffset, &page[sd::kRootDigestOffset]);
	}
	return page;
}

std::vector<WindowSpec> realWindows() {
	return {{"OS", kOsOffset, kOsBytes}, {"SystemOS", kSystemOsOffset, kSystemOsBytes}};
}

int parse(const Page &page, uint64_t partitionBytes, sd::RootManifest &manifest) {
	return sd::parseRootManifest(page.data(), partitionBytes, digest, manifest);
}

// ---------------------------------------------------------------- checks
void checkValidPage() {
	const Page page = buildPage(realWindows());
	sd::RootManifest manifest = {};
	CHECK(parse(page, kPartitionBytes, manifest) == sd::kRootOk);
	CHECK(manifest.count == 2);
	CHECK(manifest.imageBytes == kImageBytes);
	CHECK(std::strcmp(manifest.window[0].name, "OS") == 0);
	CHECK(manifest.window[0].offset == kOsOffset);
	CHECK(manifest.window[0].size == kOsBytes);
	CHECK(std::strcmp(manifest.window[1].name, "SystemOS") == 0);
	CHECK(manifest.window[1].offset == kSystemOsOffset);
	CHECK(manifest.window[1].size == kSystemOsBytes);
	CHECK(manifest.window[0].sha256[0] == 0x30);

	// The root volume has to be published last, and everything else in manifest order before it.
	sd::RootPlan plan = {};
	CHECK(sd::buildRootPlan(manifest, plan));
	CHECK(plan.count == 2);
	CHECK(plan.order[0] == 1);  // SystemOS first
	CHECK(plan.order[1] == 0);  // OS last

	// A third window (a cryptex the owner image carries) goes between the two, not after the OS.
	const std::vector<WindowSpec> three = {{"OS", kOsOffset, kOsBytes},
	                                       {"Cryptex1", 0x22f400000ull, 0x10000000ull},
	                                       {"SystemOS", kSystemOsOffset, kSystemOsBytes}};
	const Page page3 = buildPage(three, 0x23f400000ull);
	sd::RootManifest manifest3 = {};
	CHECK(parse(page3, kPartitionBytes, manifest3) == sd::kRootOk);
	sd::RootPlan plan3 = {};
	CHECK(sd::buildRootPlan(manifest3, plan3));
	CHECK(plan3.count == 3);
	CHECK(plan3.order[0] == 2);  // SystemOS
	CHECK(plan3.order[1] == 1);  // Cryptex1
	CHECK(plan3.order[2] == 0);  // OS
}

void checkBadPages() {
	sd::RootManifest manifest = {};

	Page noMagic = buildPage(realWindows());
	noMagic[0] = 'X';
	digest(noMagic.data(), sd::kRootDigestOffset, &noMagic[sd::kRootDigestOffset]);
	CHECK(parse(noMagic, kPartitionBytes, manifest) == sd::kRootNoMagic);

	CHECK(parse(buildPage(realWindows(), kImageBytes, 2), kPartitionBytes, manifest) == sd::kRootBadVersion);
	CHECK(parse(buildPage({}), kPartitionBytes, manifest) == sd::kRootBadVersion);

	std::vector<WindowSpec> five = realWindows();
	five.push_back({"A", 0x1e0000000ull, 0x1000});
	five.push_back({"B", 0x1e0010000ull, 0x1000});
	five.push_back({"C", 0x1e0020000ull, 0x1000});
	CHECK(parse(buildPage(five), kPartitionBytes, manifest) == sd::kRootBadVersion);

	Page badDigest = buildPage(realWindows());
	badDigest[sd::kRootDigestOffset] ^= 0xff;
	CHECK(parse(badDigest, kPartitionBytes, manifest) == sd::kRootBadDigest);

	// Bounds: the image must fit the partition, windows must sit after the manifest block, be 1 MiB
	// aligned, and end (with the sector padding a 4 KiB reader fetches) inside the image.
	CHECK(parse(buildPage(realWindows(), kPartitionBytes + 1), kPartitionBytes, manifest) == sd::kRootBadBounds);
	CHECK(parse(buildPage({{"OS", 0x800, 0x1000}}), kPartitionBytes, manifest) == sd::kRootBadBounds);
	CHECK(parse(buildPage({{"OS", 0x100800, 0x1000}}), kPartitionBytes, manifest) == sd::kRootBadBounds);
	CHECK(parse(buildPage({{"OS", kOsOffset, 0x1001}}), kPartitionBytes, manifest) == sd::kRootBadBounds);
	CHECK(parse(buildPage({{"OS", kOsOffset, kImageBytes}}), kPartitionBytes, manifest) == sd::kRootBadBounds);
	CHECK(parse(buildPage({{"OS", kImageBytes, 0x1000}}), kPartitionBytes, manifest) == sd::kRootBadBounds);
	// The boundary the other way: a window that ends exactly at the last byte of the image is legal, and
	// the 4 KiB padding rule must not turn it into a failure. (With both the image size and every offset
	// 4 KiB aligned, the padding a 4 KiB reader fetches past the window always lands inside the image.)
	sd::RootManifest boundary = {};
	CHECK(parse(buildPage({{"OS", kOsOffset, kImageBytes - kOsOffset}}), kPartitionBytes, boundary) == sd::kRootOk);
	CHECK(boundary.window[0].size == kImageBytes - kOsOffset);

	// Names: empty, unterminated, and repeated.
	CHECK(parse(buildPage({{"", kOsOffset, kOsBytes}}), kPartitionBytes, manifest) == sd::kRootBadName);
	Page longName = buildPage({{"OS", kOsOffset, kOsBytes}});
	for (unsigned i = 0; i < sd::kRootNameBytes; ++i) longName[sd::kRootHeaderBytes + i] = 'A';
	digest(longName.data(), sd::kRootDigestOffset, &longName[sd::kRootDigestOffset]);
	CHECK(parse(longName, kPartitionBytes, manifest) == sd::kRootBadName);
	CHECK(parse(buildPage({{"OS", kOsOffset, kOsBytes}, {"OS", kSystemOsOffset, kSystemOsBytes}}), kPartitionBytes,
	            manifest) == sd::kRootBadName);

	// Attributes: the kernel publishes these read-only in 512-byte sectors, and nothing else.
	CHECK(parse(buildPage({{"OS", kOsOffset, kOsBytes, 4096, 1}}), kPartitionBytes, manifest) == sd::kRootBadAttributes);
	CHECK(parse(buildPage({{"OS", kOsOffset, kOsBytes, 512, 0}}), kPartitionBytes, manifest) == sd::kRootBadAttributes);

	// Overlap: SystemOS starting inside OS is the shape a hand-edited manifest takes.
	CHECK(parse(buildPage({{"OS", kOsOffset, kOsBytes}, {"SystemOS", kOsOffset + 0x100000, kSystemOsBytes}}),
	            kPartitionBytes, manifest) == sd::kRootOverlap);
}

void checkPlans() {
	// No OS window: there is nothing to mount, so no plan exists even though the page parses.
	const Page page = buildPage({{"SystemOS", kSystemOsOffset, kSystemOsBytes}});
	sd::RootManifest manifest = {};
	CHECK(parse(page, kPartitionBytes, manifest) == sd::kRootOk);
	sd::RootPlan plan = {};
	CHECK(!sd::buildRootPlan(manifest, plan));

	sd::RootManifest empty = {};
	CHECK(!sd::buildRootPlan(empty, plan));
}

// The real artefact, when the caller names one: the page a build actually produced has to be accepted by
// the very parser that will run in the kernel, and its windows have to be inside the partition they came
// from. That is a different question from "does the parser follow the format".
void checkRealImage(const char *path) {
	std::FILE *file = std::fopen(path, "rb");
	if (!file) {
		std::printf("FAIL: cannot open %s\n", path);
		++failures;
		return;
	}
	Page page(sd::kRootManifestBytes, 0);
	const size_t got = std::fread(page.data(), 1, page.size(), file);
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fclose(file);
	CHECK(got == page.size());

	sd::RootManifest manifest = {};
	const int status = parse(page, static_cast<uint64_t>(size), manifest);
	if (status != sd::kRootOk) {
		std::printf("FAIL %s: manifest status %d (%s)\n", path, status, sd::rootStatusName(status));
		++failures;
		return;
	}
	CHECK(manifest.imageBytes == static_cast<uint64_t>(size));
	CHECK(manifest.count >= 2);
	bool hasOs = false, hasSystemOs = false;
	for (uint32_t index = 0; index < manifest.count; ++index) {
		hasOs = hasOs || std::strcmp(manifest.window[index].name, "OS") == 0;
		hasSystemOs = hasSystemOs || std::strcmp(manifest.window[index].name, "SystemOS") == 0;
	}
	CHECK(hasOs);
	CHECK(hasSystemOs);
	sd::RootPlan plan = {};
	CHECK(sd::buildRootPlan(manifest, plan));
	CHECK(plan.count == manifest.count);
	CHECK(manifest.window[plan.order[plan.count - 1]].name[0] == 'O');
	std::printf("real image: %s, %u windows, %llu bytes\n", path, manifest.count,
	            static_cast<unsigned long long>(manifest.imageBytes));
}

}  // namespace

int main(int argc, char **argv) {
	checkValidPage();
	checkBadPages();
	checkPlans();
	if (argc > 1) {
		checkRealImage(argv[1]);
	}
	if (failures) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("all root-manifest checks passed\n");
	return 0;
}
