// SdHost.h -- polling dw_mmc host core for the RK3588 SD controller (mmc@fe2c0000).
//
// This is the half of M4 that cannot be reused from Rk3588Emmc: that driver speaks SDHCI (dwcmshc,
// registers at 0x00 DMA address / 0x04 block size / 0x0e command), while the RK3588 SD slot is a
// Synopsys DesignWare mobile-storage host (dw_mmc: 0x000 CTRL / 0x02c CMD / 0x044 RINTSTS / 0x200 FIFO).
// The two are different register protocols, not different parameters.
//
// Everything above this file -- GPT parsing, root window publishing, the Data volume, the kernel hooks --
// is reused verbatim from Rk3588Emmc, which is why this core only has to do five things:
//
//   * find the card the firmware already initialised (CMD13 probe over RCA 1..4, no CMD0/CMD8/ACMD41),
//   * read blocks (CMD17 single, CMD18 multi) through the FIFO,
//   * write blocks (CMD24 single, CMD25 multi) plus the occasional erase,
//   * report geometry (CSD/SCR: block count, erase size, write-protect),
//   * survive errors: reset the controller, retry, never hang.
//
// Design decisions that follow from the hardware and from how EmmcHost.h earned its trust:
//
//   * PIO only. The firmware drives the same card through PIO (DwMmcCreateTrb sets UseFifo for SD), FIFO
//     depth is 256 words = 1 KiB per burst, and at 52 MHz 4-bit the ceiling is ~26 MB/s, so PIO's
//     expected 8-12 MB/s is not the bottleneck for a boot that reads tens of megabytes. IDMAC
//     (BMOD/DBADDR/IDSTS) is left out on purpose; adding it later does not change this interface.
//   * Polling only. RINTSTS is polled and INTMASK stays fully open, mirroring the hard-won lesson in
//     EmmcHost.h:29 -- masking interrupts silences the status bits and every command then "times out".
//   * No clock, no phase, no IO-mux, no card-detect setup. The firmware (DwMmcHcDxe + RkSdmmcDxe) already
//     selected 52 MHz / 4-bit and set the CRU phases; §3.6 of docs/research/m4-storage.md records that
//     RkSdmmcDxe itself contains no tuning code, it relies on the SoC's power-on defaults. We inherit a
//     live card, exactly as EmmcHost.h inherits a live eMMC.
//   * Platform indirection. Every register access, delay, clock and log call goes through Platform, so
//     tests can link this file against a model of the controller (see tests/test-sd-host.cpp).
//
// Controller facts this code depends on, all verified against the RK3588 sources in this tree:
//
//   * Register window is 0x4000 (16 KiB), not 0x400000: board DTS `reg = <0x00 0xfe2c0000 0x00 0x4000>`.
//   * FIFO is at 0x200, depth 0x100 words; FIFOTH wants SET_FIFOTH(0x2, 127, 128).
//   * RINTSTS bits are write-one-to-clear; reading STATUS bit 9 (DATA_BUSY) says whether the card is
//     still programming.
//   * CMD exposes its index in bits 5:0 and START in bit 31; a command is only accepted while STATUS's
//     command FSM is idle, so every send waits for that first.
//   * The card's RCA is whichever the firmware assigned; we find it by probing, never by assuming 1.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Read-only diagnostics, defined in NativeSd.cpp alongside the other counters: the clock registers as they
// stand right after the last clock update, and the microseconds the PIO read path spends in each of its two
// phases. Written from setClockDivider() and the read loop below; read by the one-time clock line and the
// periodic swap-ok line. Counting only -- nothing here changes what those loops do.
extern "C" volatile uint32_t native_sd_clkdiv, native_sd_clksrc, native_sd_clkena;
extern "C" volatile uint64_t native_sd_fill_us, native_sd_drain_us;

namespace sd {

constexpr uint32_t kBlockBytes = 512;

// ---------------------------------------------------------------- registers --
// Synopsys DesignWare mobile-storage host, as decoded in
// edk2-rockchip/Silicon/Synopsys/DesignWare/Drivers/DwMmcHcDxe/DwMmcHci.h:30-64.
constexpr uint32_t kRegCtrl = 0x000;
constexpr uint32_t kRegPwrEn = 0x004;

// PWREN (0x004): bit 0 applies card power. CTRL_RESET_ALL clears it and edk2 always rewrites it after a
// reset (DwMmcHci.c:515); the first board run showed what happens when we do not — CMD13 gets no answer.
constexpr uint32_t kPwrEnEnable = 1u << 0;
constexpr uint32_t kRegClkDiv = 0x008;
constexpr uint32_t kRegClkSrc = 0x00c;
constexpr uint32_t kRegClkEna = 0x010;
constexpr uint32_t kRegTimeout = 0x014;
constexpr uint32_t kRegCType = 0x018;
constexpr uint32_t kRegBlkSiz = 0x01c;
constexpr uint32_t kRegBytCnt = 0x020;
constexpr uint32_t kRegIntMask = 0x024;
constexpr uint32_t kRegCmdArg = 0x028;
constexpr uint32_t kRegCmd = 0x02c;
constexpr uint32_t kRegResp0 = 0x030;
constexpr uint32_t kRegResp1 = 0x034;
constexpr uint32_t kRegResp2 = 0x038;
constexpr uint32_t kRegResp3 = 0x03c;
constexpr uint32_t kRegRintSts = 0x044;
constexpr uint32_t kRegStatus = 0x048;
constexpr uint32_t kRegFifoTh = 0x04c;
constexpr uint32_t kRegDebnce = 0x064;
constexpr uint32_t kRegUsrId = 0x068;
constexpr uint32_t kRegVerId = 0x06c;
constexpr uint32_t kRegHCon = 0x070;
constexpr uint32_t kRegBMod = 0x080;
constexpr uint32_t kRegDbAddr = 0x088;
constexpr uint32_t kRegIdSts = 0x08c;
constexpr uint32_t kRegIdIntEn = 0x090;
constexpr uint32_t kRegCardThrCtl = 0x100;
constexpr uint32_t kRegFifo = 0x200;

// CTRL (0x000)
constexpr uint32_t kCtrlReset = 1u << 0;
constexpr uint32_t kCtrlFifoReset = 1u << 1;
constexpr uint32_t kCtrlDmaReset = 1u << 2;
constexpr uint32_t kCtrlIntEn = 1u << 4;
constexpr uint32_t kCtrlDmaEn = 1u << 5;
constexpr uint32_t kCtrlUseHoldReg = 1u << 22;
constexpr uint32_t kCtrlIdmacEn = 1u << 25;
constexpr uint32_t kCtrlEnableOd = 1u << 28;

// CLKENA (0x010): bit 0 enables the card clock, bit 16 is "update clock only" in the same write.
constexpr uint32_t kClkEnaEnable = 1u << 0;
constexpr uint32_t kClkEnaLowPower = 1u << 16;

// CMD (0x02c) bits, from DwMmcHci.h:88-105.
constexpr uint32_t kCmdStart = 1u << 31;
constexpr uint32_t kCmdUseHoldReg = 1u << 29;
constexpr uint32_t kCmdVoltSwitch = 1u << 28;
constexpr uint32_t kCmdCcsExpected = 1u << 23;
constexpr uint32_t kCmdUpdateClockOnly = 1u << 21;
constexpr uint32_t kCmdSendInit = 1u << 15;
constexpr uint32_t kCmdStopAbort = 1u << 14;
constexpr uint32_t kCmdWaitPrvDataComplete = 1u << 13;
constexpr uint32_t kCmdSendAutoStop = 1u << 12;
constexpr uint32_t kCmdStreamTransfer = 1u << 11;
constexpr uint32_t kCmdWrite = 1u << 10;
constexpr uint32_t kCmdDataExpected = 1u << 9;
constexpr uint32_t kCmdCheckResponseCrc = 1u << 8;
constexpr uint32_t kCmdLongResponse = 1u << 7;
constexpr uint32_t kCmdResponseExpect = 1u << 6;

// RINTSTS (0x044) / INTMASK (0x024), from DwMmcHci.h:106-124.
constexpr uint32_t kIntSdioInt = 1u << 16;
constexpr uint32_t kIntEbe = 1u << 15;   // end-bit error
constexpr uint32_t kIntAcd = 1u << 14;   // auto command done
constexpr uint32_t kIntSbe = 1u << 13;   // start-bit error
constexpr uint32_t kIntHle = 1u << 12;   // hardware-locked error
constexpr uint32_t kIntFrun = 1u << 11;  // FIFO under/overrun
constexpr uint32_t kIntHto = 1u << 10;   // data starvation by host timeout
constexpr uint32_t kIntDrt = 1u << 9;    // data read timeout
constexpr uint32_t kIntRto = 1u << 8;    // response timeout
constexpr uint32_t kIntDcrc = 1u << 7;   // data CRC error
constexpr uint32_t kIntRcrc = 1u << 6;   // response CRC error
constexpr uint32_t kIntRxdr = 1u << 5;   // receive FIFO data request
constexpr uint32_t kIntTxdr = 1u << 4;   // transmit FIFO data request
constexpr uint32_t kIntDto = 1u << 3;    // data transfer over
constexpr uint32_t kIntCmdDone = 1u << 2;
constexpr uint32_t kIntRe = 1u << 1;     // response error
constexpr uint32_t kIntCd = 1u << 0;     // card detect

constexpr uint32_t kDataErrorBits = kIntEbe | kIntSbe | kIntHle | kIntFrun | kIntHto |
                                    kIntDrt | kIntDcrc | kIntRcrc | kIntRe;
constexpr uint32_t kAllErrorBits = kDataErrorBits;

// STATUS (0x048)
constexpr uint32_t kStsFifoEmpty = 1u << 2;
constexpr uint32_t kStsFifoFull = 1u << 3;
constexpr uint32_t kStsDataBusy = 1u << 9;
constexpr uint32_t kStsDataStateMask = 0xfu << 8;
constexpr uint32_t kStsDataStateBusy = 0x4u << 8;
constexpr uint32_t kStsDataStateRecv = 0x6u << 8;
constexpr uint32_t kStsDataStateTrans = 0x2u << 8;
constexpr uint32_t kStsCmdFsmMask = 0xfu << 4;
constexpr uint32_t kStsFifoCountShift = 17;

// FIFOTH (0x04c): the firmware's SET_FIFOTH(0x2, 127, 128).
constexpr uint32_t kFifoThValue = (2u << 28) | (127u << 16) | 128u;

// HCON (0x070) bits we read for diagnostics only. In particular there is no "FIFO depth" field: dw_mmc
// reports FIFO occupancy in STATUS and FIFO_FULL in STATUS[3], and never reports the depth itself, which is
// why the PIO loops below gate on those two signals instead of on a computed capacity.
constexpr uint32_t kHConDmaInterface = 1u << 5;   // 1 = internal DMA controller (IDMAC) present
constexpr uint32_t kHConDataWidthShift = 0;       // 3 bits: log2 of the card data width

// PIO read batching. The board counters (round 5) priced the per-word shape exactly: 5.67 ms per 64 KiB
// chunk, of which 2.52 ms was bus time, 3.23 ms the 16,384 FIFO reads and 2.46 ms 12,484 STATUS polls that
// found 1.3 words each. So the chunk was CPU-bound on MMIO, not bus-bound. Waiting for the FIFO to hold a
// batch before draining turns ~1 STATUS poll per word into ~1 per batch; the drain itself still costs one
// MMIO read per word, and that is PIO's floor (~193 ns/word on the 52 MHz bus, i.e. ~26 MB/s).
//
// The watermark is 32 because round 8 measured both values. At 128 the CPU drains 128 words (128 * 193 ns =
// 24.7 us) and then waits for a whole batch to arrive (128 * 154 ns = 19.7 us), 44.4 us per 128 words and
// 5.68 ms per 64 KiB chunk. Dropping to 32 was meant to overlap the two phases: a 32-word drain costs
// 6.2 us, in which the card should put ~40 words in the FIFO, so the next batch would already be there and
// the chunk should fall to the CPU floor of about 3.16 ms (20.7 MB/s). The board does not agree. The
// counters show the new shape exactly -- bursts per chunk go from 121 to 449 -- but the chunk still costs
// 5.67 ms, i.e. 11.56 MB/s against the 11.52 MB/s it cost at 128. The batch size is therefore not what sets
// the chunk time; whatever sets it does not scale with the batch. The candidate the numbers favour is the card's own
// delivery rate of ~346 ns per 4-byte word, under which the drain hides completely at any batch size. The
// other candidate is the fill wait itself (kPioFillWaitUs), now paid 3.7 times as often per chunk; the two
// were not separated, because that would change a constant outside round 8's scope.
//
// Round 10 returns to 128 to make the base clock decidable: at 32 the kPioFillWaitUs=4 polling
// granularity can add up to ~2 ms of phantom fill per chunk, so the 14 MB/s and 26 MB/s line-rate
// hypotheses are indistinguishable; at 128 the granularity is <=4 us per burst and the ideal fill
// per burst cleanly separates 19.7 us (base 52 MHz -> 26 MB/s) from 39.4 us (base 26 MHz -> 13 MB/s).
constexpr uint32_t kPioReadBatchWords = 128;
// Spent between fill polls while the FIFO is below the watermark. At 26 MB/s the bus adds a word every
// ~155 ns, so a 128-word batch arrives in ~20 us; a few microseconds per poll keeps the poll count near one
// per batch without going blind to the refill. Platform::delayUs spins below 1 ms (NativeSd.cpp:221), so this
// is a busy wait on nowUs, not a timer-wheel sleep.
constexpr uint32_t kPioFillWaitUs = 4;

// ---------------------------------------------------------------- responses --
constexpr uint32_t kRespNone = 0;
constexpr uint32_t kRespShort = 1;   // 48-bit, CRC checked
constexpr uint32_t kRespLong = 2;    // 136-bit

// Command indices we issue. The set is deliberately small: the firmware did the identification.
constexpr uint8_t kCmdSendStatus = 13;   // ACMD13 is the SD status; plain CMD13 is the card status
constexpr uint8_t kCmdReadSingle = 17;
constexpr uint8_t kCmdReadMulti = 18;
constexpr uint8_t kCmdWriteSingle = 24;
constexpr uint8_t kCmdWriteMulti = 25;
constexpr uint8_t kCmdEraseStart = 32;
constexpr uint8_t kCmdEraseEnd = 33;
constexpr uint8_t kCmdErase = 38;
constexpr uint8_t kCmdAppCmd = 55;
constexpr uint8_t kCmdSendOpCond = 41;   // ACMD41, only used if the firmware left the card idle
constexpr uint8_t kCmdSendCsd = 9;
constexpr uint8_t kCmdSendCid = 10;
constexpr uint8_t kCmdStopTransmission = 12;

// Card status bits inside the R1 response (SD Physical Layer Simplified Spec 4.10, §4.10.1).
constexpr uint32_t kStatusCurrentStateMask = 0xfu << 9;
constexpr uint32_t kStatusStateTransfer = 0x4u << 9;
constexpr uint32_t kStatusStateProgram = 0x7u << 9;
constexpr uint32_t kStatusReadyForData = 1u << 8;
constexpr uint32_t kStatusCardIsLocked = 1u << 25;
constexpr uint32_t kStatusAppCmd = 1u << 5;

// ---------------------------------------------------------------- timeouts ---
constexpr uint32_t kCommandTimeoutUs = 100 * 1000;
constexpr uint32_t kTransferTimeoutUs = 5 * 1000 * 1000;
constexpr uint32_t kProgramTimeoutUs = 5 * 1000 * 1000;   // the card's garbage collection can be slow
constexpr uint32_t kResetTimeoutUs = 100 * 1000;
// CMD12 to end a transfer the firmware left running. Short: the card either answers at once or is not
// there at all, and identify() reports that separately.
constexpr uint32_t kAbortTimeoutUs = 50 * 1000;
constexpr uint32_t kPollIntervalUs = 50;

// Result codes: 0 is success, negative values are failures. Kept as ints rather than an enum so that
// callers can log the raw number the way EmmcHost.h's resultName() expects.
enum Result : int {
	kOk = 0,
	kErrArgument = -1,
	kErrTimeout = -2,
	kErrResponse = -3,
	kErrData = -4,
	kErrState = -5,
	kErrUnsupported = -6,
	kErrNoCard = -7,
	kErrRange = -8,
};

inline const char *resultName(int result) {
	switch (result) {
	case kOk: return "ok";
	case kErrArgument: return "bad argument";
	case kErrTimeout: return "timeout";
	case kErrResponse: return "response error";
	case kErrData: return "data error";
	case kErrState: return "wrong card state";
	case kErrUnsupported: return "unsupported";
	case kErrNoCard: return "no card";
	case kErrRange: return "outside write range";
	default: return "unknown";
	}
}

// ---------------------------------------------------------------- platform ----
// Everything the host core cannot do by itself. The kernel driver fills this in with MMIO accessors and
// IOKit time/log functions; the unit test fills it in with a controller model.
struct Platform {
	uint32_t (*read)(void *base, uint32_t offset, unsigned width);   // width 8 / 16 / 32
	void (*write)(void *base, uint32_t offset, unsigned width, uint32_t value);
	uint64_t (*nowUs)();
	void (*delayUs)(uint32_t microseconds);
	void (*log)(const char *format, ...) __attribute__((format(printf, 1, 2)));
};

struct Stats {
	uint64_t commands = 0;
	uint64_t commandErrors = 0;
	uint64_t blocksRead = 0;
	uint64_t blocksWritten = 0;
	uint64_t pioBursts = 0;
	uint64_t retries = 0;
	uint64_t recoveries = 0;
	uint64_t timeouts = 0;
	uint64_t writeBlocks = 0;   // blocks written and acknowledged by READY_FOR_DATA
};

// What we learned about the card. Filled by identify() and used by the layer above to size windows.
struct Geometry {
	bool valid = false;
	uint32_t rca = 0;
	uint32_t blockCount = 0;    // from CSD C_SIZE (SDHC/SDXC: (C_SIZE+1) << 10 blocks)
	uint32_t eraseBlocks = 0;   // units of kBlockBytes
	uint32_t readBlkLen = 0;
	bool highCapacity = false;
	bool writeProtected = false;
	bool sdhc = false;
	// Set when blockCount came from the GPT instead of from the CSD (Host::adoptCapacity). A caller that
	// logs where the number came from has to be able to tell the two apart: on this board they disagree.
	bool capacityFromGpt = false;
};

// ---------------------------------------------------------------- the host ----
class Host {
public:
	Host() = default;
	Host(const Host &) = delete;
	Host &operator=(const Host &) = delete;
	// Movable so that a caller can build a Host in a helper and hand it back. The members are all plain
	// values plus one borrowed pointer, so the default move is correct.
	Host(Host &&) = default;
	Host &operator=(Host &&) = default;

	void attach(const Platform *platform, void *registers) {
		plat_ = platform;
		base_ = registers;
	}

	uint32_t rca() const { return rca_; }
	bool usable() const { return usable_; }
	const Geometry &geometry() const { return geo_; }
	const Stats &stats() const { return stats_; }
	void resetStats() { stats_ = Stats(); }

	// Probe for a card the firmware already brought up. Returns kOk when a card answers CMD13 in the
	// transfer state. This never re-runs identification: no CMD0, no CMD8, no ACMD41 unless the card turns
	// out to be idle (which would mean the firmware did not finish, and is reported rather than papered over).
	//
	// `softReset` defaults to false, and that default is the whole lesson of the first board run. The
	// firmware hands over a controller that is mid-transfer with the card powered and clocked; resetting it
	// (CTRL_RESET_ALL) drops PWREN and the card stops answering, which is exactly what happened on
	// 2026-11-02: `rca=0 blocks=0 ... cmd=0` and "the card did not answer". A caller that wants the old
	// behaviour passes true, and then `restorePower()` must be able to recover it.
	int identify(bool softReset = false);

	// Bulk I/O. Reads and writes are in units of kBlockBytes and must stay inside geometry().blockCount.
	int readBlocks(uint64_t lba, uint32_t count, void *out);
	int writeBlocks(uint64_t lba, uint32_t count, const void *in);
	int flushCache();

	// Adopt a capacity that did not come from the CSD. Returns kOk when the bound is now at least
	// `blockCount`, kErrState before identify() has vouched for the card.
	//
	// The CSD is the spec's capacity source and this core decodes it, but on the board (2026-11-03) the
	// CMD9 response did not vouch for itself: no byte order passed the CRC7/end-bit/structure test, the
	// fallback reading called a 30 GiB SDHC card a 96 MiB SDSC one, and every block read through the same
	// controller was still perfect. The GPT is the second opinion, and a better one for addressing: its
	// header and entry array carry CRC-32s over exactly the blocks the controller just delivered, and
	// alt_lba is the last LBA of the medium that table was written on. It is also the table the windows
	// are cut from, so when the two disagree it is the GPT that has to be believed.
	//
	// The bound is only ever raised, and this core has no write path, so an over-large bound cannot
	// damage the medium: a read past the card's real end fails at the card and is reported as an error.
	int adoptCapacity(uint32_t blockCount);
	// The one place the driver lets itself write: [first, last]. The host core refuses
	// every write outside this range (EVB10 NativeEmmc::enableWrites port).
	bool setWriteRange(uint64_t first, uint64_t last);
	void clearWriteRange();
	bool writable() const { return writeOpen_; }

	// Diagnostics: dump the register file, and report the last status word the FSM produced.
	void dump(const char *tag);
	uint32_t lastStatus() const { return lastStatus_; }

	// The card clock is the firmware's divider (CLKDIV) and that is what limits this port: the shared cache of
	// the userland is a multi-GiB read, and at the hardware's own bus width the divider is the only knob left
	// that costs nothing to try. 0 means "no division". Returns kOk when the clock-update command went out.
	int setClockDivider(uint32_t divider);

private:
	// --- register access -------------------------------------------------------
	uint32_t rd32(uint32_t offset) const { return plat_->read(base_, offset, 32); }
	void wr32(uint32_t offset, uint32_t value) const { plat_->write(base_, offset, 32, value); }

	// HiWord update: dw_mmc latches bits [31:16] as the mask and [15:0] as the value when the register is
	// written this way, which avoids a read-modify-write race with the controller. Used for CTYPE/CLKENA.
	void update32(uint32_t offset, uint32_t value, uint32_t mask) const {
		wr32(offset, ((value & mask) | (mask << 16)));
	}

	// --- low level -------------------------------------------------------------
	int waitCommandIdle(uint32_t timeoutUs) const;
	int waitDataIdle(uint32_t timeoutUs) const;
	int resetAll();
	// Restore the parts of the controller that CTRL_RESET_ALL clears but that a driver still needs: card
	// power (PWREN) and, when the divider is changed, a clock-update command. Separate from resetAll()
	// because it is also useful on its own after an error recovery.
	int restorePower();
	int updateClock();
	// Abort an in-flight data transfer the firmware left behind (CMD12), so that the card returns to the
	// transfer state without a controller reset. Best effort: a card already idle answers an error and that
	// is fine.
	int abortTransfer();
	int sendCommand(uint8_t index, uint32_t argument, uint32_t responseType, uint32_t *response,
	                uint32_t timeoutUs = kCommandTimeoutUs);
	int sendAppCommand(uint8_t index, uint32_t argument, uint32_t *response,
	                   uint32_t timeoutUs = kCommandTimeoutUs);
	int cardStatus(uint32_t *status);
	int waitReadyForData(uint32_t timeoutUs);
	int clearInterrupts();

	// --- data path -------------------------------------------------------------
	int transferPio(bool write, uint64_t lba, uint32_t count, void *buffer);
	int readCsd(Geometry &geometry);

	// --- error handling --------------------------------------------------------
	void recover(const char *why);
	bool logBudget() { return errorLogs_++ < 24; }

	const Platform *plat_ = nullptr;
	void *base_ = nullptr;
	uint32_t rca_ = 0;
	bool usable_ = false;
	// No bus-width member on purpose: the firmware already selected the card's bus width, and this
	// core never writes CTYPE. Keeping a copy of a value we neither set nor read would only invite
	// someone to "restore" it.
	uint32_t hcon_ = 0;
	Geometry geo_{};
	Stats stats_{};
	uint32_t lastStatus_ = 0;
	uint64_t writeFirst_ = 0, writeLast_ = 0;
	bool writeOpen_ = false;
	uint32_t errorLogs_ = 0;
};

// -----------------------------------------------------------------------------
// Implementation. Header-only for the same reason EmmcHost.h is: the unit test links it directly, and
// keeping it in one file makes the register protocol readable end to end.
// -----------------------------------------------------------------------------

inline int Host::clearInterrupts() {
	// RINTSTS is write-one-to-clear; write back whatever is pending.
	uint32_t pending = rd32(kRegRintSts);
	if (pending) {
		wr32(kRegRintSts, pending);
	}
	return kOk;
}

inline int Host::waitCommandIdle(uint32_t timeoutUs) const {
	uint64_t deadline = plat_->nowUs() + timeoutUs;
	while (true) {
		uint32_t status = rd32(kRegStatus);
		if ((status & kStsCmdFsmMask) == 0) {
			return kOk;
		}
		if (plat_->nowUs() >= deadline) {
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}
}

inline int Host::waitDataIdle(uint32_t timeoutUs) const {
	uint64_t deadline = plat_->nowUs() + timeoutUs;
	while (true) {
		uint32_t status = rd32(kRegStatus);
		if ((status & kStsDataBusy) == 0) {
			return kOk;
		}
		if (plat_->nowUs() >= deadline) {
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}
}

inline int Host::restorePower() {
	// CTRL_RESET_ALL clears the card-side state machine *and* drops card power. edk2's
	// DwMmcHcInitPowerVoltage (DwMmcHci.c:508-527) always re-writes PWREN after a reset for exactly this
	// reason; the first board run proved it matters, because after our reset CMD13 got no answer at all
	// (rca=0, cmd=0, "the card did not answer").
	wr32(kRegPwrEn, kPwrEnEnable);
	// Give the card a moment to come back; the SD spec wants 1 ms after VDD is applied before the first
	// clock edges and 74 clocks after that. The clock is already running from the firmware's setup, so the
	// 74 clocks are already in flight and only the supply settle time is ours to observe.
	plat_->delayUs(2000);
	return kOk;
}

inline int Host::setClockDivider(uint32_t divider) {
	if (!plat_ || !base_) {
		return kErrArgument;
	}
	wr32(kRegClkDiv, divider & 0xffu);
	const int error = updateClock();
	// Read back what the registers actually hold once the update command has landed (or failed): the CLKDIV
	// in the boot log is a firmware dump from before the driver took over, so only this answers "which card
	// clock really ran". Stored for the one-time line in NativeSd; no other effect.
	native_sd_clkdiv = rd32(kRegClkDiv);
	native_sd_clksrc = rd32(kRegClkSrc);
	native_sd_clkena = rd32(kRegClkEna);
	return error;
}

inline int Host::updateClock() {
	// dw_mmc latches a new CLKDIV into the card clock only when a clock-update command runs. edk2 does this
	// in DwMmcHcUpdateClock (DwMmcHci.c:214-246) after every divider change; we do it here so that a caller
	// who touches CLKDIV has a supported way to make it take effect. The command needs no argument and
	// produces no response.
	if (!plat_ || !base_) {
		return kErrArgument;
	}

	int error = waitCommandIdle(kCommandTimeoutUs);
	if (error) {
		return error;
	}
	clearInterrupts();

	wr32(kRegCmd, kCmdWaitPrvDataComplete | kCmdUpdateClockOnly | kCmdStart);

	// The controller clears START when the update lands. A Hardware Locked Error means it did not.
	uint64_t deadline = plat_->nowUs() + kCommandTimeoutUs;
	while (true) {
		if ((rd32(kRegRintSts) & kIntHle) != 0) {
			stats_.commandErrors++;
			return kErrResponse;
		}
		if ((rd32(kRegCmd) & kCmdStart) == 0) {
			return kOk;
		}
		if (plat_->nowUs() >= deadline) {
			stats_.timeouts++;
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}
}

inline int Host::abortTransfer() {
	// CMD12 (STOP_TRANSMISSION) is accepted in the data/rcv state and returns an error in transfer, so
	// sending it unconditionally is the cheapest way to get back to a known state. No response is
	// *required* — some controllers report CMD_DONE without one — so the only wait that matters is that
	// the data FSM goes idle.
	if (!plat_ || !base_) {
		return kErrArgument;
	}
	if ((rd32(kRegStatus) & kStsDataStateMask) != kStsDataStateBusy &&
	    (rd32(kRegStatus) & kStsDataStateMask) != kStsDataStateRecv) {
		// Not mid-transfer; nothing to abort. Common case after the firmware finished cleanly.
		return kOk;
	}

	(void)sendCommand(kCmdStopTransmission, 0, kRespShort, nullptr, kAbortTimeoutUs);
	(void)waitDataIdle(kAbortTimeoutUs);
	clearInterrupts();
	return kOk;
}

inline int Host::resetAll() {
	// Reset the controller, the FIFO and the DMA engine, then wait for the bits to self-clear. The
	// controller clears each bit when the corresponding reset has completed, so a stuck bit is a real
	// failure and not something to retry forever.
	wr32(kRegCtrl, kCtrlReset | kCtrlFifoReset | kCtrlDmaReset);
	uint64_t deadline = plat_->nowUs() + kResetTimeoutUs;
	while (true) {
		uint32_t ctrl = rd32(kRegCtrl);
		if ((ctrl & (kCtrlReset | kCtrlFifoReset | kCtrlDmaReset)) == 0) {
			break;
		}
		if (plat_->nowUs() >= deadline) {
			if (logBudget()) {
				plat_->log("sd: controller reset did not complete, CTRL=0x%08x", ctrl);
			}
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}

	// Restore the conservative baseline: interrupts enabled (masking silences RINTSTS, see EmmcHost.h:29),
	// FIFO thresholds as the firmware left them, DMA off, and the card clock running.
	//
	// The PWREN restore is not optional and is the one thing the first board run taught the hard way:
	// CTRL_RESET_ALL also clears PWREN, and a card with no power answers nothing at all, which looks
	// exactly like "no card" (`rca=0 ... cmd=0`). edk2 never separates these two steps either
	// (DwMmcHci.c:508-527). restorePower() also waits for the supply to settle.
	wr32(kRegIntMask, 0xffffffffu);
	wr32(kRegFifoTh, kFifoThValue);
	wr32(kRegCtrl, kCtrlIntEn);
	update32(kRegClkEna, kClkEnaEnable, kClkEnaEnable);
	restorePower();
	clearInterrupts();
	return kOk;
}

inline int Host::sendCommand(uint8_t index, uint32_t argument, uint32_t responseType, uint32_t *response,
                             uint32_t timeoutUs) {
	if (!plat_ || !base_) {
		return kErrArgument;
	}

	int error = waitCommandIdle(kCommandTimeoutUs);
	if (error) {
		stats_.timeouts++;
		return error;
	}
	clearInterrupts();

	wr32(kRegCmdArg, argument);

	uint32_t command = (static_cast<uint32_t>(index) & 0x3fu) | kCmdStart | kCmdUseHoldReg;
	if (responseType == kRespShort) {
		command |= kCmdResponseExpect | kCmdCheckResponseCrc;
	} else if (responseType == kRespLong) {
		command |= kCmdResponseExpect | kCmdCheckResponseCrc | kCmdLongResponse;
	}
	wr32(kRegCmd, command);

	// Wait for CMD_DONE or a response error. The controller raises CMD_DONE even for commands without a
	// response, so this single wait covers both kinds.
	uint64_t deadline = plat_->nowUs() + timeoutUs;
	while (true) {
		uint32_t pending = rd32(kRegRintSts);
		if (pending & (kIntRe | kIntRcrc)) {
			wr32(kRegRintSts, pending);
			stats_.commandErrors++;
			lastStatus_ = pending;
			return kErrResponse;
		}
		if (pending & kIntCmdDone) {
			wr32(kRegRintSts, pending);
			break;
		}
		if (plat_->nowUs() >= deadline) {
			stats_.timeouts++;
			lastStatus_ = pending;
			wr32(kRegRintSts, pending);
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}

	stats_.commands++;

	if (response) {
		if (responseType == kRespLong) {
			// The long response arrives in RESP0..RESP3 in reverse word order; callers that need the CSD
			// read all four registers themselves.
			response[0] = rd32(kRegResp0);
			response[1] = rd32(kRegResp1);
			response[2] = rd32(kRegResp2);
			response[3] = rd32(kRegResp3);
		} else if (responseType == kRespShort) {
			*response = rd32(kRegResp0);
		} else {
			*response = 0;
		}
	}
	return kOk;
}

inline int Host::sendAppCommand(uint8_t index, uint32_t argument, uint32_t *response, uint32_t timeoutUs) {
	// CMD55 marks the next command as an application command for the addressed card.
	uint32_t status = 0;
	int error = sendCommand(kCmdAppCmd, static_cast<uint32_t>(rca_) << 16, kRespShort, &status, timeoutUs);
	if (error) {
		return error;
	}
	if ((status & kStatusAppCmd) == 0) {
		// The card is not in the transfer state; an ACMD now would be interpreted as a plain command.
		return kErrState;
	}
	return sendCommand(index, argument, kRespShort, response, timeoutUs);
}

inline int Host::cardStatus(uint32_t *status) {
	if (!status) {
		return kErrArgument;
	}
	return sendCommand(kCmdSendStatus, static_cast<uint32_t>(rca_) << 16, kRespShort, status);
}

inline int Host::waitReadyForData(uint32_t timeoutUs) {
	// After a write the controller's Transfer Complete means the FIFO drained, not that the card finished
	// programming. EmmcHost.h:37-41 records what happens if you assume otherwise: the card holds DAT0 low
	// in the PRG state and the next command is illegal. So poll CMD13 until READY_FOR_DATA is set.
	uint64_t deadline = plat_->nowUs() + timeoutUs;
	while (true) {
		uint32_t status = 0;
		int error = cardStatus(&status);
		if (error == kOk && (status & kStatusReadyForData)) {
			return kOk;
		}
		if (plat_->nowUs() >= deadline) {
			stats_.timeouts++;
			return kErrTimeout;
		}
		plat_->delayUs(200);
	}
}

// CRC7 over the first fifteen CSD bytes, compared against the sixteenth. The SD spec puts a 7-bit CRC
// with a zero end bit in CSD[7:1], which on the wire is the low seven bits of the last byte; the last
// byte's top bit is the always-1 end bit of the CSD.
inline bool csdCrc7Holds(const uint8_t *csd) {
	uint8_t crc = 0;
	for (unsigned byte = 0; byte < 15; ++byte) {
		uint8_t value = csd[byte];
		for (unsigned bit = 0; bit < 8; ++bit) {
			crc <<= 1;
			if ((value & 0x80u) != 0) {
				crc ^= 0x09u;
			}
			value = static_cast<uint8_t>(value << 1);
			crc &= 0x7fu;
		}
	}
	return crc == ((csd[15] >> 1) & 0x7fu);
}

// A CSD that arrived intact satisfies three things at once: the structure field names a layout the SD spec
// defines, the last byte ends on the always-1 end bit, and the CRC7 over the first fifteen bytes matches that
// last byte. All three are needed. The CRC on its own is weaker than it looks -- a byte-reversed reading very
// often checksums to zero and "matches" a zero-packed tail -- and the two-bit structure field on its own is
// almost no evidence at all. The end bit is what actually separates the two framings: byte 15 of a real CSD
// has bit 0 set, and byte 15 of the reversed reading is a payload byte, so it is set only by accident.
inline bool csdLooksIntact(const uint8_t *csd) {
	if (((csd[0] >> 6) & 0x3u) > 1) {
		return false;                    // not a structure version the SD spec defines
	}
	if ((csd[15] & 0x1u) == 0) {
		return false;                    // the end bit a real CSD always carries
	}
	return csdCrc7Holds(csd);
}

inline int Host::readCsd(Geometry &geometry) {
	uint32_t response[4] = {0, 0, 0, 0};
	int error = sendCommand(kCmdSendCsd, static_cast<uint32_t>(rca_) << 16, kRespLong, response);
	if (error) {
		return error;
	}

	// Where the 128-bit R2 payload starts inside RESP0..RESP3 is not the same on every DesignWare
	// integration, and getting it wrong is silent: on the board the "natural" reading decoded a 30727 MiB
	// SDHC card as CSD version 1.0 with a 24 MiB capacity, while every block read through the same
	// registers was perfect. So the byte order is decided by evidence rather than by assumption.
	//
	// The CSD carries a CRC7 in its last byte over the first fifteen, which makes the payload vouch for its
	// own layout. Two orders are worth testing: each register holding the next four CSD bytes most
	// significant first, or least significant first. (A one-byte *shift* cannot be tested this way -- on a
	// sixteen-byte frame a shift either discards the CRC byte or pushes it out of the window -- so shifts
	// are not among the candidates; if this board turns out to need one, the firmware already decoded the
	// card and the shift will have to be identified from the log line below.)
	const uint32_t raw[4] = {response[0], response[1], response[2], response[3]};

	uint8_t candidates[2][16];
	for (unsigned word = 0; word < 4; ++word) {
		for (unsigned byte = 0; byte < 4; ++byte) {
			candidates[0][word * 4 + byte] = static_cast<uint8_t>(raw[word] >> (24 - 8 * byte));
			candidates[1][word * 4 + byte] = static_cast<uint8_t>(raw[word] >> (8 * byte));
		}
	}

	// A candidate has to earn its place on all three counts described above. Checking them together is what
	// makes the search decisive instead of decorative.
	int chosen = -1;
	for (unsigned index = 0; index < 2; ++index) {
		if (csdLooksIntact(candidates[index])) {
			chosen = static_cast<int>(index);
			break;
		}
	}

	if (chosen < 0) {
		// Nothing self-validated. Prefer the order whose structure field is one the SD spec defines over one
		// that is not; otherwise keep the historical most-significant-first reading so behaviour only
		// changes when there is a reason for it to.
		for (unsigned index = 0; index < 2; ++index) {
			if (((candidates[index][0] >> 6) & 0x3u) <= 1) {
				chosen = static_cast<int>(index);
				break;
			}
		}
	}

	const uint8_t *csd = (chosen >= 0) ? candidates[chosen] : candidates[0];

	if (logBudget()) {
		plat_->log("sd: csd raw %08x %08x %08x %08x -> %s", raw[0], raw[1], raw[2], raw[3],
			(chosen == 0) ? "most-significant-first" : (chosen == 1) ? "least-significant-first"
																   : "unvalidated (as-is)");
	}

	// CSD bit 127 is the top bit of byte 0, so byte k covers CSD bits [127-8k : 120-8k]. Decoding from the
	// byte stream rather than from the words keeps the field definitions in one place per version.
	const uint32_t csdStructure = (csd[0] >> 6) & 0x3u;

	if (csdStructure == 1) {
		// CSD version 2.0 (SDHC/SDXC). C_SIZE is CSD[69:48]: the low six bits of byte 7 plus bytes 8 and 9.
		// Capacity = (C_SIZE + 1) * 512 KiB = (C_SIZE + 1) << 10 blocks.
		const uint32_t cSize = (static_cast<uint32_t>(csd[7] & 0x3fu) << 16) |
			(static_cast<uint32_t>(csd[8]) << 8) | static_cast<uint32_t>(csd[9]);
		geometry.sdhc = true;
		geometry.highCapacity = true;
		geometry.blockCount = (cSize + 1) << 10;
		geometry.readBlkLen = kBlockBytes;
		// Erase granularity for version 2.0 is (C_SIZE+1)/2 * 512 KiB worth of blocks; a conservative
		// 128-block unit keeps erase requests small enough to never straddle a region boundary.
		geometry.eraseBlocks = 128;
	} else if (csdStructure == 0) {
		// CSD version 1.0 (SDSC). Bit k of the CSD lives in byte (127-k)/8, at bit (127-k)%8, so:
		//   READ_BL_LEN  CSD[83:80] -> byte 5 bits 3..0
		//   C_SIZE       CSD[73:62] -> byte 6 bits 1..0 (high 2), byte 7 (middle 8), byte 8 bits 7..6
		//   C_SIZE_MULT  CSD[49:47] -> byte 9 bits 6..4 (bit 49 is offset 6 inside byte 9, because byte 9
		//   covers CSD[55:48] and 55-49 = 6)
		const uint32_t readBlkLen = csd[5] & 0x0fu;
		const uint32_t cSize = (static_cast<uint32_t>(csd[6] & 0x03u) << 10) |
			(static_cast<uint32_t>(csd[7]) << 2) | (static_cast<uint32_t>(csd[8]) >> 6);
		const uint32_t mult = 1u << (((csd[9] >> 4) & 0x7u) + 2);
		geometry.sdhc = false;
		geometry.highCapacity = false;
		geometry.readBlkLen = readBlkLen ? (1u << readBlkLen) : kBlockBytes;
		const uint64_t bytes = static_cast<uint64_t>(cSize + 1) * mult * geometry.readBlkLen;
		geometry.blockCount = static_cast<uint32_t>(bytes / kBlockBytes);
		geometry.eraseBlocks = 1;
	} else {
		if (logBudget()) {
			plat_->log("sd: unsupported CSD structure %u", csdStructure);
		}
		return kErrUnsupported;
	}

	geometry.valid = geometry.blockCount > 0;
	return geometry.valid ? kOk : kErrUnsupported;
}

inline int Host::transferPio(bool write, uint64_t lba, uint32_t count, void *buffer) {
	uint8_t *bytes = static_cast<uint8_t *>(buffer);
	uint32_t totalBytes = count * kBlockBytes;

	if (count == 0 || totalBytes / kBlockBytes != count) {
		return kErrArgument;
	}
	// lba is a byte-addressable block number and the controller's BYTCNT is 16 bits, so a run longer than
	// 65535 blocks would need BLKSIZ/BYTCNT tricks we do not use; reject it instead of silently truncating.
	if (count > 0xffffu) {
		return kErrArgument;
	}
	if (lba > 0xffffffffull) {
		return kErrArgument;
	}

	int error = waitDataIdle(kTransferTimeoutUs);
	if (error) {
		stats_.timeouts++;
		return error;
	}
	clearInterrupts();

	// Drain anything the previous command left behind, then program the block size and count.
	//
	// BLKSIZ/BYTCNT are 32-bit registers: the firmware writes them with MmioWrite32 (DwMmcHci.c:795-796),
	// and a 16-bit store would leave the upper half of BYTCNT holding whatever the last transfer set.
	//
	// The FIFO reset is not optional. DwMmcHci.c:1446-1456 resets the FIFO for every data transfer because a
	// previous command that ended in an error leaves stale words behind, and the first burst of this transfer
	// would then start mid-block.
	{
		wr32(kRegCtrl, rd32(kRegCtrl) | kCtrlFifoReset);
		uint64_t resetDeadline = plat_->nowUs() + kResetTimeoutUs;
		while ((rd32(kRegCtrl) & kCtrlFifoReset) != 0) {
			if (plat_->nowUs() >= resetDeadline) {
				stats_.timeouts++;
				return kErrTimeout;
			}
			plat_->delayUs(kPollIntervalUs);
		}
	}

	wr32(kRegBlkSiz, kBlockBytes);
	wr32(kRegBytCnt, count * kBlockBytes);
	wr32(kRegCmdArg, static_cast<uint32_t>(lba));

	// Multi-block transfers use CMD18/CMD25 with auto-stop; a single block uses CMD17/CMD24 without it.
	bool multi = count > 1;
	uint8_t index = write ? (multi ? kCmdWriteMulti : kCmdWriteSingle)
	                      : (multi ? kCmdReadMulti : kCmdReadSingle);

	uint32_t command = (static_cast<uint32_t>(index) & 0x3fu) | kCmdStart | kCmdUseHoldReg |
	                   kCmdResponseExpect | kCmdCheckResponseCrc | kCmdDataExpected |
	                   kCmdWaitPrvDataComplete;
	command |= write ? kCmdWrite : 0;
	command |= multi ? kCmdSendAutoStop : 0;

	uint64_t deadline = plat_->nowUs() + kTransferTimeoutUs;
	wr32(kRegCmd, command);

	// Wait for the command to be accepted before touching the FIFO.
	while (true) {
		uint32_t pending = rd32(kRegRintSts);
		if (pending & kAllErrorBits) {
			wr32(kRegRintSts, pending);
			stats_.commandErrors++;
			lastStatus_ = pending;
			return kErrResponse;
		}
		if (pending & kIntCmdDone) {
			wr32(kRegRintSts, kIntCmdDone);
			break;
		}
		if (plat_->nowUs() >= deadline) {
			stats_.timeouts++;
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}

	// Move the payload through the FIFO.
	//
	// STATUS[28:17] counts *filled* locations, not free ones (DwMmcHci.h:160 "Number of filled locations in
	// FIFO"), so the two directions need different predicates, and the firmware's own PIO path shows why:
	// TransferFifo pushes words while FIFO_FULL is clear for writes, and for reads drains exactly the filled
	// count. Using the count as free space writes nothing when the FIFO is full and overruns when it is empty.
	//
	// Neither loop assumes a FIFO depth. dw_mmc does not expose one; the firmware keys off FIFO_FULL and the
	// occupancy count for the same reason, so a driver that batches by a guessed depth breaks on a shallower
	// FIFO than the guess.
	uint32_t doneBytes = 0;
	uint32_t words = totalBytes / 4;

	if (write) {
		// Word at a time, re-reading STATUS before each store, exactly as the firmware's PIO transmitter does
		// (DwMmcHci.c:880-902 re-reads STATUS inside the inner loop). This is capacity-agnostic on purpose:
		// dw_mmc never reports its FIFO depth in a register we can trust, so the only safe predicate for
		// "may I push another word" is FIFO_FULL itself. Batching by an assumed depth overruns a shallower
		// FIFO than the guess.
		while (words > 0) {
			uint32_t pending = rd32(kRegRintSts);
			if (pending & kAllErrorBits) {
				wr32(kRegRintSts, pending);
				stats_.commandErrors++;
				lastStatus_ = pending;
				return kErrData;
			}
			if (rd32(kRegStatus) & kStsFifoFull) {
				if (plat_->nowUs() >= deadline) {
					stats_.timeouts++;
					return kErrTimeout;
				}
				continue;
			}
			const uint8_t *source = bytes + doneBytes;
			uint32_t word = static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8) |
			                (static_cast<uint32_t>(source[2]) << 16) | (static_cast<uint32_t>(source[3]) << 24);
			wr32(kRegFifo, word);
			doneBytes += 4;
			words -= 1;
			stats_.pioBursts++;
		}
	} else {
		// Two phases per burst, because the board counters say the old per-word shape was CPU-bound, not
		// bus-bound: draining the moment STATUS showed a single word meant 12,484 polls for 16,384 words.
		//
		// Phase 1 watches the FIFO fill, phase 2 drains the batch without touching STATUS in between. The
		// invariant is unchanged: every batch re-checks the error bits and clears RINTSTS, a DTO with an empty
		// FIFO still ends the transfer normally, the shared deadline still bounds the whole loop, and
		// pioBursts still counts one drain. Only the polling granularity changes -- one STATUS read per batch
		// instead of one per word.
		//
		// Phase timing (round 9): how much of the read path is waiting for the FIFO to fill against the
		// card clock (bus-limited) and how much is draining it word by word (CPU-limited). Three counter
		// reads per batch, accumulated into native_sd_fill_us / native_sd_drain_us; nothing else changes.
		uint64_t tickFrequency = 0;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(tickFrequency));
		while (words > 0) {
			uint32_t pending = rd32(kRegRintSts);
			if (pending & kAllErrorBits) {
				wr32(kRegRintSts, pending);
				stats_.commandErrors++;
				lastStatus_ = pending;
				return kErrData;
			}

			// A full batch, or everything that is left when the transfer is shorter than one batch.
			uint32_t wanted = (words < kPioReadBatchWords) ? words : kPioReadBatchWords;
			uint32_t filledWords = 0;
			bool endOfData = false;
			uint64_t fillStart = 0;
			__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(fillStart));

			// Phase 1: let the FIFO fill.
			while (true) {
				filledWords = (rd32(kRegStatus) >> kStsFifoCountShift) & 0x1fffu;
				if (filledWords >= wanted) {
					break;
				}
				// Below the watermark the card may still be feeding us, so the bits that can change the plan
				// are re-read here instead of once per batch: errors abort, DTO decides whether to drain.
				pending = rd32(kRegRintSts);
				if (pending & kAllErrorBits) {
					wr32(kRegRintSts, pending);
					stats_.commandErrors++;
					lastStatus_ = pending;
					return kErrData;
				}
				if (pending & kIntDto) {
					// Data transfer over. Empty FIFO: the card sent less than we asked for, so fall through to
					// the completion wait exactly as before and let it report the mismatch. Non-empty: these
					// are the last words of the transfer, drain them rather than wait for a batch that will
					// never arrive.
					if (filledWords == 0) {
						endOfData = true;
					}
					break;
				}
				if (plat_->nowUs() >= deadline) {
					stats_.timeouts++;
					return kErrTimeout;
				}
				// Pause instead of spinning: a poll costs one MMIO read (~197 ns here) and would find the
				// FIFO barely fuller, which is the shape that produced 1.3 words per poll. delayUs spins
				// below 1 ms (NativeSd.cpp:221), so this does not sleep the thread or the timer.
				plat_->delayUs(kPioFillWaitUs);
			}
			uint64_t fillEnd = 0;
			__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(fillEnd));
			if (tickFrequency != 0) {
				native_sd_fill_us = native_sd_fill_us + (fillEnd - fillStart) * 1000000ull / tickFrequency;
			}
			if (endOfData) {
				break;
			}
			if (filledWords > words) {
				filledWords = words;
			}
			// Phase 2: drain the batch, one MMIO read per word and no STATUS read in between.
			for (uint32_t i = 0; i < filledWords; ++i) {
				uint32_t word = rd32(kRegFifo);
				uint8_t *target = bytes + doneBytes + i * 4;
				target[0] = static_cast<uint8_t>(word & 0xff);
				target[1] = static_cast<uint8_t>((word >> 8) & 0xff);
				target[2] = static_cast<uint8_t>((word >> 16) & 0xff);
				target[3] = static_cast<uint8_t>((word >> 24) & 0xff);
			}
			uint64_t drainEnd = 0;
			__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(drainEnd));
			if (tickFrequency != 0) {
				native_sd_drain_us = native_sd_drain_us + (drainEnd - fillEnd) * 1000000ull / tickFrequency;
			}
			doneBytes += filledWords * 4;
			words -= filledWords;
			stats_.pioBursts++;
		}
	}

	// Data transfer over, plus the auto-stop completion for multi-block runs.
	while (true) {
		uint32_t pending = rd32(kRegRintSts);
		if (pending & kAllErrorBits) {
			wr32(kRegRintSts, pending);
			stats_.commandErrors++;
			lastStatus_ = pending;
			return kErrData;
		}
		bool haveData = (pending & kIntDto) != 0;
		bool haveStop = !multi || (pending & kIntAcd) != 0;
		if (haveData && haveStop) {
			wr32(kRegRintSts, pending);
			break;
		}
		if (plat_->nowUs() >= deadline) {
			stats_.timeouts++;
			return kErrTimeout;
		}
		plat_->delayUs(kPollIntervalUs);
	}

	if (write) {
		int ready = waitReadyForData(kProgramTimeoutUs);
		if (ready) {
			return ready;
		}
		stats_.blocksWritten += count;
		stats_.writeBlocks += count;
	} else {
		stats_.blocksRead += count;
	}
	return kOk;
}

inline int Host::identify(bool softReset) {
	if (!plat_ || !base_) {
		return kErrArgument;
	}

	hcon_ = rd32(kRegHCon);

	if (softReset) {
		int resetError = resetAll();
		if (resetError) {
			return resetError;
		}
	} else {
		// The gentle path, and the one that matches what the firmware actually leaves behind. Its last
		// command on the board was CMD18 with START already cleared and STATUS showing FSM_DATA, so the
		// card may still be streaming a block we never asked for.
		//
		// INTMASK=0 silences RINTSTS (EmmcHost.h:29) — it is a mask, so a zero there means no status bit
		// ever gets set, and every wait in this file would time out with a zero status word. Restore it and
		// drop the stale pending bits *before* the first command, otherwise the abort below cannot complete.
		wr32(kRegIntMask, 0xffffffffu);
		wr32(kRegCtrl, kCtrlIntEn);
		clearInterrupts();
		// CMD12 ends an in-flight transfer. Sent unconditionally: legal from data/rcv, a harmless error
		// from transfer, and it needs no state knowledge either way.
		abortTransfer();
	}

	// The firmware left the card in the transfer state. Find its RCA by asking; the assignment is the
	// firmware's, so probing beats assuming. 1..4 matches what the RK3588 BSP uses and what the SD spec
	// suggests for a single-card slot.
	uint32_t savedRca = rca_;
	for (uint32_t candidate = 1; candidate <= 4; ++candidate) {
		rca_ = candidate;
		uint32_t status = 0;
		if (cardStatus(&status) == kOk && (status & kStatusCurrentStateMask) == kStatusStateTransfer) {
			geo_.rca = candidate;
			const int csdError = readCsd(geo_);
			if (csdError) {
				geo_ = Geometry();
				continue;
			}
			geo_.writeProtected = (status & kStatusCardIsLocked) != 0;
			usable_ = true;
			return kOk;
		}
	}

	rca_ = savedRca;
	usable_ = false;
	geo_ = Geometry();
	return kErrNoCard;
}

// The capacity second opinion described at the declaration. The card keeps its CSD figures (they are what
// the format says they are); what changes is the bound every read is checked against. Both numbers are
// logged, because a driver that silently replaces one source of truth with another is worse than one that
// only ever had the wrong one.
inline int Host::adoptCapacity(uint32_t blockCount) {
	if (!usable_ || !geo_.valid) {
		return kErrState;
	}
	if (blockCount <= geo_.blockCount) {
		return kOk;  // never shrink a bound the CSD already proved
	}
	if (logBudget()) {
		plat_->log("sd: adopting the GPT capacity %u blocks; the CSD said %u", blockCount, geo_.blockCount);
	}
	geo_.blockCount = blockCount;
	geo_.capacityFromGpt = true;
	// A standard-capacity card tops out at 2 GiB by definition, so a medium larger than that cannot be
	// SDSC no matter what the undecodable CSD said. This matters beyond the log line: CMD17/CMD18 take a
	// block address on SDHC/SDXC and a byte address on SDSC, and mislabelling the card would move the
	// addressing mode with it.
	if (static_cast<uint64_t>(blockCount) * kBlockBytes > 0x80000000ull) {
		geo_.sdhc = true;
		geo_.highCapacity = true;
	}
	return kOk;
}

inline bool Host::setWriteRange(uint64_t first, uint64_t last) {
	if (!usable_ || first > last || last >= geo_.blockCount) return false;
	writeFirst_ = first;
	writeLast_ = last;
	writeOpen_ = true;
	return true;
}

inline void Host::clearWriteRange() {
	writeOpen_ = false;
}

inline int Host::readBlocks(uint64_t lba, uint32_t count, void *out) {	if (!usable_) {
		return kErrState;
	}
	if (!out || count == 0) {
		return kErrArgument;
	}
	if (!geo_.valid || lba + count > geo_.blockCount) {
		if (logBudget()) {
			plat_->log("sd: read out of range lba=%llu count=%u capacity=%u", static_cast<unsigned long long>(lba),
			           count, geo_.blockCount);
		}
		return kErrArgument;
	}
	return transferPio(false, lba, count, out);
}

inline int Host::writeBlocks(uint64_t lba, uint32_t count, const void *in) {
	if (!usable_) {
		return kErrState;
	}
	if (!writeOpen_ || count == 0 || lba < writeFirst_ || lba + count - 1 > writeLast_) {
		return kErrRange;
	}
	if (!in || count == 0) {
		return kErrArgument;
	}
	if (geo_.writeProtected) {
		return kErrState;
	}
	if (!geo_.valid || lba + count > geo_.blockCount) {
		if (logBudget()) {
			plat_->log("sd: write out of range lba=%llu count=%u capacity=%u", static_cast<unsigned long long>(lba),
			           count, geo_.blockCount);
		}
		return kErrArgument;
	}
	return transferPio(true, lba, count, const_cast<void *>(in));
}

inline int Host::flushCache() {
	if (!usable_) {
		return kErrState;
	}
	// A single-block read is a reliable "wait until the card accepts commands again" fence, and it does not
	// depend on CMD13 timing. The writes themselves already waited for READY_FOR_DATA.
	uint8_t probe[kBlockBytes];
	if (geo_.valid && geo_.blockCount > 0) {
		return readBlocks(0, 1, probe);
	}
	return kOk;
}

inline void Host::recover(const char *why) {
	stats_.recoveries++;
	if (logBudget()) {
		plat_->log("sd: recovering after %s (status=0x%08x)", why, lastStatus_);
	}
	resetAll();
}

inline void Host::dump(const char *tag) {
	if (!plat_ || !base_) {
		return;
	}
	plat_->log("sd: %s", tag ? tag : "registers");
	plat_->log("  CTRL=%08x PWREN=%08x CLKDIV=%08x CLKSRC=%08x CLKENA=%08x", rd32(kRegCtrl), rd32(kRegPwrEn),
	           rd32(kRegClkDiv), rd32(kRegClkSrc), rd32(kRegClkEna));
	plat_->log("  TMOUT=%08x CTYPE=%08x BLKSIZ=%08x BYTCNT=%08x INTMASK=%08x", rd32(kRegTimeout),
	           rd32(kRegCType), rd32(kRegBlkSiz), rd32(kRegBytCnt), rd32(kRegIntMask));
	plat_->log("  CMDARG=%08x CMD=%08x RINTSTS=%08x STATUS=%08x FIFOTH=%08x", rd32(kRegCmdArg), rd32(kRegCmd),
	           rd32(kRegRintSts), rd32(kRegStatus), rd32(kRegFifoTh));
	plat_->log("  HCON=%08x VERID=%08x USRID=%08x BMOD=%08x IDSTS=%08x", hcon_ ? hcon_ : rd32(kRegHCon),
	           rd32(kRegVerId), rd32(kRegUsrId), rd32(kRegBMod), rd32(kRegIdSts));
	plat_->log("  rca=%u blocks=%u erase=%u %s%s", rca_, geo_.blockCount, geo_.eraseBlocks,
	           geo_.sdhc ? "SDHC " : "SDSC ", geo_.writeProtected ? "RO" : "RW");
	plat_->log("  stats: cmd=%llu err=%llu rd=%llu wr=%llu bursts=%llu recoveries=%llu timeouts=%llu",
	           static_cast<unsigned long long>(stats_.commands),
	           static_cast<unsigned long long>(stats_.commandErrors),
	           static_cast<unsigned long long>(stats_.blocksRead),
	           static_cast<unsigned long long>(stats_.blocksWritten),
	           static_cast<unsigned long long>(stats_.pioBursts),
	           static_cast<unsigned long long>(stats_.recoveries),
	           static_cast<unsigned long long>(stats_.timeouts));
}

}  // namespace sd
