// Host test for package/SdHost/Gpt.h: build a synthetic disk from the board's real GPT numbers and
// check that the parser reproduces them, then corrupt one field at a time and check it notices.
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -I package/SdHost tests/test-gpt.cpp -o /tmp/test-gpt
//
// The numbers are the ones the board reported (`tools/board-probe*.sh`): a single partition named
// "primary" at LBA 32768..62929886, 512-byte sectors, alternate LBA one past the end of the card.
#include <cstdio>
#include <cstring>
#include <vector>

#include "Gpt.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
	do {                                                                        \
		if (!(condition)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
			++failures;                                                         \
		}                                                                       \
	} while (0)

using Disk = std::vector<unsigned char>;

constexpr uint32_t kBlockBytes = 512;
constexpr uint64_t kDiskBlocks = 62929920;   // alt_lba 62929919 + 1
constexpr uint64_t kPartFirst = 32768;
constexpr uint64_t kPartLast = 62929886;

void putLe32(Disk &disk, size_t at, uint32_t value) {
	for (int i = 0; i < 4; ++i) disk[at + i] = static_cast<unsigned char>(value >> (8 * i));
}
void putLe64(Disk &disk, size_t at, uint64_t value) {
	for (int i = 0; i < 8; ++i) disk[at + i] = static_cast<unsigned char>(value >> (8 * i));
}
void putName(Disk &disk, size_t at, const char *name) {
	for (size_t i = 0; i < 36; ++i) {
		const unsigned char unit = static_cast<unsigned char>(i < std::strlen(name) ? name[i] : 0);
		disk[at + 2 * i] = unit;
		disk[at + 2 * i + 1] = 0;
	}
}

// A disk whose LBA 1 holds a header and whose LBA 2 holds one entry. CRC fields are computed the
// same way the real thing is, so a correct parser must accept it.
//
// The entry array is 128 entries of 128 bytes = 16 KiB = 32 blocks, so the synthetic disk has to be
// large enough for a full read of it: a parser that reads the whole array in one call is the one we
// want, and a too-small fixture would fail it for the fixture's sake rather than for its own.
constexpr uint32_t kEntryBlocks = (128 * 128) / kBlockBytes;
constexpr uint64_t kEntriesLba = 2;
constexpr uint64_t kDiskFixtureBlocks = kEntriesLba + kEntryBlocks + 1;

Disk buildDisk() {
	Disk disk(static_cast<size_t>(kBlockBytes) * kDiskFixtureBlocks, 0);
	const size_t header = kBlockBytes * 1;
	const size_t entries = kBlockBytes * kEntriesLba;

	std::memcpy(&disk[header], "EFI PART", 8);
	putLe32(disk, header + 8, 0x00010000);          // revision 1.0
	putLe32(disk, header + 12, 92);                 // header size
	putLe64(disk, header + 24, 1);                  // my_lba
	putLe64(disk, header + 32, kDiskBlocks - 1);    // alt_lba
	putLe64(disk, header + 40, 34);                 // first_usable
	putLe64(disk, header + 48, kDiskBlocks - 34);   // last_usable
	for (int i = 0; i < 16; ++i) disk[header + 56 + i] = static_cast<unsigned char>(0x10 + i);
	putLe64(disk, header + 72, kEntriesLba);        // entries_lba
	putLe32(disk, header + 80, 128);                // count
	putLe32(disk, header + 84, 128);                // entry size

	// Entry 0: the Linux root partition. Entry 1 is left all zero, which the parser must skip.
	for (int i = 0; i < 16; ++i) disk[entries + i] = static_cast<unsigned char>(0xa0 + i);
	for (int i = 0; i < 16; ++i) disk[entries + 16 + i] = static_cast<unsigned char>(0x40 + i);
	putLe64(disk, entries + 32, kPartFirst);
	putLe64(disk, entries + 40, kPartLast);
	putName(disk, entries + 56, "primary");

	putLe32(disk, header + 88, sd::crc32(&disk[entries], 128 * 128));
	for (int i = 16; i < 20; ++i) disk[header + i] = 0;
	putLe32(disk, header + 16, sd::crc32(&disk[header], 92));
	return disk;
}

struct Reader {
	const Disk &disk;
	bool fail = false;
	size_t calls = 0;
	bool operator()(uint64_t lba, uint32_t count, void *out) {
		++calls;
		if (fail) return false;
		const size_t at = static_cast<size_t>(lba) * kBlockBytes;
		const size_t bytes = static_cast<size_t>(count) * kBlockBytes;
		if (at + bytes > disk.size()) return false;
		std::memcpy(out, &disk[at], bytes);
		return true;
	}
};

std::vector<unsigned char> scratch(sd::kGptScratchBytes);
sd::GptEntry entries[sd::kGptMaxEntries];

void parse(const Disk &disk, sd::GptInfo &info, int &status, unsigned &used) {
	Reader reader{disk};
	status = sd::readGpt(reader, kBlockBytes, scratch.data(), info, entries, sd::kGptMaxEntries);
	used = info.used;
}

void testRealGeometry() {
	Disk disk = buildDisk();
	sd::GptInfo info{};
	int status = 0;
	unsigned used = 0;
	parse(disk, info, status, used);
	CHECK(status == sd::kGptOk);
	CHECK(info.headerCrcOk);
	CHECK(info.entriesCrcOk);
	CHECK(info.myLba == 1);
	CHECK(info.altLba == kDiskBlocks - 1);
	CHECK(info.firstUsable == 34);
	CHECK(info.count == 128);
	CHECK(info.entrySize == 128);
	CHECK(used == 1);                       // the all-zero entry is skipped
	CHECK(entries[0].first == kPartFirst);
	CHECK(entries[0].last == kPartLast);
	CHECK(std::strcmp(entries[0].name, "primary") == 0);
	CHECK(entries[0].slot == 0);
}

void testNoSignature() {
	Disk disk = buildDisk();
	std::memset(&disk[kBlockBytes], 0, 8);
	sd::GptInfo info{};
	int status = 0;
	unsigned used = 0;
	parse(disk, info, status, used);
	CHECK(status == sd::kGptNoSignature);
}

void testReadFailure() {
	Disk disk = buildDisk();
	Reader reader{disk};
	reader.fail = true;
	sd::GptInfo info{};
	const int status = sd::readGpt(reader, kBlockBytes, scratch.data(), info, entries,
	                               sd::kGptMaxEntries);
	CHECK(status == sd::kGptReadFailed);
}

void testHeaderCrcNoticesDamage() {
	Disk disk = buildDisk();
	disk[kBlockBytes + 40] ^= 0xff;   // first_usable changes, header CRC does not follow
	sd::GptInfo info{};
	int status = 0;
	unsigned used = 0;
	parse(disk, info, status, used);
	// The header CRC is reported, not fatal: a disk whose entries are intact is still readable, and
	// hiding it behind a parse failure would make a repairable card look empty.
	CHECK(status == sd::kGptOk);
	CHECK(!info.headerCrcOk);
}

void testEntryCrcNoticesDamage() {
	Disk disk = buildDisk();
	disk[2 * kBlockBytes + 32] ^= 0xff;   // first LBA of the entry changes
	sd::GptInfo info{};
	int status = 0;
	unsigned used = 0;
	parse(disk, info, status, used);
	CHECK(status == sd::kGptOk);
	CHECK(info.headerCrcOk);
	CHECK(!info.entriesCrcOk);
}

void testUnsupportedEntrySize() {
	Disk disk = buildDisk();
	putLe32(disk, kBlockBytes + 84, 256);
	sd::GptInfo info{};
	int status = 0;
	unsigned used = 0;
	parse(disk, info, status, used);
	CHECK(status == sd::kGptUnsupported);
}

void testBadBlockSize() {
	Disk disk = buildDisk();
	Reader reader{disk};
	sd::GptInfo info{};
	const int status =
	    sd::readGpt(reader, 256, scratch.data(), info, entries, sd::kGptMaxEntries);
	CHECK(status == sd::kGptUnsupported);
	CHECK(reader.calls == 0);   // rejected before touching the disk
}

void testCrcIsTheOneTheTableUses() {
	// CRC-32/ISO-HDLC of "123456789" is 0xcbf43926; if this drifts, every GPT in the world stops
	// parsing and the failure looks like a card problem.
	const char *text = "123456789";
	CHECK(sd::crc32(text, 9) == 0xcbf43926u);
}

void testGuidFormatting() {
	unsigned char guid[16];
	for (int i = 0; i < 16; ++i) guid[i] = static_cast<unsigned char>(i);
	char out[37];
	sd::formatGuid(guid, out);
	// The first three fields are little endian on disk, the last two big endian.
	CHECK(std::strcmp(out, "03020100-0504-0706-0809-0a0b0c0d0e0f") == 0);
}

void testSameName() {
	CHECK(sd::sameName("primary", "primary"));
	CHECK(!sd::sameName("primary", "primar"));
	CHECK(!sd::sameName("", "primary"));
	CHECK(sd::sameName("", ""));
}

}  // namespace

int main() {
	testCrcIsTheOneTheTableUses();
	testGuidFormatting();
	testSameName();
	testRealGeometry();
	testNoSignature();
	testReadFailure();
	testHeaderCrcNoticesDamage();
	testEntryCrcNoticesDamage();
	testUnsupportedEntrySize();
	testBadBlockSize();

	if (failures) {
		std::printf("FAILED: %d check(s)\n", failures);
		return 1;
	}
	std::printf("PASS: all Gpt.h tests passed\n");
	return 0;
}
