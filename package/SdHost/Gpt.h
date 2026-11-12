// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Gpt.h -- read and validate a GUID partition table through a block-read callback.
// Pure functions, no kernel dependency: a host test can run this header against a synthetic disk.
//
// Adapted from UniXNU-evb10 package/Rk3588Emmc/Gpt.h (same table parsing, same CRC rules) for the
// SD port. Two differences: the namespace is `sd`, and this file carries its own CRC-32 so that it
// does not pull in the whole host core the way EmmcHost.h did.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sd {

constexpr uint32_t kGptMaxEntries = 128;
constexpr uint32_t kGptEntryBytes = 128;
constexpr size_t kGptScratchBytes = static_cast<size_t>(kGptMaxEntries) * kGptEntryBytes;

struct GptEntry {
	uint32_t slot;  // index in the on-disk array
	uint8_t typeGuid[16];
	uint8_t uniqueGuid[16];
	uint64_t first;
	uint64_t last;
	uint64_t attributes;
	char name[37];  // ASCII; anything outside 0x20..0x7e becomes '?'
};

struct GptInfo {
	uint8_t diskGuid[16];
	uint64_t myLba, altLba, firstUsable, lastUsable, entriesLba;
	uint32_t count, entrySize;
	bool headerCrcOk, entriesCrcOk;
	uint32_t used;  // entries returned
};

enum GptStatus : int {
	kGptOk = 0,
	kGptReadFailed = -1,
	kGptNoSignature = -2,
	kGptUnsupported = -3,
};

// The CRC-32 of IEEE 802.3 (reflected, polynomial 0xedb88320), which is what a GPT header and its
// entry array carry. Seeded with 0 so that several buffers can be chained.
inline uint32_t crc32(const void *data, size_t length, uint32_t seed = 0) {
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	uint32_t crc = ~seed;
	for (size_t i = 0; i < length; ++i) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

inline uint32_t le32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t *p) {
	return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (the first three fields are stored little endian).
inline void formatGuid(const uint8_t *guid, char *out /* 37 bytes */) {
	static const char digits[] = "0123456789abcdef";
	static const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	unsigned at = 0;
	for (unsigned i = 0; i < 16; ++i) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-';
		out[at++] = digits[guid[order[i]] >> 4];
		out[at++] = digits[guid[order[i]] & 15];
	}
	out[at] = 0;
}

inline bool allZero(const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		if (p[i]) return false;
	}
	return true;
}

inline bool sameName(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// `read(lba, count, out)` returns true on success. `scratch` must hold kGptScratchBytes.
// Entries that pass the header CRC but fail the entry-array CRC are still returned
// (info.entriesCrcOk says so): a damaged array is better described than hidden.
template <class Reader>
int readGpt(Reader &&read, uint32_t blockBytes, uint8_t *scratch, GptInfo &info, GptEntry *entries,
            unsigned maxEntries) {
	alignas(8) uint8_t header[512];
	info = GptInfo{};
	if (blockBytes < 512 || blockBytes > 4096) return kGptUnsupported;
	if (!read(1, 1, header)) return kGptReadFailed;
	if (__builtin_memcmp(header, "EFI PART", 8) != 0) return kGptNoSignature;
	uint32_t headerSize = le32(header + 12);
	uint32_t headerCrc = le32(header + 16);
	info.myLba = le64(header + 24);
	info.altLba = le64(header + 32);
	info.firstUsable = le64(header + 40);
	info.lastUsable = le64(header + 48);
	__builtin_memcpy(info.diskGuid, header + 56, 16);
	info.entriesLba = le64(header + 72);
	info.count = le32(header + 80);
	info.entrySize = le32(header + 84);
	uint32_t entriesCrc = le32(header + 88);
	if (headerSize >= 92 && headerSize <= blockBytes) {
		alignas(8) uint8_t copy[512];
		__builtin_memcpy(copy, header, sizeof(copy));
		__builtin_memset(copy + 16, 0, 4);
		info.headerCrcOk = crc32(copy, headerSize) == headerCrc;
	}
	if (info.entrySize != kGptEntryBytes || info.count == 0 || info.count > kGptMaxEntries) {
		return kGptUnsupported;
	}

	uint32_t bytes = info.count * info.entrySize;
	uint32_t blocks = (bytes + blockBytes - 1) / blockBytes;
	if (!read(info.entriesLba, blocks, scratch)) return kGptReadFailed;
	info.entriesCrcOk = crc32(scratch, bytes) == entriesCrc;
	for (uint32_t slot = 0; slot < info.count && info.used < maxEntries; ++slot) {
		const uint8_t *raw = scratch + static_cast<size_t>(slot) * info.entrySize;
		if (allZero(raw, 16)) continue;
		GptEntry &entry = entries[info.used++];
		entry.slot = slot;
		__builtin_memcpy(entry.typeGuid, raw, 16);
		__builtin_memcpy(entry.uniqueGuid, raw + 16, 16);
		entry.first = le64(raw + 32);
		entry.last = le64(raw + 40);
		entry.attributes = le64(raw + 48);
		unsigned length = 0;
		for (; length < 36; ++length) {
			uint32_t unit = raw[56 + 2 * length] | (raw[57 + 2 * length] << 8);
			if (unit == 0) break;
			entry.name[length] = (unit >= 0x20 && unit < 0x7f) ? static_cast<char>(unit) : '?';
		}
		entry.name[length] = 0;
	}
	return kGptOk;
}

}  // namespace sd
