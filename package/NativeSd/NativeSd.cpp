// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSd: the SD host of the Orange Pi 5 Plus as an IOKit service, and the block devices it
// publishes.
//
// NativeSd attaches to the /arm-io node that dts/opi5plus.dts publishes for the DesignWare MSHC at
// 0xFE2C0000 (compatible "rockchip,rk3588-dw-mshc"). It keeps the bus state the firmware left --
// the firmware's own DwMmcHcDxe + RkSdmmcDxe have already identified the card, chosen the bus width
// and set the clock, and this driver never touches any of that -- and reads the card through the
// core in SdHost.h.
//
// NativeSdBlock is a window of the card (the whole disk, or one partition) as a subclass of the
// original IOBlockStorageDevice: 512-byte sectors, read-only, synchronous. The original
// IOBlockStorageDriver, the partition schemes and APFS sit on top unchanged. How the subclass is
// bound to the original class is explained in scripts/bind-sd-storage.py of this repository; it is
// the same mechanism UniXNU-evb10 uses for its eMMC driver, and it works here because both drivers
// are built against the same original kernelcache.
//
// WHY READ-ONLY. On this board the SD card is not spare capacity: it carries the Linux root
// filesystem this port is developed from, and the only recovery path when the SPI flash holds a
// firmware that does not boot (the card is what the BootROM falls back to). A write path in the
// kernel would be a way to destroy both. The core in SdHost.h has no write command at all, and
// nothing here calls one.
//
// Boot-args:  sd=0           do not attach
//             sd=1           probe only (default): dump the controller, read the GPT, check that
//                            reads are stable, publish nothing
//             sd=2           also publish the whole disk as one read-only block device
//             sd=3           also publish the partition named by sd-partition as one device
//             sd=4           also publish the root windows of the partition named by sd-root-partition:
//                            the SHNGROOT manifest `unixnu build userdata` writes, then one block device
//                            per window (SystemOS first, OS last -- see RootManifest.h buildRootPlan)
//             sd-partition=NAME  partition to publish at sd=3 (default "primary")
//             sd-root-partition=NAME  partition holding the root image at sd=4 (default "userdata")
//             sd-root-trust=1  let bsd_init accept the mounted root volume without Apple's root
//                            authentication, if it is a volume inside a window published here. The OS volume
//                            of an IPSW is never sealed, so this is the only way past
//                            "rootvp not authenticated after mounting" (see native_emmc_root_trusted).
//             sd-boot-manifest=0  do not publish /chosen/boot-manifest-hash (default 1, see
//                            publishBootManifestHash): the original `mount -P 1` exits 66 without it
//             sd-data-ram=1  publish the payload's Data seed (/chosen/memory-map/RAMDisk, written by the
//                            loader from --data-ramdisk) as a WRITABLE block device of its own, so that
//                            APFS can pair its volume with the System volume and launchd gets a
//                            /private/var (default 0: only a payload that carries a seed should see it)
//             sd-reset=1     reset the controller before probing (default 0). Kept off because the
//                            firmware hands over a live, powered card and CTRL_RESET_ALL clears PWREN,
//                            which made every probe command go unanswered on 2026-11-02. See
//                            evidence/m4/M4-05-sd-board-test.md.
//             sd-probe=N     blocks of the chosen partition to read twice, one block at a time and
//                            then as one run, and compare (default 256, 0 = skip). The comparison is
//                            what proves multi-block transfers work on this card; there is no boot
//                            argument that turns the run off, because CMD18 is what
//                            IOBlockStorageDevice hands us anyway.
//
// What this version deliberately does not do, and why:
//   * No write path to the card. See above.
//   * No persistent Data volume. What it does publish is the payload's Data seed as a writable RAM block
//     device (sd-data-ram=1, publishDataRamDisk): launchd needs a writable /private/var for its shared
//     region and for swap, and APFS pairs the seed's volume with the System volume. The seed is lost on
//     reset -- the persistent half of M4 is the next step, not this one.
//   * No watchdog feeding and no ApplePMGRNub. The eMMC driver needs both to survive a full boot; a
//     probe run does not reach the point where either matters. They are the first thing to add when
//     this publishes a root volume.
//   * The capacity is taken from the GPT rather than from the CSD (Host::adoptCapacity) because the
//     CSD reading on this board does not vouch for itself. The CSD decode stays as the first opinion and
//     the disagreement is logged; the GPT is what cuts the windows, so it is what addressing follows.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>

#include "Gpt.h"
#include "RootManifest.h"
#include "SdHost.h"

// The Data seed the loader left in boot memory is reached through its physical address, the same way the
// original RamBlock driver does it. gPhysBase/gPhysSize bound what the loader may hand over.
extern "C" uintptr_t ml_static_ptovirt(uintptr_t paddr);
extern "C" unsigned long gPhysBase, gPhysSize;

// The console's own non-blocking UART reader (pexpert/arm/pe_kprintf.c serial_getc -> pe_serial.c uart_getc):
// it returns a negative value when the FIFO is empty and carries none of the NMI state machine that
// _serial_getc() drives, so a byte count other than the console's cannot send the boot into Debugger().
// It is not exported and needs no export: -Wl,-force_load links this archive into the kernel whole, which is
// the same reason ml_io_map below resolves.
extern "C" int serial_getc(void);

// The sizes of the original classes (scripts/data/opi5plus-storage-interface-audit.json). The public
// headers this file compiles against must describe the same objects as the original kernelcache, or
// a subclass would overlay its members on the base class's.
static_assert(sizeof(IOService) == 0x88, "IOService layout differs from the original kernelcache");
static_assert(sizeof(IOBlockStorageDevice) == 0x90,
              "IOBlockStorageDevice layout differs from the original kernelcache");

extern "C" {
volatile uint32_t native_sd_ready = 0;            // 1 once the card answered and the GPT parsed
volatile uint64_t native_sd_part_first = 0;       // first block of the window the board is run against
volatile uint64_t native_sd_part_last = 0;
volatile uint32_t native_sd_blocks_published = 0;
volatile uint64_t native_sd_block_requests = 0;
volatile uint64_t native_sd_block_failures = 0;
volatile uint64_t native_sd_blocks_read = 0;
volatile uint64_t native_sd_read_calls = 0;       // host reads, one per bounce-buffer chunk
volatile uint64_t native_sd_read_blocks = 0;      // blocks the card delivered
volatile uint64_t native_sd_commands = 0;         // commands the core issued to the card
volatile uint64_t native_sd_timeouts = 0;
volatile uint32_t native_sd_multi_block = 0;      // 1 once runs of blocks were proven to work (CMD18)
volatile uint32_t native_sd_geo_blocks = 0;       // geometry().blockCount, for the serial log
volatile uint32_t native_sd_rca = 0;
volatile uint32_t native_sd_capacity_from_gpt = 0;  // 1 when the GPT, not the CSD, set the bound
volatile uint32_t native_sd_root_status = 0xffffffffu;  // sd::RootStatus of the root image, once looked at
volatile uint32_t native_sd_root_windows = 0;          // windows published from the manifest
volatile uint64_t native_sd_root_image_bytes = 0;
volatile uint32_t native_sd_root_trust = 0;            // boot-arg sd-root-trust=1: owner vouches for the root
volatile uint32_t native_sd_data_ready = 0;            // 1 once the Data RAM disk was published
volatile uint64_t native_sd_ram_writes = 0;            // writes answered by that RAM disk
volatile uint64_t native_sd_blocks_written = 0;   // blocks written to the card (persistent Data)
volatile uint64_t native_sd_write_calls = 0;      // host write calls
// What the PIO path costs, for a periodic line to print. Both are counted where the card is already driven, so
// neither adds an access to the medium: the bursts are the counter sd::Stats::pioBursts the core keeps anyway
// (SdHost.h:245), refreshed on every read call, and the microseconds are two counter reads around a transfer
// that takes milliseconds. Counting only -- nothing here prints, sleeps or changes what the FIFO loop does.
volatile uint64_t native_sd_pio_bursts = 0;       // sd::Stats::pioBursts as of the last read call
volatile uint64_t native_sd_chunk_us = 0;         // microseconds spent inside successful chunk reads
volatile uint64_t native_sd_chunks = 0;           // successful chunk reads timed
// Round 9 diagnostics. The clock registers are read back once, right after a clock update lands, because
// the CLKDIV in the boot log is the firmware's dump from before the driver took over and cannot answer
// "which card clock actually ran". The two phase counters split the PIO read path: filling the FIFO
// against the card clock, then draining it word by word against the CPU. Counting only -- nothing here
// prints, sleeps or changes what the loops do.
volatile uint32_t native_sd_clkdiv = 0;   // CLKDIV (0x008) as the driver last left it
volatile uint32_t native_sd_clksrc = 0;   // CLKSRC (0x00c)
volatile uint32_t native_sd_clkena = 0;   // CLKENA (0x010)
volatile uint64_t native_sd_fill_us = 0;  // phase-1 microseconds: waiting for the FIFO to fill
volatile uint64_t native_sd_drain_us = 0; // phase-2 microseconds: draining the batch from the FIFO
}
namespace {

constexpr uint32_t kChunkBlocks = 128;
constexpr uint32_t kChunkBytes = kChunkBlocks * sd::kBlockBytes;
constexpr unsigned kRequestLogLines = 48;
static_assert(kChunkBlocks <= 0xffffu, "the core refuses a run longer than BYTCNT can express");

// A trace of the card requests that goes out the UART itself, not through the BSD console: the boot stops
// dead during the dyld shared cache reads, and the console prints stop with it. sd-trace-io=1 turns it on.
extern "C" void (*PE_kputc)(char);

bool traceIoEnabled() {
	static int cached = -1;
	if (cached < 0) {
		uint32_t value = 0;
		PE_parse_boot_argn("sd-trace-io", &value, sizeof(value));
		cached = value != 0 ? 1 : 0;
	}
	return cached == 1;
}

void pePut(const char *text) {
	if (!PE_kputc) return;
	while (*text) PE_kputc(*text++);
}

void peNum(uint64_t value) {
	if (!PE_kputc) return;
	if (value == 0) {
		PE_kputc('0');
		return;
	}
	char digits[24];
	int count = 0;
	while (value != 0 && count < 24) {
		digits[count++] = static_cast<char>('0' + (value % 10));
		value /= 10;
	}
	while (count > 0) PE_kputc(digits[--count]);
}

// The RK3588 watchdog (DesignWare), the same one EVB10 feeds with wdt-kick, is armed from the kernel's
// first-exec hook and kicked by the heartbeat thread (see the definitions after this namespace).



uint32_t mmioRead(void *base, uint32_t offset, unsigned width) {
	switch (width) {
	case 8:
		return *reinterpret_cast<volatile uint8_t *>(static_cast<uint8_t *>(base) + offset);
	case 16:
		return *reinterpret_cast<volatile uint16_t *>(static_cast<uint8_t *>(base) + offset);
	default:
		return *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
	}
}

void mmioWrite(void *base, uint32_t offset, unsigned width, uint32_t value) {
	switch (width) {
	case 8:
		*reinterpret_cast<volatile uint8_t *>(static_cast<uint8_t *>(base) + offset) =
		    static_cast<uint8_t>(value);
		break;
	case 16:
		*reinterpret_cast<volatile uint16_t *>(static_cast<uint8_t *>(base) + offset) =
		    static_cast<uint16_t>(value);
		break;
	default:
		*reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset) = value;
		break;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

uint64_t nowUs() {
	uint64_t nanoseconds = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

// IODelay() sleeps in the timer wheel when the platform does not report a CPU idle latency, and the
// SD core polls with a microsecond granularity: a millisecond sleep per poll would turn a 20 ms
// transfer into minutes and flood the timer interrupt. Spin for the short waits, sleep for the long.
void delayUs(uint32_t microseconds) {
	if (microseconds >= 1000) {
		IOSleep((microseconds + 999) / 1000);
		return;
	}
	const uint64_t deadline = nowUs() + microseconds;
	while (nowUs() < deadline) {
		__asm__ volatile("yield" ::: "memory");
	}
}

const sd::Platform kPlatform = {mmioRead, mmioWrite, nowUs, delayUs, IOLog};

uint32_t bootArg(const char *name, uint32_t fallback) {
	uint32_t value = fallback;
	if (PE_parse_boot_argn(name, &value, sizeof(value))) return value;
	return fallback;
}

// Boot args are parsed as integers by PE_parse_boot_argn; a string argument needs PE_getenvvar-style
// handling, which the original kernel does not export. Reading the string form out of the raw
// command line keeps sd-partition=NAME working without another dependency.
bool bootArgString(const char *name, char *out, size_t size) {
	if (!out || size < 2) return false;
	const char *commandLine = PE_boot_args();
	if (!commandLine) return false;
	const size_t nameLength = __builtin_strlen(name);
	for (const char *scan = commandLine; *scan; ++scan) {
		if ((scan == commandLine || scan[-1] == ' ') && __builtin_strncmp(scan, name, nameLength) == 0 &&
		    scan[nameLength] == '=') {
			const char *value = scan + nameLength + 1;
			size_t length = 0;
			while (value[length] && value[length] != ' ' && length + 1 < size) {
				out[length] = value[length];
				++length;
			}
			out[length] = 0;
			return length != 0;
		}
	}
	return false;
}

// The SHA-256 the root-manifest parser needs. parseRootManifest() takes it as a functor so that the host
// test can run the same header without a kernel, and so that this file does not have to reimplement the
// digest rules: XNU's sha2.h already has them.
void sha256(const uint8_t *data, size_t length, uint8_t out[32]) {
	SHA256_CTX context;
	SHA256_Init(&context);
	SHA256_Update(&context, data, length);
	SHA256_Final(out, &context);
}

void hexLine(const char *label, uint64_t lba, const uint8_t *bytes, unsigned count) {
	char line[3 * 16 + 1];
	unsigned used = 0;
	for (unsigned i = 0; i < count && i < 16; ++i) {
		static const char digits[] = "0123456789abcdef";
		line[used++] = digits[bytes[i] >> 4];
		line[used++] = digits[bytes[i] & 15];
		line[used++] = ' ';
	}
	line[used ? used - 1 : 0] = 0;
	IOLog("NativeSd: %s block %llu: %s\n", label, static_cast<unsigned long long>(lba), line);
}

unsigned long long ull(uint64_t value) { return static_cast<unsigned long long>(value); }

}  // namespace

// The RK3588 watchdog (DesignWare), the same one EVB10 feeds with wdt-kick. Armed here instead of only fed:
// the kernel deadlocks during the shared-cache reads, and a deadlocked kernel cannot panic itself back to the
// card, so nothing resets the board and the next payload cannot be pushed. With this armed and kicked from the
// heartbeat thread, a deadlock becomes a reset about 89 s later and an unattended run becomes possible.
namespace {
constexpr uint64_t kWdtPhys = 0xFEAF0000ull;
constexpr uint32_t kWdtCr = 0x00;
constexpr uint32_t kWdtTorr = 0x04;
constexpr uint32_t kWdtCrr = 0x0C;
constexpr uint32_t kWdtRestart = 0x76;

volatile uint32_t *gWatchdogRegs = nullptr;
}  // namespace

// The arm64 kernel's device-memory mapper; the driver headers of this kit do not carry it (EVB10's do).
extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);

// Called once from the kernel's first-exec hook (sd-watchdog=N selects the timeout index, 15 is about 89 s at
// the 24 MHz reference), then kicked by the heartbeat thread.
extern "C" int
native_sd_watchdog_start(uint32_t timeoutIndex)
{
	vm_offset_t base = ml_io_map(static_cast<vm_offset_t>(kWdtPhys), 0x1000);
	if (!base) {
		return 0;
	}
	volatile uint32_t *regs = reinterpret_cast<volatile uint32_t *>(base);
	if (timeoutIndex > 15) timeoutIndex = 15;
	regs[kWdtTorr / 4] = timeoutIndex;
	regs[kWdtCrr / 4] = kWdtRestart;
	// Bit 0 enable, bit 1 reset mode (1 would be an interrupt, which nothing here would take).
	regs[kWdtCr / 4] = 0x3;
	regs[kWdtCrr / 4] = kWdtRestart;
	__asm__ volatile("dsb sy" ::: "memory");
	gWatchdogRegs = regs;
	return 1;
}

extern "C" void
native_sd_watchdog_kick(void)
{
	if (gWatchdogRegs) {
		gWatchdogRegs[kWdtCrr / 4] = kWdtRestart;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

class NativeSdBlock;

class NativeSd : public IOService {
	OSDeclareDefaultStructors(NativeSd)

	IOMemoryMap *mapping = nullptr;
	IOLock *gate = nullptr;
	sd::Host host;
	uint64_t diskBlocks = 0;
	uint64_t partFirst = 0, partLast = 0;
	bool partFound = false;
	uint64_t rootFirst = 0, rootLast = 0;  // partition that carries the SHNGROOT image
	bool rootFound = false;

	bool scanGpt();
	void probePartition();
	bool publishRootWindows();
	bool publishDataRamDisk();
	bool persistentData();
	bool formatDataWindow(uint64_t windowFirst, uint64_t dataBlocks, const uint8_t *seed, uint64_t seedBytes, uint32_t seedCrc, uint32_t generation);
	bool publishBootManifestHash(const sd::RootManifest &manifest);

 public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free() override;

	// Serialised read of whole blocks; the block devices call this.
	IOReturn readBlocks(uint64_t lba, uint32_t count, void *out) {
		if (!gate) return kIOReturnNotReady;
		IOLockLock(gate);
		const int result = host.readBlocks(lba, count, out);
		const sd::Stats &stats = host.stats();
		native_sd_read_calls = native_sd_read_calls + 1;
		native_sd_read_blocks = stats.blocksRead;
		native_sd_commands = stats.commands;
		native_sd_timeouts = stats.timeouts;
		native_sd_pio_bursts = stats.pioBursts;
		IOLockUnlock(gate);
		if (result == sd::kOk) return kIOReturnSuccess;
		return result == sd::kErrTimeout ? kIOReturnTimeout : kIOReturnIOError;
	}

	NativeSdBlock *publishWindow(const char *label, uint64_t first, uint64_t blocks, bool canWrite = false);
	IOReturn writeBlocks(uint64_t lba, uint32_t count, const void *in) {
		if (!gate) return kIOReturnNotReady;
		IOLockLock(gate);
		const int result = host.writeBlocks(lba, count, in);
		native_sd_write_calls = native_sd_write_calls + 1;
		IOLockUnlock(gate);
		if (result == sd::kOk) return kIOReturnSuccess;
		return result == sd::kErrTimeout ? kIOReturnTimeout : kIOReturnIOError;
	}
	IOReturn flushCache() {
		if (!gate) return kIOReturnNotReady;
		IOLockLock(gate);
		const int result = host.flushCache();
		IOLockUnlock(gate);
		if (result == sd::kOk) return kIOReturnSuccess;
		return result == sd::kErrTimeout ? kIOReturnTimeout : kIOReturnIOError;
	}
};

OSDefineMetaClassAndStructors(NativeSd, IOService)

class NativeSdBlock : public IOBlockStorageDevice {
	OSDeclareDefaultStructors(NativeSdBlock)

	NativeSd *owner = nullptr;  // our provider; it outlives us
	uint64_t first = 0;         // first card block of the window
	uint64_t blocks = 0;        // length of the window in blocks
	char label[32] = {};
	uint8_t *bounce = nullptr;
	// The bounce buffer doubles as a read-ahead window: the card answers one 64 KiB chunk, and a caller that
	// faults one 4 KiB page at a time (dyld mapping the shared cache) is served from it. The card path was
	// measured at about eleven requests a second on the board, so sixteen pages per request matters.
	uint64_t cacheFirst = 0;    // first card block the bounce buffer holds
	uint32_t cacheBlocks = 0;   // how many blocks of it are valid
	IOLock *lock = nullptr;     // the bounce buffer is shared by all requests of this window
	uint8_t *ram = nullptr;     // a RAM window: the kernel's mapping of the bytes, and its length
	uint64_t ramBytes = 0;
	bool online = false;
	bool writable = false;    // card window the host core may write (persistent Data)

 public:
	bool configure(NativeSd *controller, const char *name, uint64_t firstBlock, uint64_t blockCount, bool canWrite = false);
	// A window that is kernel memory instead of card blocks: the Data seed. Writable by construction,
	// because that is the whole point of it (see publishDataRamDisk).
	bool configureRam(const char *name, uint8_t *memory, uint64_t bytes);
	const char *windowLabel() const { return label; }
	bool start(IOService *provider) override;
	void free() override;
	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("UniXNU"); }
	char *getProductString() override { return label; }
	char *getRevisionString() override { return const_cast<char *>("1.0"); }
	char *getAdditionalDeviceInfoString() override {
		return const_cast<char *>(ram ? "RK3588 Data RAM disk, writable" : writable ? "RK3588 SD, persistent Data" : "RK3588 SD, read-only");
	}
	IOReturn reportBlockSize(UInt64 *value) override {
		*value = sd::kBlockBytes;
		return kIOReturnSuccess;
	}
	// A card that stays in its slot is neither ejectable nor removable. Both have to be answered:
	// the prelinked AppleAPFSContainerScheme::probe asks, and a missing answer fails the probe.
	IOReturn reportEjectability(bool *value) override {
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *value) override {
		*value = blocks - 1;
		return kIOReturnSuccess;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override {
		*present = online;
		if (changed) *changed = false;
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *value) override {
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportWriteProtection(bool *value) override {
		*value = !(ram || writable);
		return kIOReturnSuccess;
	}
	IOReturn doSynchronizeCache() override {
		return online ? kIOReturnSuccess : kIOReturnNotReady;
	}
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *value) override {
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
	                          IOStorageCompletion *) override;
	// A refused request, on the console, once per reason. A refusal is what makes IOBlockStorageDriver log
	// "executeRequest: request failed to start" and APFS mark the disk failed; without the numbers behind the
	// refusal the only visible symptom is the panic that follows (2026-11-09, Data volume at t+660 s).
	void reject(const char *why, uint64_t block, uint64_t count, IOMemoryDescriptor *buffer, uint64_t bytes);
};

OSDefineMetaClassAndStructors(NativeSdBlock, IOBlockStorageDevice)

// The Data RAM disk, for the two kernel hooks below and for native_sd_data_device. A started device keeps
// its reference for the life of the system, so the pointer stays valid.
static NativeSdBlock *gDataWindow = nullptr;

bool NativeSdBlock::configure(NativeSd *controller, const char *name, uint64_t firstBlock,
                              uint64_t blockCount, bool canWrite) {
	if (!controller || !name || !blockCount) return false;
	if (firstBlock > UINT64_MAX - blockCount) return false;
	if (blockCount > UINT64_MAX / sd::kBlockBytes) return false;
	owner = controller;
	first = firstBlock;
	blocks = blockCount;
	writable = canWrite;
	__builtin_strncpy(label, name, sizeof(label) - 1);
	bounce = static_cast<uint8_t *>(IOMallocData(kChunkBytes));
	lock = IOLockAlloc();
	return bounce && lock;
}

bool NativeSdBlock::configureRam(const char *name, uint8_t *memory, uint64_t bytes) {
	if (!name || !memory || !bytes || (bytes % sd::kBlockBytes)) return false;
	owner = nullptr;
	first = 0;
	blocks = bytes / sd::kBlockBytes;
	ram = memory;
	ramBytes = bytes;
	__builtin_strncpy(label, name, sizeof(label) - 1);
	return true;
}

bool NativeSdBlock::start(IOService *provider) {
	if (!(ram || (bounce && lock)) || !IOBlockStorageDevice::start(provider)) return false;
	online = true;
	// The property the partition schemes and IOMedia read. Without it the media has no name to
	// publish and the whole window is invisible even though it answers every request.
	setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
	// registerService() is what puts the device in the IORegistry; IOBlockStorageDriver matches there.
	// Without it the block device is attached, answers IOBlockStorageDevice calls and logs "published",
	// but no IOMedia, no partition scheme and no APFS container ever appears -- and the kernel sits in
	// "Waiting on <IOMedia, Apple_HFS>" until it panics. UniXNU-evb10's NativeEmmcBlock::start() has the
	// same call (NativeEmmc.cpp:475-479); 2026-11-06 was the board discovering why.
	registerService();
	return true;
}

void NativeSdBlock::free() {
	if (bounce) {
		if (lock) IOLockLock(lock);
		IOFreeData(bounce, kChunkBytes);
		bounce = nullptr;
		if (lock) IOLockUnlock(lock);
	}
	if (lock) {
		IOLockFree(lock);
		lock = nullptr;
	}
	IOBlockStorageDevice::free();
}

void NativeSdBlock::reject(const char *why, uint64_t block, uint64_t count, IOMemoryDescriptor *buffer,
                           uint64_t bytes) {
	// Bounded: a refusal that repeats would otherwise flood a 1.5 Mbaud console and hide what came before it.
	// kRequestLogLines is the same bound the read and write failure paths use.
	static unsigned logged = 0;
	if (logged >= kRequestLogLines) return;
	logged++;
	IOLog("NativeSd: %s refused a request: %s (lba %llu count %llu of %llu blocks, descriptor %llu B,"
	      " request %llu B, direction %d)\n", label, why, ull(block), ull(count), ull(blocks),
	      ull(buffer ? buffer->getLength() : 0), ull(bytes), buffer ? static_cast<int>(buffer->getDirection()) : -1);
}

IOReturn NativeSdBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                         IOStorageAttributes *attributes,
                                         IOStorageCompletion *completion) {
	if (!buffer || !completion || !completion->action) return kIOReturnBadArgument;
	// A zero-length request is not an error. The storage stack issues one for a flush or an empty deblocking
	// step, and refusing it is what the Data volume crashed on: IOBlockStorageDriver logged "request failed to
	// start", APFS logged "disk3: I/O error", and a later request panicked the machine in the error path
	// (2026-11-09, t+660 s, the first run whose userland could actually write to the 2 GiB volume).
	if (!count) {
		completion->action(completion->target, completion->parameter, kIOReturnSuccess, 0);
		return kIOReturnSuccess;
	}
	if (count > UINT64_MAX / sd::kBlockBytes || block >= blocks || count > blocks - block) {
		reject(block >= blocks || count > blocks - block ? "outside the window" : "count overflows", block,
		       count, buffer, 0);
		return kIOReturnBadArgument;
	}
	const UInt64 bytes = count * sd::kBlockBytes;
	if (buffer->getLength() < bytes) {
		reject("the descriptor is shorter than the request", block, count, buffer, bytes);
		return kIOReturnBadArgument;
	}
	if (!online) return kIOReturnNotReady;

	// A RAM window answers both directions: kIODirectionOut means the data flows out of the descriptor
	// (a write of the device), kIODirectionIn means it lands in it (a read).
	if (ram) {
		const IODirection ramDirection = buffer->getDirection();
		if (ramDirection != kIODirectionIn && ramDirection != kIODirectionOut) return kIOReturnBadArgument;
		const bool ramWrite = ramDirection == kIODirectionOut;
		uint8_t *at = ram + block * sd::kBlockBytes;
		const UInt64 moved = ramWrite ? buffer->readBytes(0, at, bytes) : buffer->writeBytes(0, at, bytes);
		const IOReturn ramStatus = moved == bytes ? kIOReturnSuccess : kIOReturnIOError;
		if (ramWrite) native_sd_ram_writes = native_sd_ram_writes + 1;
		if (ramStatus != kIOReturnSuccess) {
			IOLog("NativeSd: RAM %s lba %llu count %llu moved %llu of %llu bytes\n",
			      ramWrite ? "write" : "read", ull(block), ull(count), ull(moved), ull(bytes));
		}
		completion->action(completion->target, completion->parameter, ramStatus, moved);
		return kIOReturnSuccess;
	}
	if (!owner) return kIOReturnNotReady;

	const IODirection direction = buffer->getDirection();
	if (direction == kIODirectionOut) {
		if (!writable) {
			// There is no write path on purpose for the root windows: the card carries the
			// Linux root filesystem and the BootROM fallback chain. See the note at the top
			// of this file. Only the persistent Data window is writable.
			return kIOReturnNotWritable;
		}
		IOLockLock(lock);
		// Chunked through the bounce buffer, like the read path: the card answers 64 KiB runs, and a request
		// larger than that used to allocate a temporary buffer of its own size (IOMallocData(bytes)). Under the
		// write load of a live userland that allocation is the first thing to fail, and a refused write is what
		// took the machine down at t+660 s on 2026-11-09.
		IOReturn status = kIOReturnSuccess;
		UInt64 done = 0;
		while (done < bytes) {
			const uint32_t want = static_cast<uint32_t>((bytes - done) / sd::kBlockBytes);
			const uint32_t take = want < kChunkBlocks ? want : kChunkBlocks;
			const UInt64 length = static_cast<UInt64>(take) * sd::kBlockBytes;
			const UInt64 moved = buffer->readBytes(done, bounce, length);
			if (moved != length) {
				IOLog("NativeSd: %s write: the descriptor gave %llu of %llu bytes at offset %llu\n", label,
				      ull(moved), ull(length), ull(done));
				status = kIOReturnIOError;
				break;
			}
			const uint64_t lba = first + block + done / sd::kBlockBytes;
			IOReturn one = owner->writeBlocks(lba, take, bounce);
			if (one != kIOReturnSuccess) {
				// One retry after a flush: a PIO write that fails once is usually a stale card state, and the
				// alternative is handing APFS an I/O error it does not survive.
				const IOReturn flushed = owner->flushCache();
				IOLog("NativeSd: %s write of %u blocks at %llu failed (%d, flush %d); retrying once\n", label, take,
				      ull(lba), static_cast<int>(one), static_cast<int>(flushed));
				one = owner->writeBlocks(lba, take, bounce);
			}
			if (one != kIOReturnSuccess) {
				IOLog("NativeSd: %s write of %u blocks at %llu failed again (%d)\n", label, take, ull(lba),
				      static_cast<int>(one));
				status = one;
				break;
			}
			done += length;
		}
		if (status == kIOReturnSuccess && attributes &&
		    (attributes->options & kIOStorageOptionForceUnitAccess)) {
			status = owner->flushCache();
		}
		// The bounce buffer caches card reads for this window: a write through the
		// same window must invalidate it, or later reads serve the pre-write bytes.
		// (Only the persistent Data window is writable, so no other window can hold
		// a stale copy of these blocks.)
		if (status == kIOReturnSuccess) cacheBlocks = 0;
		IOLockUnlock(lock);
		if (status == kIOReturnSuccess) native_sd_blocks_written = native_sd_blocks_written + count;
		else native_sd_block_failures = native_sd_block_failures + 1;
		completion->action(completion->target, completion->parameter, status, status == kIOReturnSuccess ? bytes : 0);
		return status;
	}
	if (direction != kIODirectionIn) return kIOReturnBadArgument;

	IOLockLock(lock);
	IOReturn status = kIOReturnSuccess;
	UInt64 done = 0;
	while (done < bytes) {
		const uint64_t lba = first + block + done / sd::kBlockBytes;
		const uint32_t want = static_cast<uint32_t>((bytes - done) / sd::kBlockBytes);
		if (cacheBlocks != 0 && lba >= cacheFirst && lba < cacheFirst + cacheBlocks) {
			// Take as much as this chunk holds: a request larger than one chunk comes back for the rest,
			// which is what keeps the loop moving (a whole-request test would never match).
			const uint32_t available = static_cast<uint32_t>(cacheFirst + cacheBlocks - lba);
			const uint32_t take = want < available ? want : available;
			const uint64_t offset = (lba - cacheFirst) * sd::kBlockBytes;
			const UInt64 length = static_cast<UInt64>(take) * sd::kBlockBytes;
			if (buffer->writeBytes(done, bounce + offset, length) != length) {
				status = kIOReturnIOError;
				break;
			}
			done += length;
			continue;
		}
		// Miss: fill the bounce buffer with the whole aligned chunk that holds this block, and serve the
		// rest of this request (and the following ones) from it.
		const uint64_t chunkFirst = lba & ~static_cast<uint64_t>(kChunkBlocks - 1);
		uint32_t chunkBlocks = kChunkBlocks;
		if (chunkFirst + chunkBlocks > first + blocks) {
			chunkBlocks = static_cast<uint32_t>(first + blocks - chunkFirst);
		}
		cacheBlocks = 0;
		if (traceIoEnabled()) {
			// Straight to the UART: when the boot stops dead, the last line here is the request it died in.
			pePut("\r\nio lba ");
			peNum(chunkFirst);
			pePut(" blocks ");
			peNum(chunkBlocks);
		}
		// Observability only (round 4): what this 64 KiB chunk costs, accumulated and never printed from here,
		// because a line per chunk would change the I/O it measures. The counter is the one the display path
		// already uses (LegacyDisplayProbe.cpp:237); reading it has no side effect on the FIFO loop below.
		uint64_t chunkStart = 0, chunkEnd = 0, chunkFrequency = 0;
		__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(chunkStart));
		const IOReturn result = owner->readBlocks(chunkFirst, chunkBlocks, bounce);
		__asm__ volatile("isb\n\tmrs %0, cntpct_el0\n\tmrs %1, cntfrq_el0"
		                 : "=r"(chunkEnd), "=r"(chunkFrequency));
		if (result == kIOReturnSuccess && chunkFrequency != 0) {
			native_sd_chunk_us = native_sd_chunk_us + (chunkEnd - chunkStart) * 1000000ull / chunkFrequency;
			native_sd_chunks = native_sd_chunks + 1;
		}
		if (result != kIOReturnSuccess) {
			status = result;
			native_sd_block_failures = native_sd_block_failures + 1;
			if (native_sd_block_failures <= kRequestLogLines) {
				IOLog("NativeSd: %s read of %u blocks at %llu failed (%d)\n", label, chunkBlocks,
				      ull(chunkFirst), static_cast<int>(result));
			}
			break;
		}
		native_sd_blocks_read = native_sd_blocks_read + chunkBlocks;
		cacheFirst = chunkFirst;
		cacheBlocks = chunkBlocks;
	}
	native_sd_block_requests = native_sd_block_requests + 1;
	IOLockUnlock(lock);

	if (status == kIOReturnSuccess && attributes &&
	    (attributes->options & kIOStorageOptionForceUnitAccess)) {
		// Nothing to flush on a read-only window, but a caller that asked for it should not learn
		// that the option was ignored.
	}
	completion->action(completion->target, completion->parameter, status, bytes);
	return status;
}

NativeSdBlock *NativeSd::publishWindow(const char *label, uint64_t firstBlock, uint64_t blocks, bool canWrite) {
	NativeSdBlock *device = OSTypeAlloc(NativeSdBlock);
	if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
	    !device->configure(this, label, firstBlock, blocks, canWrite) || !device->attach(this)) {
		IOLog("NativeSd: cannot prepare block device %s\n", label);
		if (device) device->release();
		return nullptr;
	}
	if (!device->start(this)) {
		IOLog("NativeSd: block device %s did not start\n", label);
		device->detach(this);
		device->release();
		return nullptr;
	}
	// Started objects keep their reference: IOMedia clients hold the device for the life of the system.
	native_sd_blocks_published = native_sd_blocks_published + 1;
	IOLog("NativeSd: published %s: lba %llu + %llu blocks (%llu MiB), %s\n", device->windowLabel(),
	      ull(firstBlock), ull(blocks), ull(blocks >> 11), canWrite ? "writable" : "read-only");
	return device;
}

// The kernel hook bsd_init.c asks before it refuses an unsealed root
// (UniXNU-rk3588-common patches/xnu/rk3588-root-trust.patch). Every IPSW OS volume is unsealed, so on a
// developer board the root can only ever be "authenticated" by its owner; the patch clears require_rootauth
// when the root is mounted read-only *and* this returns non-zero.
//
// Three conditions here, matching what the eMMC driver of the EVB10 requires: the owner asked for it
// (`sd-root-trust=1`), root windows were published at all, and the device really is a volume inside one of
// them. The last one is checked by finding the IOMedia whose BSD major/minor is `dev` and walking its
// providers up to our own block class -- the dev_t is assigned by the BSD layer after the media exists, so
// it cannot be remembered at publish time.
extern "C" int native_emmc_root_trusted(int32_t dev) {
	if (!native_sd_root_trust || !native_sd_root_windows) {
		return 0;
	}
	OSDictionary *match = IOService::serviceMatching("IOMedia");
	OSIterator *iterator = match ? IOService::getMatchingServices(match) : nullptr;
	if (match) match->release();
	if (!iterator) {
		return 0;
	}
	// dev_t on Darwin: major in the top 8 bits, minor in the low 24.
	const uint32_t wantedMajor = (static_cast<uint32_t>(dev) >> 24) & 0xffu;
	const uint32_t wantedMinor = static_cast<uint32_t>(dev) & 0xffffffu;
	int trusted = 0;
	while (IOService *media = static_cast<IOService *>(iterator->getNextObject())) {
		OSNumber *major = OSDynamicCast(OSNumber, media->getProperty("BSD Major"));
		OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty("BSD Minor"));
		if (!major || !minor) continue;
		if (major->unsigned32BitValue() != wantedMajor || minor->unsigned32BitValue() != wantedMinor) continue;
		for (IOService *walk = media->getProvider(); walk; walk = walk->getProvider()) {
			if (OSDynamicCast(NativeSdBlock, walk)) {
				trusted = 1;
				break;
			}
		}
		break;
	}
	iterator->release();
	if (trusted) {
		IOLog("NativeSd: root volume dev %d is inside a published window; sd-root-trust=1 accepts it\n", dev);
	}
	return trusted;
}

// The payload's Data seed as a writable block device of its own. The loader copies the seed and writes
// {physical base, size} into /chosen/memory-map/RAMDisk (docs/concepts/payload.md, "Dense | RAMDisk |
// DarwinBoot copies the whole image"), which is the handoff the EVB10's driver consumes as well.
//
// It hangs off this service rather than off the card: nothing about it depends on the controller. Without
// it launchd has no /private/var -- its swap file and the shared-region cache both fail, and the boot stops
// right after libignition's first message. The seed is kernel memory, so the volume does not survive a reset.
bool NativeSd::publishDataRamDisk() {
	IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
	if (!map) {
		IOLog("NativeSd: data-ram: no /chosen/memory-map\n");
		return false;
	}
	OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
	uint64_t physical = 0, size = 0;
	if (range && range->getLength() == 16) {
		memcpy(&physical, range->getBytesNoCopy(), 8);
		memcpy(&size, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
	}
	map->release();
	if (!size) {
		IOLog("NativeSd: data-ram: the payload hands over no RAMDisk\n");
		return false;
	}
	// The same bounds the original RamBlock driver checks: page aligned, inside the memory the kernel was
	// given, and small enough to be a seed.
	if (size > 0x80000000ull || (physical & 4095) || (size & 4095) || physical < gPhysBase ||
	    size > gPhysSize || physical - gPhysBase > gPhysSize - size) {
		IOLog("NativeSd: data-ram: RAMDisk %#llx + %#llx is not boot memory (%#lx + %#lx)\n",
		      ull(physical), ull(size), gPhysBase, gPhysSize);
		return false;
	}
	uint8_t *memory = reinterpret_cast<uint8_t *>(ml_static_ptovirt(static_cast<uintptr_t>(physical)));
	if (!memory) {
		IOLog("NativeSd: data-ram: no static mapping for %#llx\n", ull(physical));
		return false;
	}
	IOLog("NativeSd: data-ram: RAMDisk %#llx + %llu bytes at %p, header %02x%02x%02x%02x (NXSB at +32: %02x%02x%02x%02x)\n",
	      ull(physical), ull(size), static_cast<void *>(memory), memory[0], memory[1], memory[2], memory[3],
	      memory[32], memory[33], memory[34], memory[35]);
	NativeSdBlock *device = OSTypeAlloc(NativeSdBlock);
	if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
	    !device->configureRam("Data", memory, size) || !device->attach(this)) {
		IOLog("NativeSd: cannot prepare the Data RAM device\n");
		if (device) device->release();
		return false;
	}
	if (!device->start(this)) {
		IOLog("NativeSd: the Data RAM device did not start\n");
		device->detach(this);
		device->release();
		return false;
	}
	native_sd_blocks_published = native_sd_blocks_published + 1;
	native_sd_data_ready = 1;
	gDataWindow = device;
	IOLog("NativeSd: published Data: RAM %p + %llu bytes (%llu MiB), writable\n", static_cast<void *>(memory),
	      ull(size), ull(size >> 20));
	return true;
}

// The device of the volume inside the Data RAM disk, for the kernel patch that mounts it on /private/var
// (patches/xnu/0002-mount-data-volume.patch). APFS names volumes "diskNsM", so the volume is the media whose
// BSD name carries the "s" part and whose provider chain reaches the Data window -- the container's own
// media is "diskN" and is not what a mount wants.
extern "C" int native_sd_data_device(char *path, size_t size, int32_t *device) {
	if (!gDataWindow || !path || size < 8) return 0;
	OSDictionary *match = IOService::serviceMatching("IOMedia");
	OSIterator *iterator = match ? IOService::getMatchingServices(match) : nullptr;
	if (match) match->release();
	if (!iterator) return 0;
	char best[32] = {};
	int32_t bestDevice = 0;
	while (IOService *media = static_cast<IOService *>(iterator->getNextObject())) {
		OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
		OSNumber *major = OSDynamicCast(OSNumber, media->getProperty("BSD Major"));
		OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty("BSD Minor"));
		if (!name || !major || !minor) continue;
		const char *text = name->getCStringNoCopy();
		if (!text || __builtin_strncmp(text, "disk", 4) != 0 || !__builtin_strchr(text + 4, 's')) continue;
		if (__builtin_strlen(text) >= sizeof(best)) continue;
		bool ours = false;
		for (IOService *walk = media->getProvider(); walk; walk = walk->getProvider()) {
			if (walk == gDataWindow) {
				ours = true;
				break;
			}
		}
		if (!ours) continue;
		__builtin_strncpy(best, text, sizeof(best) - 1);
		bestDevice = static_cast<int32_t>((major->unsigned32BitValue() << 24) |
		                                  (minor->unsigned32BitValue() & 0xffffffu));
	}
	iterator->release();
	if (!best[0]) {
		IOLog("NativeSd: data-ram: no volume media under the Data window yet\n");
		return 0;
	}
	snprintf(path, size, "/dev/%s", best);
	if (device) *device = bestDevice;
	return 1;
}

// The device of the volume with this UUID inside any window published here, for the kernel patch that mounts
// the SystemOS cryptex over / (patches/xnu/0002-mount-data-volume.patch). APFS publishes the volume UUID as
// the "UUID" property of its IOMedia -- the same property /chosen/root-matching matched the root by -- so the
// lookup is by UUID and the provider chain only has to prove the media is one of ours.
extern "C" int native_sd_volume_device(const char *uuid, char *path, size_t size, int32_t *device) {
	if (!uuid || !path || size < 8) return 0;
	OSDictionary *match = IOService::serviceMatching("IOMedia");
	OSIterator *iterator = match ? IOService::getMatchingServices(match) : nullptr;
	if (match) match->release();
	if (!iterator) return 0;
	int found = 0;
	while (IOService *media = static_cast<IOService *>(iterator->getNextObject())) {
		OSString *mediaUUID = OSDynamicCast(OSString, media->getProperty("UUID"));
		OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
		OSNumber *major = OSDynamicCast(OSNumber, media->getProperty("BSD Major"));
		OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty("BSD Minor"));
		if (!mediaUUID || !name || !major || !minor) continue;
		if (__builtin_strcasecmp(mediaUUID->getCStringNoCopy(), uuid) != 0) continue;
		bool ours = false;
		for (IOService *walk = media->getProvider(); walk; walk = walk->getProvider()) {
			if (OSDynamicCast(NativeSdBlock, walk)) {
				ours = true;
				break;
			}
		}
		if (!ours) continue;
		snprintf(path, size, "/dev/%s", name->getCStringNoCopy());
		if (device) {
			*device = static_cast<int32_t>((major->unsigned32BitValue() << 24) |
			                               (minor->unsigned32BitValue() & 0xffffffu));
		}
		found = 1;
		break;
	}
	iterator->release();
	if (!found) IOLog("NativeSd: no volume %s under the published windows\n", uuid);
	return found;
}

bool NativeSd::scanGpt() {
	uint8_t *scratch = static_cast<uint8_t *>(IOMallocData(sd::kGptScratchBytes));
	sd::GptEntry *entries =
	    static_cast<sd::GptEntry *>(IOMallocData(sizeof(sd::GptEntry) * sd::kGptMaxEntries));
	if (!scratch || !entries) {
		if (scratch) IOFreeData(scratch, sd::kGptScratchBytes);
		if (entries) IOFreeData(entries, sizeof(sd::GptEntry) * sd::kGptMaxEntries);
		return false;
	}
	sd::GptInfo info = {};
	const int status = sd::readGpt(
	    [this](uint64_t lba, uint32_t count, void *out) {
		    return host.readBlocks(lba, count, out) == sd::kOk;
	    },
	    sd::kBlockBytes, scratch, info, entries, sd::kGptMaxEntries);

	bool usable = false;
	if (status != sd::kGptOk) {
		IOLog("NativeSd: no usable GPT at block 1 (status %d)\n", status);
	} else {
		IOLog("NativeSd: GPT: my_lba %llu alt_lba %llu first_usable %llu last_usable %llu, %u entries, header crc %s, entries crc %s\n",
		      ull(info.myLba), ull(info.altLba), ull(info.firstUsable), ull(info.lastUsable), info.count,
		      info.headerCrcOk ? "ok" : "BAD", info.entriesCrcOk ? "ok" : "BAD");
		diskBlocks = info.altLba + 1;
		char gptName[37];
		sd::formatGuid(info.diskGuid, gptName);
		IOLog("NativeSd: disk %llu blocks (%llu MiB), disk GUID %s\n", ull(diskBlocks),
		      ull((diskBlocks * sd::kBlockBytes) >> 20), gptName);

		char wanted[37] = {};
		if (!bootArgString("sd-partition", wanted, sizeof(wanted))) {
			__builtin_strncpy(wanted, "primary", sizeof(wanted) - 1);
		}
		// The root image lives in its own partition, so that the Linux root and the root window set never
		// share a partition: `unixnu build userdata` writes the whole image for that partition.
		char rootName[37] = {};
		if (!bootArgString("sd-root-partition", rootName, sizeof(rootName))) {
			__builtin_strncpy(rootName, "userdata", sizeof(rootName) - 1);
		}
		for (unsigned i = 0; i < info.used; ++i) {
			char unique[37];
			sd::formatGuid(entries[i].uniqueGuid, unique);
			IOLog("NativeSd: gpt[%u] \"%s\" %llu..%llu (%llu MiB) guid %s\n", i, entries[i].name,
			      ull(entries[i].first), ull(entries[i].last),
			      ull(((entries[i].last - entries[i].first + 1) * sd::kBlockBytes) >> 20), unique);
			if (!partFound && sd::sameName(entries[i].name, wanted)) {
				partFirst = entries[i].first;
				partLast = entries[i].last;
				partFound = true;
				native_sd_part_first = partFirst;
				native_sd_part_last = partLast;
			}
			if (!rootFound && sd::sameName(entries[i].name, rootName)) {
				rootFirst = entries[i].first;
				rootLast = entries[i].last;
				rootFound = true;
			}
		}
		if (!partFound) {
			IOLog("NativeSd: no partition named \"%s\"\n", wanted);
		}
		IOLog("NativeSd: root image partition \"%s\": %s\n", rootName,
		      rootFound ? "found" : "absent");
		usable = true;
	}
	IOFreeData(scratch, sd::kGptScratchBytes);
	IOFreeData(entries, sizeof(sd::GptEntry) * sd::kGptMaxEntries);
	return usable;
}

// Reads the same blocks twice and compares. The second read is a run of blocks, so it goes out as one
// CMD18 while the first was split by the core into CMD17s only when count is 1; comparing the two is
// therefore a direct check that multi-block transfers deliver what single-block ones did. A card that
// fails this is left alone by the rest of the session: the driver publishes nothing rather than
// serving a filesystem from a card whose transfers do not agree with themselves.
void NativeSd::probePartition() {
	const uint32_t limit = bootArg("sd-probe", 256);
	if (!partFound || limit < 2) {
		IOLog("NativeSd: probe skipped (partition %s, limit %u)\n", partFound ? "found" : "missing", limit);
		return;
	}
	const uint64_t blocks = partLast - partFirst + 1;
	const uint32_t count = blocks < limit ? static_cast<uint32_t>(blocks) : limit;
	if (count < 2) return;
	uint8_t *single = static_cast<uint8_t *>(IOMallocData(count * sd::kBlockBytes));
	uint8_t *run = static_cast<uint8_t *>(IOMallocData(count * sd::kBlockBytes));
	if (!single || !run) {
		if (single) IOFreeData(single, count * sd::kBlockBytes);
		if (run) IOFreeData(run, count * sd::kBlockBytes);
		return;
	}

	// One block at a time first: count == 1 keeps CMD18 out of the picture entirely.
	bool singlesOk = true;
	uint64_t at = partFirst;
	for (uint32_t i = 0; i < count; ++i) {
		const int result = host.readBlocks(at + i, 1, single + static_cast<size_t>(i) * sd::kBlockBytes);
		if (result != sd::kOk) {
			IOLog("NativeSd: single-block read of %llu failed (%s)\n", ull(at + i), sd::resultName(result));
			singlesOk = false;
			break;
		}
	}
	if (singlesOk) {
		const int result = host.readBlocks(partFirst, count, run);
		if (result != sd::kOk) {
			IOLog("NativeSd: multi-block read of %u blocks at %llu failed (%s)\n", count, ull(partFirst),
			      sd::resultName(result));
		} else if (__builtin_memcmp(single, run, count * sd::kBlockBytes) != 0) {
			IOLog("NativeSd: multi-block read of %u blocks disagrees with the single-block reads\n", count);
		} else {
			native_sd_multi_block = 1;
			IOLog("NativeSd: %u blocks read both ways agree; multi-block transfers verified\n", count);
		}
	}
	if (count) hexLine("partition first", partFirst, single, 16);
	IOFreeData(single, count * sd::kBlockBytes);
	IOFreeData(run, count * sd::kBlockBytes);
}

bool NativeSd::publishRootWindows() {
	if (!rootFound) {
		char name[37] = {};
		if (!bootArgString("sd-root-partition", name, sizeof(name))) {
			__builtin_strncpy(name, "userdata", sizeof(name) - 1);
		}
		IOLog("NativeSd: sd=4 but there is no partition named \"%s\"; nothing published\n", name);
		return false;
	}
	// The manifest is the first 4 KiB of the partition, sector aligned by construction. It is read
	// through the same bounded readBlocks() as everything else: if it lands outside the capacity the
	// driver believes in, that is a failure worth seeing rather than a short read to paper over.
	static_assert(sd::kRootManifestBytes % sd::kBlockBytes == 0, "the manifest must be whole blocks");
	const uint32_t pageBlocks = sd::kRootManifestBytes / sd::kBlockBytes;
	uint8_t *page = static_cast<uint8_t *>(IOMallocData(sd::kRootManifestBytes));
	if (!page) {
		IOLog("NativeSd: no memory for the root manifest\n");
		return false;
	}
	const int read = host.readBlocks(rootFirst, pageBlocks, page);
	if (read != sd::kOk) {
		IOLog("NativeSd: cannot read the root manifest at %llu (%s)\n", ull(rootFirst), sd::resultName(read));
		IOFreeData(page, sd::kRootManifestBytes);
		return false;
	}
	const uint64_t partitionBytes = (rootLast - rootFirst + 1) * sd::kBlockBytes;
	sd::RootManifest manifest = {};
	const int status = sd::parseRootManifest(page, partitionBytes, sha256, manifest);
	IOFreeData(page, sd::kRootManifestBytes);
	native_sd_root_status = static_cast<uint32_t>(status);
	if (status != sd::kRootOk) {
		// kRootNoMagic is the expected answer on a partition that has not been flashed with a root image
		// yet; every other status means an image is there and is not usable, which is a different problem.
		IOLog("NativeSd: no usable root image in the partition: %s (status %d)\n", sd::rootStatusName(status),
		      status);
		return false;
	}

	sd::RootPlan plan = {};
	if (!sd::buildRootPlan(manifest, plan)) {
		IOLog("NativeSd: the root manifest has no OS window; nothing to mount\n");
		return false;
	}
	native_sd_root_image_bytes = manifest.imageBytes;
	IOLog("NativeSd: root manifest ok: %u window(s), image %llu bytes (%llu MiB)\n", manifest.count,
	      ull(manifest.imageBytes), ull(manifest.imageBytes >> 20));

	for (unsigned position = 0; position < plan.count; ++position) {
		const sd::RootWindow &window = manifest.window[plan.order[position]];
		// Both fields passed parseRootManifest's alignment rules, so these divisions are exact.
		const uint64_t firstBlock = rootFirst + window.offset / sd::kBlockBytes;
		const uint64_t blocks = window.size / sd::kBlockBytes;
		IOLog("NativeSd: window \"%s\": %llu + %llu blocks (%llu MiB), read-only\n", window.name,
		      ull(firstBlock), ull(blocks), ull((blocks * sd::kBlockBytes) >> 20));
		if (!publishWindow(window.name, firstBlock, blocks)) {
			return false;
		}
		native_sd_root_windows = native_sd_root_windows + 1;
	}
	// launchd's own data-volume mount task (`mount -P 1`) cannot proceed without the identity iBoot would
	// have published; on a board without iBoot the owner's record of what was booted takes its place.
	if (bootArg("sd-boot-manifest", 1)) publishBootManifestHash(manifest);
	return true;
}

// /chosen/boot-manifest-hash: the SHA-384 an iBoot boot would have put there. Without it the original
// `mount -P 1` fails ("failed to get boot manifest hash", exit status 66) and launchd gives up on its
// own data-volume mount. There is no iBoot here, so the hash names what the owner booted: the SHA-256 of
// the OS window and of the SystemOS window as the root manifest lists them, each with its role byte,
// under a domain of this port's own.
bool NativeSd::publishBootManifestHash(const sd::RootManifest &manifest) {
	static const char domain[] = "SdRootImage.OwnerBootSet.v1";  // the NUL is part of it
	const sd::RootWindow *os = nullptr;
	const sd::RootWindow *system = nullptr;
	for (uint32_t index = 0; index < manifest.count; ++index) {
		const sd::RootWindow &window = manifest.window[index];
		if (__builtin_strcmp(window.name, "OS") == 0) os = &window;
		if (__builtin_strcmp(window.name, "SystemOS") == 0) system = &window;
	}
	if (!os || !system) {
		IOLog("NativeSd: boot-manifest-hash: the root image has no %s window\n", os ? "SystemOS" : "OS");
		return false;
	}
	const uint8_t osRole = 1, systemRole = 2;
	uint8_t digest[SHA384_DIGEST_LENGTH];
	SHA384_CTX context;
	SHA384_Init(&context);
	SHA384_Update(&context, domain, sizeof(domain));
	SHA384_Update(&context, &osRole, sizeof(osRole));
	SHA384_Update(&context, os->sha256, sizeof(os->sha256));
	SHA384_Update(&context, &systemRole, sizeof(systemRole));
	SHA384_Update(&context, system->sha256, sizeof(system->sha256));
	SHA384_Final(digest, &context);

	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (!chosen) {
		IOLog("NativeSd: boot-manifest-hash: no /chosen\n");
		return false;
	}
	bool published = false;
	if (chosen->getProperty("boot-manifest-hash")) {
		IOLog("NativeSd: boot-manifest-hash: /chosen already carries one, not replaced\n");
	} else {
		OSData *value = OSData::withBytes(digest, sizeof(digest));
		OSData *tag = OSData::withBytes(domain, sizeof(domain));
		published = value && tag && chosen->setProperty("owner-boot-manifest-domain", tag) &&
		            chosen->setProperty("boot-manifest-hash", value);
		OSSafeReleaseNULL(tag);
		OSSafeReleaseNULL(value);
	}
	chosen->release();
	if (published) {
		char text[2 * sizeof(digest) + 1];
		for (unsigned i = 0; i < sizeof(digest); ++i) {
			static const char digits[] = "0123456789abcdef";
			text[2 * i] = digits[digest[i] >> 4];
			text[2 * i + 1] = digits[digest[i] & 15];
		}
		text[2 * sizeof(digest)] = 0;
		IOLog("NativeSd: boot-manifest-hash published in /chosen: %s (domain %s)\n", text, domain);
	}
	return published;
}

namespace {
constexpr uint32_t kDataHeaderBlocks = 2048;        // 1 MiB
constexpr uint64_t kDataMarginBlocks = 131072;      // 64 MiB between the root image and the window
constexpr uint64_t kDataZeroBlocks = 131072;        // 64 MiB zeroed in front of the container when formatting
constexpr uint64_t kDataMaxBytes = 8ull << 30;
constexpr uint32_t kDataVersion = 1;
constexpr uint32_t kDataFormatting = 1, kDataReady = 2;
constexpr char kSdDataMagic[8] = {'S', 'D', 'D', 'A', 'T', 'A', '0', '1'};

struct SdDataHeader {
	uint32_t version = 0, state = 0;
	uint64_t dataBlocks = 0, seedBytes = 0;
	uint32_t seedCrc = 0, generation = 0;
};

void encodeSdDataHeader(uint8_t (&block)[sd::kBlockBytes], const SdDataHeader &header) {
	bzero(block, sizeof(block));
	memcpy(block, kSdDataMagic, 8);
	memcpy(block + 8, &header.version, 4);
	memcpy(block + 12, &header.state, 4);
	memcpy(block + 16, &header.dataBlocks, 8);
	memcpy(block + 24, &header.seedBytes, 8);
	memcpy(block + 32, &header.seedCrc, 4);
	memcpy(block + 36, &header.generation, 4);
	const uint32_t crc = sd::crc32(block, 508);
	memcpy(block + 508, &crc, 4);
}

bool decodeSdDataHeader(const uint8_t (&block)[sd::kBlockBytes], SdDataHeader &header) {
	if (memcmp(block, kSdDataMagic, 8)) return false;
	uint32_t crc = 0;
	memcpy(&crc, block + 508, 4);
	if (crc != sd::crc32(block, 508)) return false;
	memcpy(&header.version, block + 8, 4);
	memcpy(&header.state, block + 12, 4);
	memcpy(&header.dataBlocks, block + 16, 8);
	memcpy(&header.seedBytes, block + 24, 8);
	memcpy(&header.seedCrc, block + 32, 4);
	memcpy(&header.generation, block + 36, 4);
	return true;
}

}  // namespace

bool NativeSd::formatDataWindow(uint64_t windowFirst, uint64_t dataBlocks, const uint8_t *seed, uint64_t seedBytes, uint32_t seedCrc, uint32_t generation) {
	const uint64_t dataFirst = windowFirst + kDataHeaderBlocks;
	alignas(8) uint8_t block[sd::kBlockBytes];
	uint8_t *buffer = static_cast<uint8_t *>(IOMallocData(kChunkBytes));
	if (!buffer) return false;
	bool ok = true;
	SdDataHeader header;
	header.version = kDataVersion;
	header.state = kDataFormatting;
	header.dataBlocks = dataBlocks;
	header.seedBytes = seedBytes;
	header.seedCrc = seedCrc;
	header.generation = generation;
	encodeSdDataHeader(block, header);
	if (writeBlocks(windowFirst, 1, block) != kIOReturnSuccess || flushCache() != kIOReturnSuccess) {
		IOLog("NativeSd: data: cannot write the header of the Data window\n");
		ok = false;
	}
	// Only zero the part of the container the seed does not cover: when the seed is the
	// whole container (our 64 MiB mkapfs image) this skips a redundant full-container write,
	// which matters because PIO writes are slow and the watchdog fires ~89 s after boot.
	const uint64_t seedBlocks = (seedBytes + sd::kBlockBytes - 1) / sd::kBlockBytes;
	const uint64_t zeroFrom = seedBlocks < dataBlocks ? seedBlocks : dataBlocks;
	const uint64_t zeroBlocks = dataBlocks - zeroFrom < kDataZeroBlocks ? dataBlocks - zeroFrom : kDataZeroBlocks;
	bzero(buffer, kChunkBytes);
	for (uint64_t at = 0; ok && at < zeroBlocks; at += kChunkBlocks) {
		const uint32_t count = static_cast<uint32_t>(zeroBlocks - at < kChunkBlocks ? zeroBlocks - at : kChunkBlocks);
		if (writeBlocks(dataFirst + zeroFrom + at, count, buffer) != kIOReturnSuccess) {
			IOLog("NativeSd: data: zeroing failed at block %llu of the window\n", ull(zeroFrom + at));
			ok = false;
		}
	}
	for (uint64_t at = 0; ok && at < seedBytes; at += kChunkBytes) {
		const uint64_t bytes = seedBytes - at < kChunkBytes ? seedBytes - at : kChunkBytes;
		memcpy(buffer, seed + at, static_cast<size_t>(bytes));
		if (writeBlocks(dataFirst + at / sd::kBlockBytes, static_cast<uint32_t>(bytes / sd::kBlockBytes), buffer) != kIOReturnSuccess) {
			IOLog("NativeSd: data: writing the seed failed at byte %llu\n", ull(at));
			ok = false;
		}
	}
	if (ok && flushCache() != kIOReturnSuccess) {
		IOLog("NativeSd: data: the flush after the seed failed\n");
		ok = false;
	}
	if (ok) {
		uint32_t crc = 0;
		for (uint64_t at = 0; ok && at < seedBytes; at += kChunkBytes) {
			const uint64_t bytes = seedBytes - at < kChunkBytes ? seedBytes - at : kChunkBytes;
			if (readBlocks(dataFirst + at / sd::kBlockBytes, static_cast<uint32_t>(bytes / sd::kBlockBytes), buffer) != kIOReturnSuccess) {
				IOLog("NativeSd: data: read-back failed at byte %llu\n", ull(at));
				ok = false;
				break;
			}
			crc = sd::crc32(buffer, static_cast<size_t>(bytes), crc);
		}
		if (ok && crc != seedCrc) {
			IOLog("NativeSd: data: the seed read back from the card differs (crc32 0x%08x, expected 0x%08x)\n", crc, seedCrc);
			ok = false;
		}
	}
	if (ok) {
		header.state = kDataReady;
		encodeSdDataHeader(block, header);
		if (writeBlocks(windowFirst, 1, block) != kIOReturnSuccess || flushCache() != kIOReturnSuccess) {
			IOLog("NativeSd: data: cannot write the ready header\n");
			ok = false;
		}
	}
	IOFreeData(buffer, kChunkBytes);
	IOLog("NativeSd: data: format %s at lba %llu (%llu MiB)\n", ok ? "ready" : "FAILED", ull(windowFirst), ull((kDataHeaderBlocks + dataBlocks) >> 11));
	return ok;
}

bool NativeSd::persistentData() {
	IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
	if (!map) {
		IOLog("NativeSd: data-persist: no /chosen/memory-map\n");
		return false;
	}
	OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
	uint64_t physical = 0, size = 0;
	if (range && range->getLength() == 16) {
		memcpy(&physical, range->getBytesNoCopy(), 8);
		memcpy(&size, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
	}
	map->release();
	if (!size) {
		IOLog("NativeSd: data-persist: the payload hands over no RAMDisk seed\n");
		return false;
	}
	const uint8_t *seed = reinterpret_cast<const uint8_t *>(ml_static_ptovirt(static_cast<uintptr_t>(physical)));
	if (!seed) {
		IOLog("NativeSd: data-persist: no static mapping for seed %#llx\n", ull(physical));
		return false;
	}
	uint32_t apfsBlock = 0;
	uint64_t apfsCount = 0;
	if (size >= 4096 && !memcmp(seed + 32, "NXSB", 4)) {
		memcpy(&apfsBlock, seed + 36, 4);
		memcpy(&apfsCount, seed + 40, 8);
	}
	if (apfsBlock < 4096 || apfsBlock > 65536 || (apfsBlock & (apfsBlock - 1)) || !apfsCount || apfsCount > kDataMaxBytes / apfsBlock) {
		IOLog("NativeSd: data-persist: the seed is no APFS container (block size %u, %llu blocks)\n", apfsBlock, ull(apfsCount));
		return false;
	}
	const uint64_t containerBytes = static_cast<uint64_t>(apfsBlock) * apfsCount;
	if ((containerBytes & 0xfffff) || containerBytes < size || containerBytes < (64ull << 20)) {
		IOLog("NativeSd: data-persist: container %llu bytes does not fit seed %llu bytes\n", ull(containerBytes), ull(size));
		return false;
	}
	const uint64_t dataBlocks = containerBytes / sd::kBlockBytes;
	const uint64_t total = kDataHeaderBlocks + dataBlocks;
	if (!rootFound || rootLast <= rootFirst) {
		IOLog("NativeSd: data-persist: no usable root partition\n");
		return false;
	}
	const uint64_t partBlocks = rootLast - rootFirst + 1;
	if (total > partBlocks) {
		IOLog("NativeSd: data-persist: %llu MiB do not fit userdata (%llu MiB)\n", ull(total >> 11), ull(partBlocks >> 11));
		return false;
	}
	const uint64_t windowFirst = rootLast + 1 - total;
	const uint64_t rootEnd = rootFirst + (native_sd_root_image_bytes + sd::kBlockBytes - 1) / sd::kBlockBytes;
	if (windowFirst < rootEnd + kDataMarginBlocks) {
		IOLog("NativeSd: data-persist: window at %llu too close to root end %llu; not writing\n", ull(windowFirst), ull(rootEnd));
		return false;
	}
	IOLog("NativeSd: data-persist: window lba %llu..%llu (%llu MiB), root ends at %llu\n", ull(windowFirst), ull(rootLast), ull(total >> 11), ull(rootEnd));
	const uint32_t seedCrc = sd::crc32(seed, static_cast<size_t>(size));
	alignas(8) uint8_t block[sd::kBlockBytes];
	SdDataHeader header;
	const bool haveHeader = readBlocks(windowFirst, 1, block) == kIOReturnSuccess && decodeSdDataHeader(block, header);
	const bool reset = bootArg("sd-data-reset", 0) != 0;
	const bool keep = haveHeader && !reset && header.version == kDataVersion && header.state == kDataReady && header.dataBlocks == dataBlocks;
	if (haveHeader) {
		IOLog("NativeSd: data-persist: header v%u state %u, %llu blocks, crc 0x%08x, gen %u%s\n", header.version, header.state, ull(header.dataBlocks), header.seedCrc, header.generation, reset ? " (reset)" : "");
	} else {
		IOLog("NativeSd: data-persist: no header in the window%s: formatting\n", reset ? " (reset)" : "");
	}
	if (!host.setWriteRange(windowFirst, rootLast)) {
		IOLog("NativeSd: data-persist: host would not open the window for writing\n");
		return false;
	}
	if (keep) {
		if (header.seedCrc != seedCrc) {
			IOLog("NativeSd: data-persist: seed changed (crc 0x%08x, payload 0x%08x); contents kept\n", header.seedCrc, seedCrc);
		}
	} else if (!formatDataWindow(windowFirst, dataBlocks, seed, size, seedCrc, haveHeader ? header.generation + 1 : 1)) {
		host.clearWriteRange();
		return false;
	}
	NativeSdBlock *device = OSTypeAlloc(NativeSdBlock);
	bool ok = device && device->init(static_cast<OSDictionary *>(nullptr)) &&
	    device->configure(this, "Data", windowFirst + kDataHeaderBlocks, dataBlocks, true) && device->attach(this) && device->start(this);
	if (!ok) {
		IOLog("NativeSd: data-persist: cannot publish the Data window\n");
		if (device) device->release();
		host.clearWriteRange();
		return false;
	}
	gDataWindow = device;
	IOLog("NativeSd: data-persist: published Data on the card: lba %llu + %llu blocks (%llu MiB), writable\n", ull(windowFirst + kDataHeaderBlocks), ull(dataBlocks), ull(dataBlocks >> 11));
	return true;
}

// The desk has no power switch, so the board is restarted by a byte typed at the host's UART: Ctrl-] (0x1d)
// is the agreed one. Nothing else reads the UART while board.toml passes serial=1, which keeps the console's
// INPUT bit clear, so serial_keyboard_poll in osfmk/console/serial_general.c never starts and never drains the
// FIFO ahead of this thread. Output is unaffected: serial=1 still selects the UART console.
constexpr int kSerialResetByte = 0x1d;
static char gSerialResetChannel;        // wait channel; only its address is used

static void
native_sd_serial_reset_thread(void *, wait_result_t)
{
	for (;;) {
		int c;
		// Drain the FIFO every period, and test the key inside the drain: a byte left behind would wait a
		// full period for the next pass, and the key is the one byte that must never be the leftover.
		while ((c = serial_getc()) >= 0) {
			if (c == kSerialResetByte) {
				IOLog("opi5plus: serial reset key, resetting\n");
				panic("opi5plus: serial reset key");
			}
		}
		// Block, do not poll with IOSleep(): this thread runs for the life of the boot, and IOSleep() here
		// would busy-wait a core at 100%.
		assert_wait_timeout((event_t)&gSerialResetChannel, THREAD_UNINT, 100, kMillisecondScale);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

bool NativeSd::start(IOService *provider) {
	if (!IOService::start(provider)) return false;

	// The reset hook is started before the sd=0 early return below, so that even a boot that publishes no
	// device (sd=0) leaves the UART key working as the way out.
	{
		thread_t thread = THREAD_NULL;
		const kern_return_t started = kernel_thread_start(native_sd_serial_reset_thread, nullptr, &thread);
		if (started == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			IOLog("opi5plus: serial reset thread did not start (%d)\n", started);
		}
	}

	const uint32_t mode = bootArg("sd", 1);
	native_sd_root_trust = bootArg("sd-root-trust", 0) ? 1 : 0;
	IOLog("NativeSd: mode sd=%u%s\n", mode, native_sd_root_trust ? ", sd-root-trust=1" : "");
	if (mode == 0) return true;

	mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
	if (!mapping || mapping->getLength() < 0x1000) {
		IOLog("NativeSd: no register window from the provider\n");
		return false;
	}
	void *registers = reinterpret_cast<void *>(mapping->getVirtualAddress());
	IOLog("NativeSd: registers at %p, %llu bytes\n", registers,
	      ull(static_cast<uint64_t>(mapping->getLength())));

	host.attach(&kPlatform, registers);
	gate = IOLockAlloc();
	if (!gate) return false;

	host.dump("firmware state");
	// sd-reset=1 asks for the controller to be reset before probing. It is off by default, and that default
	// is the lesson of the 2026-11-02 board run: the firmware hands over a live, powered, clocked card, and
	// CTRL_RESET_ALL drops PWREN -- so a reset made every CMD13 go unanswered (`rca=0 ... cmd=0`,
	// "the card did not answer"). The reset path is still available for a board whose firmware leaves the
	// controller in a state the gentle path cannot handle.
	const bool softReset = bootArg("sd-reset", 0) != 0;
	if (softReset) {
		IOLog("NativeSd: sd-reset=1, resetting the controller before probing\n");
	}
	const int identified = host.identify(softReset);
	if (identified != sd::kOk) {
		IOLog("NativeSd: the card did not answer (%s); nothing published\n", sd::resultName(identified));
		return false;
	}
	const sd::Geometry geometry = host.geometry();
	native_sd_rca = geometry.rca;
	native_sd_geo_blocks = geometry.blockCount;
	IOLog("NativeSd: card rca %u, %u blocks (%llu MiB), %s\n", geometry.rca, geometry.blockCount,
	      ull((static_cast<uint64_t>(geometry.blockCount) * sd::kBlockBytes) >> 20),
	      geometry.highCapacity ? "SDHC" : "SDSC");

	if (!scanGpt()) return false;
	// The card's own capacity claim and the partition table have to agree before this driver cuts any
	// window out of the medium, and on this board they do not (see Host::adoptCapacity): the GPT is
	// self-validating and the CSD decode is not, and the GPT is also what the windows are cut from.
	if (diskBlocks > 0 && diskBlocks <= 0xffffffffull) {
		host.adoptCapacity(static_cast<uint32_t>(diskBlocks));
	}
	// sd-clock-div=N overrides the firmware's card-clock divider (0 = no division). The userland's shared
	// cache is a multi-GiB read through this driver, so the clock is worth a knob.
	{
		const uint32_t divider = static_cast<uint32_t>(bootArg("sd-clock-div", 0xffffffffu));
		if (divider != 0xffffffffu) {
			const int clocked = host.setClockDivider(divider);
			IOLog("NativeSd: sd-clock-div=%u -> %s\n", divider, sd::resultName(clocked));
			// One line, once: what the clock registers actually hold after the update command landed.
			// CLKDIV=0 -> no division; anything else is the divider the card clock really runs at.
			IOLog("opi5plus: sd clock: CLKDIV=%x CLKSRC=%x CLKENA=%x\n", native_sd_clkdiv,
			      native_sd_clksrc, native_sd_clkena);
		}
	}
	const sd::Geometry &effective = host.geometry();
	native_sd_geo_blocks = effective.blockCount;
	native_sd_capacity_from_gpt = effective.capacityFromGpt ? 1 : 0;
	if (effective.capacityFromGpt) {
		IOLog("NativeSd: capacity now %u blocks (%llu MiB) from the GPT; the CSD's own reading was %u blocks\n",
		      effective.blockCount,
		      ull((static_cast<uint64_t>(effective.blockCount) * sd::kBlockBytes) >> 20), geometry.blockCount);
	}
	native_sd_ready = 1;
	probePartition();

	IOLog("NativeSd: %llu reads / %llu blocks / %llu commands / %llu timeouts, %llu block requests, %llu failures\n",
	      ull(native_sd_read_calls), ull(native_sd_read_blocks), ull(native_sd_commands),
	      ull(native_sd_timeouts), ull(native_sd_block_requests), ull(native_sd_block_failures));

	if (mode == 2) {
		publishWindow("SD", 0, diskBlocks);
	} else if (mode == 3) {
		if (partFound) {
			publishWindow("SD", partFirst, partLast - partFirst + 1);
		} else {
			IOLog("NativeSd: sd=3 but the partition was not found; nothing published\n");
		}
	} else if (mode == 4) {
		// Windows go out one at a time and the OS window last (RootManifest.h buildRootPlan): the kernel
		// mounts the root volume as soon as it appears, then looks for SystemOS, and publishing in the
		// plan's order is what makes those two steps happen in that order.
		publishRootWindows();
	}
	// Persistent Data on the card first (sd-data-persist=1): a window at the end of userdata.
	if (bootArg("sd-data-persist", 0) && mode == 4 && rootFound) {
		if (!persistentData()) {
			IOLog("NativeSd: data-persist failed; will try the RAM disk if asked\n");
			host.clearWriteRange();
		}
	}
	// Independent of the card: only a payload that carries a Data seed asks for it.
	if (!gDataWindow && bootArg("sd-data-ram", 0)) publishDataRamDisk();
	registerService();
	return true;
}

void NativeSd::stop(IOService *provider) { IOService::stop(provider); }

void NativeSd::free() {
	if (mapping) {
		mapping->release();
		mapping = nullptr;
	}
	if (gate) {
		IOLockFree(gate);
		gate = nullptr;
	}
	IOService::free();
}
