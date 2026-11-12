// Host-side test of ./SdHost.h against a model of the RK3588 dw_mmc controller and an SD card.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -O1 -I package/SdHost tests/test-sd-host.cpp -o /tmp/test-sd-host
//   /tmp/test-sd-host
//
// The model exists to enforce the register facts the driver depends on, so that a regression in the header
// shows up here instead of as silent corruption on the board:
//
//   * STATUS[28:17] counts *filled* FIFO locations. A write is only accepted while FIFO_FULL is clear; a read
//     may only take as many words as are filled. The model refuses the access that violates this the way the
//     controller would, by overrunning or starving.
//   * RINTSTS is write-one-to-clear, so a driver that forgets to clear CMD_DONE trips the stale-CMD_DONE check.
//   * BLKSIZ/BYTCNT are 32-bit registers; the model counts any narrower access, because a 16-bit store leaves
//     the upper half holding the previous transfer's count.
//   * Every data command needs a FIFO reset first (DwMmcHci.c:1446-1456); otherwise the previous transfer's
//     leftovers shift the payload.
//   * A data phase advances one block at a time, as the FIFO allows. Serving a whole run at once would hide the
//     accounting bug these tests exist for.
//   * After a write the card is in PRG and accepts only CMD13 until READY_FOR_DATA. Anything else is illegal.
//
// Fault injection covers the paths that matter on hardware: response timeout, response CRC error, data
// timeout, a card that never leaves PRG, and a read past the end of the medium.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <set>
#include <vector>

#include "SdHost.h"

namespace {

int failures = 0;
#define CHECK(condition)                                                                 \
	do {                                                                                 \
		if (!(condition)) {                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
			++failures;                                                                  \
		}                                                                                \
	} while (0)

// The FIFO is one block deep. That is the shallowest configuration the driver has to survive, and it is what
// makes an off-by-one in the occupancy accounting visible immediately.
constexpr uint32_t kFifoDepthWords = sd::kBlockBytes / 4;

// The CSD the model hands out: a v2.0 (SDHC) structure with a 64 GiB capacity, i.e. C_SIZE = 0xE93F so the
// card advertises (C_SIZE + 1) << 10 = 59,703,296 blocks. Fixed so the capacity assertions can be exact.
constexpr uint32_t kModelCSize = 0xE93Fu;

// CRC7 as the SD spec defines it: polynomial x^7 + x^3 + 1 (0x09), seven-bit result. The model uses it to
// stamp a valid CSD, and the driver uses the same relationship to work out which byte order it is looking at.
uint8_t csdCrc7(const uint8_t *csd) {
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
	return crc;
}

struct Model {
	// --- controller -----------------------------------------------------------
	uint8_t regs[0x4000] = {};
	uint32_t intStatus = 0;
	uint32_t response[4] = {0, 0, 0, 0};
	std::deque<uint32_t> fifo;
	bool commandBusy = false;
	bool dataBusy = false;
	uint32_t resetPending = 0;
	unsigned resetPolls = 0;
	uint64_t clockUs = 0;
	uint32_t blkSiz = 0;
	uint32_t bytCnt = 0;

	// --- card -----------------------------------------------------------------
	enum State { Idle, Ready, Ident, Stby, Tran, Data, Rcv, Prg, Dis };
	State state = Tran;
	uint32_t rca = 1;
	bool illegalPending = false;
	bool writeProtected = false;
	std::vector<uint8_t> image;
	unsigned prgPollsLeft = 0;
	unsigned programPolls = 2;

	// --- the data phase in flight ---------------------------------------------
	bool inRun = false;
	bool runIsWrite = false;
	bool pendingRefill = false;   // the card owes the FIFO the next block of a read run
	uint64_t runLba = 0;
	uint32_t runRemaining = 0;
	uint32_t runServed = 0;
	uint32_t runBlocks = 0;
	std::vector<uint8_t> runWriteBuffer;

	// --- observations ---------------------------------------------------------
	std::set<unsigned> commandsSeen;
	unsigned hostResets = 0;
	unsigned fifoResets = 0;
	unsigned narrowAccesses = 0;
	unsigned staleCmdDone = 0;
	unsigned cmdWhileBusy = 0;
	unsigned dataWithoutFifoReset = 0;
	unsigned fifoOverruns = 0;
	unsigned fifoUnderruns = 0;
	unsigned dataCommands = 0;
	unsigned illegalCommands = 0;
	bool fifoResetSinceDataCommand = true;
	uint64_t blocksRead = 0;
	uint64_t blocksWritten = 0;

	// --- card power -----------------------------------------------------------
	// dw_mmc's CTRL_RESET_ALL drops PWREN, and a card without power answers nothing. Modelled explicitly
	// because leaving it out is exactly how the driver shipped a version that could not talk to the board.
	bool powered = true;
	bool cardSilent = false;
	unsigned commandsWhileUnpowered = 0;

	// --- CSD byte order -------------------------------------------------------
	// Which end of each RESP register the CSD payload starts at is integration-specific, and the board
	// proved that guessing it wrong is silent: a 30727 MiB SDHC card decoded as a 24 MiB SDSC one while
	// every block read stayed correct. The model can emit either framing so the driver's CRC7-based search
	// has something to choose between.
	enum class CsdWordOrder { MostSignificantFirst, LeastSignificantFirst };
	CsdWordOrder csdWordOrder = CsdWordOrder::MostSignificantFirst;

	// --- fault injection ------------------------------------------------------
	unsigned failNoResponse = 0;
	unsigned failResponseCrc = 0;
	unsigned failDataTimeout = 0;
	unsigned failNeverReady = 0;

	uint32_t cardStatus() const {
		uint32_t status = static_cast<uint32_t>(state) << 9;
		if (state == Tran) {
			status |= sd::kStatusReadyForData;
		}
		if (illegalPending) {
			status |= 1u << 22;   // ILLEGAL_COMMAND
		}
		if (writeProtected) {
			status |= sd::kStatusCardIsLocked;
		}
		return status;
	}

	uint32_t rd(uint32_t offset) {
		switch (offset) {
		case sd::kRegStatus: {
			// Polling STATUS is the driver's observation point; the card refills the FIFO here so that what the
			// driver sees is always what it has actually asked for.
			serviceRefill();
			uint32_t status = 0;
			if (dataBusy) {
				status |= sd::kStsDataBusy;
			}
			if (fifo.size() >= kFifoDepthWords) {
				status |= sd::kStsFifoFull;
			}
			status |= static_cast<uint32_t>(fifo.size()) << sd::kStsFifoCountShift;
			if (commandBusy) {
				status |= 0x1u << 4;
			}
			return status;
		}
		case sd::kRegRintSts:
			return intStatus;
		case sd::kRegCtrl:
			return resetPending;
		case sd::kRegHCon:
			return sd::kHConDmaInterface;   // IDMAC present, as on RK3588; a bit mask, not a shift
		case sd::kRegFifo:
			return fifoPop();
		case sd::kRegResp0:
			return response[0];
		case sd::kRegResp1:
			return response[1];
		case sd::kRegResp2:
			return response[2];
		case sd::kRegResp3:
			return response[3];
		case sd::kRegBlkSiz:
			return blkSiz;
		case sd::kRegBytCnt:
			return bytCnt;
		default: {
			uint32_t value = 0;
			memcpy(&value, regs + offset, sizeof(value));
			return value;
		}
		}
	}

	void wr(uint32_t offset, unsigned width, uint32_t value) {
		if (width != 32) {
			++narrowAccesses;
			return;
		}
		switch (offset) {
		case sd::kRegCmd:
			issueCommand(value);
			return;
		case sd::kRegRintSts:
			intStatus &= ~value;   // write-one-to-clear
			return;
		case sd::kRegCtrl:
			if (value & sd::kCtrlReset) {
				++hostResets;
				resetPending |= sd::kCtrlReset;
				// CTRL_RESET_ALL clears the card-side state machine and, on dw_mmc, drops card power. This is
				// not a detail the driver may ignore: the first board run (2026-11-02) reset the controller,
				// never rewrote PWREN, and every CMD13 afterwards got no answer at all -- which the driver
				// reported as "no card" with cmd=0. edk2 never separates the two either: DwMmcHcInitPowerVoltage
				// writes PWREN=1 and then issues CTRL_RESET_ALL (DwMmcHci.c:508-527).
				powered = false;
				cardSilent = true;
			}
			if (value & sd::kCtrlDmaReset) {
				resetPending |= sd::kCtrlDmaReset;
			}
			if (value & sd::kCtrlFifoReset) {
				++fifoResets;
				resetPending |= sd::kCtrlFifoReset;
				fifo.clear();
				fifoResetSinceDataCommand = true;
			}
			return;
		case sd::kRegPwrEn:
			// Bit 0 applies card power. Until the driver writes it, a card that was reset stays silent.
			powered = (value & sd::kPwrEnEnable) != 0;
			if (powered) {
				cardSilent = false;
			}
			memcpy(regs + offset, &value, sizeof(value));
			return;
		case sd::kRegFifo:
			fifoPush(value);
			return;
		case sd::kRegBlkSiz:
			blkSiz = value;
			return;
		case sd::kRegBytCnt:
			bytCnt = value;
			return;
		default:
			memcpy(regs + offset, &value, sizeof(value));
			return;
		}
	}

	// ---------------------------------------------------------------- FIFO ----
	uint32_t fifoPop() {
		if (fifo.empty()) {
			// The driver asked for a word the card has not delivered. On hardware this reads a stale or zero
			// word; either way it is a bug, so the model records it rather than silently inventing data.
			++fifoUnderruns;
			return 0;
		}
		uint32_t value = fifo.front();
		fifo.pop_front();
		if (fifo.empty() && inRun && !runIsWrite) {
			if (runRemaining > 0) {
				// The card hands over the next block lazily, on the next STATUS poll, the way a real card does:
				// the FIFO does not instantly refill. That delay is what makes an over-eager read -- one that
				// asks for more words than are actually filled -- detectable, because with an eager refill the
				// correct and incorrect predicates would look identical.
				pendingRefill = true;
			} else {
				// Nothing left to deliver: the run is over as soon as its last word has been taken. A real
				// controller raises DTO here too, so the driver does not have to poll again to find out.
				finishReadRun();
			}
		}
		return value;
	}

	void fifoPush(uint32_t value) {
		if (fifo.size() >= kFifoDepthWords) {
			++fifoOverruns;
			return;
		}
		fifo.push_back(value);
		if (fifo.size() >= kFifoDepthWords) {
			consumeWriteBlock();
		}
	}

	// Called when the driver polls STATUS: this is when the card hands over the next block of a read run.
	void serviceRefill() {
		if (!pendingRefill) {
			return;
		}
		pendingRefill = false;
		if (runRemaining > 0) {
			serveReadBlock();
		} else {
			finishReadRun();
		}
	}

	// ------------------------------------------------------------- read runs --
	void startReadRun(uint64_t lba, uint32_t count) {
		inRun = true;
		runIsWrite = false;
		runLba = lba;
		runBlocks = count;
		runRemaining = count;
		runServed = 0;
		state = Data;
		dataBusy = true;
		serveReadBlock();
	}

	void serveReadBlock() {
		if (runRemaining == 0) {
			finishReadRun();
			return;
		}
		const uint64_t lba = runLba + runServed;
		if ((lba + 1) * sd::kBlockBytes > image.size()) {
			// Past the end of the medium: the card answers with a data timeout.
			intStatus |= sd::kIntDrt | sd::kIntRe;
			inRun = false;
			dataBusy = false;
			state = Tran;
			return;
		}
		fifo.clear();
		const uint8_t *source = image.data() + lba * sd::kBlockBytes;
		for (uint32_t word = 0; word < sd::kBlockBytes / 4; ++word) {
			uint32_t value = 0;
			memcpy(&value, source + word * 4, sizeof(value));
			fifo.push_back(value);
		}
		++runServed;
		--runRemaining;
		++blocksRead;
	}

	void finishReadRun() {
		intStatus |= sd::kIntDto;
		if (runBlocks > 1) {
			intStatus |= sd::kIntAcd;
		}
		inRun = false;
		dataBusy = false;
		state = Tran;
	}

	// ------------------------------------------------------------ write runs --
	void startWriteRun(uint64_t lba, uint32_t count) {
		inRun = true;
		runIsWrite = true;
		runLba = lba;
		runBlocks = count;
		runRemaining = count;
		runServed = 0;
		runWriteBuffer.clear();
		state = Rcv;
		dataBusy = true;
		if ((lba + count) * sd::kBlockBytes > image.size()) {
			intStatus |= sd::kIntDrt | sd::kIntRe;
			inRun = false;
			dataBusy = false;
			state = Tran;
		}
	}

	void consumeWriteBlock() {
		if (!inRun || !runIsWrite || runRemaining == 0) {
			return;
		}
		for (uint32_t index = 0; index < kFifoDepthWords; ++index) {
			const uint32_t word = fifo.front();
			fifo.pop_front();
			uint8_t bytes[4];
			memcpy(bytes, &word, sizeof(bytes));
			runWriteBuffer.insert(runWriteBuffer.end(), bytes, bytes + 4);
		}
		++runServed;
		--runRemaining;
		if (runRemaining == 0) {
			finishWriteRun();
		}
	}

	void finishWriteRun() {
		const uint64_t at = runLba * sd::kBlockBytes;
		if (at + runWriteBuffer.size() <= image.size()) {
			memcpy(image.data() + at, runWriteBuffer.data(), runWriteBuffer.size());
		}
		blocksWritten += runBlocks;
		intStatus |= sd::kIntDto;
		if (runBlocks > 1) {
			intStatus |= sd::kIntAcd;
		}
		inRun = false;
		dataBusy = false;
		state = Prg;
		prgPollsLeft = failNeverReady ? 1000000u : programPolls + 1;
	}

	// -------------------------------------------------------------- commands --
	void issueCommand(uint32_t word) {
		const unsigned index = word & 0x3fu;
		commandsSeen.insert(index);

		if ((word & sd::kCmdStart) == 0) {
			return;
		}
		if (commandBusy) {
			++cmdWhileBusy;
			return;
		}
		if (intStatus & sd::kIntCmdDone) {
			++staleCmdDone;
		}

		const bool hasData = (word & sd::kCmdDataExpected) != 0;
		const bool isWrite = (word & sd::kCmdWrite) != 0;
		uint32_t argument = 0;
		memcpy(&argument, regs + sd::kRegCmdArg, sizeof(argument));

		if (cardSilent) {
			// No power means no answer: not a timeout, not an error, just nothing. This is what the board
			// showed after the driver reset the controller without restoring PWREN -- every CMD13 attempt
			// left the command FSM waiting, and the run ended with `cmd=0` because no command ever finished.
			++commandsWhileUnpowered;
			return;
		}

		if (failNoResponse) {
			--failNoResponse;
			return;   // no CMD_DONE, no error: what a dead card looks like
		}

		if (state == Prg && index != sd::kCmdSendStatus) {
			// The card holds DAT0 busy and accepts only CMD13.
			++illegalCommands;
			illegalPending = true;
			intStatus |= sd::kIntRto;
			return;
		}
		if (hasData) {
			if (!fifoResetSinceDataCommand) {
				++dataWithoutFifoReset;
			}
			fifoResetSinceDataCommand = false;
			++dataCommands;
		}

		commandBusy = true;
		dispatch(index, argument, hasData, isWrite);
		commandBusy = false;
	}

	void dispatch(unsigned index, uint32_t argument, bool hasData, bool isWrite) {
		switch (index) {
		case sd::kCmdSendStatus: {
			if ((argument >> 16) != rca) {
				intStatus |= sd::kIntRto;
				return;
			}
			if (state == Prg && prgPollsLeft && --prgPollsLeft == 0) {
				state = Tran;
			}
			illegalPending = false;
			respond(cardStatus());
			return;
		}
		case sd::kCmdSendCsd: {
			if ((argument >> 16) != rca) {
				intStatus |= sd::kIntRto;
				return;
			}
			respond(0);
			// Emit the CSD as a byte stream, the way a real card presents it, and let the driver work out
			// where the payload sits: that is the whole point of the byte-order search. The model only
			// has to be self-consistent -- a v2.0 structure, a plausible C_SIZE, and a CRC7 the driver can
			// validate. RESPn carries byte stream words little-endian in the "msb" convention the driver
			// tries first, so the same bytes come back out of it.
			uint8_t csd[16] = {0};
			csd[0] = static_cast<uint8_t>(1u << 6);   // CSD_STRUCTURE = 1 (v2.0), bits 127:126
			csd[3] = 0x32;                            // TRAN_SPEED, a plausible 25 MHz / 12.5 MB/s
			csd[4] = 0x5A;                            // TAAC
			csd[7] = static_cast<uint8_t>((kModelCSize >> 16) & 0x3fu);
			csd[8] = static_cast<uint8_t>((kModelCSize >> 8) & 0xffu);
			csd[9] = static_cast<uint8_t>(kModelCSize & 0xffu);
			csd[10] = 0x80;                           // CCC
			// A nonzero CRC7 matters: an all-zero CSD checksums to zero, and a zero checksum validates the
			// byte-reversed reading too, which would make the driver's search look correct while testing
			// nothing. The filler bytes above keep the checksum away from zero.
			const uint8_t crc = csdCrc7(csd);
			csd[15] = static_cast<uint8_t>((crc << 1) | 0x1u);   // CRC7 in bits 7:1, end bit set
			// "Most significant first" is what the driver tries first; the other framing shifts every byte
			// within its register, which is the layout that made the board report the wrong capacity.
			for (unsigned word = 0; word < 4; ++word) {
				uint32_t packed = 0;
				for (unsigned byte = 0; byte < 4; ++byte) {
					const unsigned shift = (csdWordOrder == CsdWordOrder::MostSignificantFirst)
						? (24 - 8 * byte)
						: (8 * byte);
					packed |= static_cast<uint32_t>(csd[word * 4 + byte]) << shift;
				}
				response[word] = packed;
			}
			intStatus |= sd::kIntCmdDone;
			return;
		}
		case sd::kCmdAppCmd: {
			if ((argument >> 16) != rca) {
				intStatus |= sd::kIntRto;
				return;
			}
			respond(cardStatus() | sd::kStatusAppCmd);
			return;
		}
		case sd::kCmdReadSingle: {
			if (state != Tran) {
				intStatus |= sd::kIntRto;
				return;
			}
			if (failDataTimeout) {
				--failDataTimeout;
				respond(0);
				intStatus |= sd::kIntDrt;
				return;
			}
			respond(0);
			startReadRun(argument, 1);
			return;
		}
		case sd::kCmdReadMulti: {
			if (state != Tran) {
				intStatus |= sd::kIntRto;
				return;
			}
			if (failDataTimeout) {
				--failDataTimeout;
				respond(0);
				intStatus |= sd::kIntDrt;
				return;
			}
			respond(0);
			startReadRun(argument, bytCnt / sd::kBlockBytes);
			return;
		}
		case sd::kCmdWriteSingle:
		case sd::kCmdWriteMulti: {
			if (state != Tran) {
				intStatus |= sd::kIntRto;
				return;
			}
			respond(0);
			startWriteRun(argument, bytCnt / sd::kBlockBytes);
			return;
		}
		case sd::kCmdStopTransmission: {
			state = Tran;
			dataBusy = false;
			inRun = false;
			fifo.clear();
			respond(cardStatus());
			return;
		}
		default:
			respond(cardStatus());
			return;
		}
		(void)hasData;
		(void)isWrite;
	}

	void respond(uint32_t value) {
		response[0] = value;
		response[1] = 0;
		response[2] = 0;
		response[3] = 0;
		if (failResponseCrc) {
			--failResponseCrc;
			intStatus |= sd::kIntRcrc | sd::kIntRe;
			return;
		}
		intStatus |= sd::kIntCmdDone;
	}

	void tick(uint32_t microseconds) {
		clockUs += microseconds;
		// Resets complete after a couple of polls, so a driver that does not wait sees them still pending.
		if (resetPending) {
			if (++resetPolls >= 2) {
				resetPending = 0;
				resetPolls = 0;
			}
		}
	}
};

// ---------------------------------------------------------------- platform ----

Model *gModel = nullptr;
unsigned gLogLines = 0;

uint32_t modelRead(void *, uint32_t offset, unsigned width) {
	if (width != 32) {
		++gModel->narrowAccesses;
	}
	return gModel->rd(offset);
}

void modelWrite(void *, uint32_t offset, unsigned width, uint32_t value) {
	gModel->wr(offset, width, value);
}

uint64_t modelNowUs() {
	return gModel->clockUs;
}

void modelDelayUs(uint32_t microseconds) {
	gModel->tick(microseconds);
}

void modelLog(const char *, ...) {
	++gLogLines;
}

sd::Platform gPlatform = {modelRead, modelWrite, modelNowUs, modelDelayUs, modelLog};

sd::Host makeHost(Model &model) {
	gModel = &model;
	sd::Host host;
	host.attach(&gPlatform, model.regs);
	return host;
}

// Block N begins with the little-endian block number, so a misaligned transfer is obvious.
std::vector<uint8_t> makeImage(uint32_t blocks) {
	std::vector<uint8_t> image(static_cast<size_t>(blocks) * sd::kBlockBytes);
	for (uint32_t block = 0; block < blocks; ++block) {
		uint8_t *at = image.data() + static_cast<size_t>(block) * sd::kBlockBytes;
		memcpy(at, &block, sizeof(block));
		for (uint32_t offset = 4; offset < sd::kBlockBytes; ++offset) {
			at[offset] = static_cast<uint8_t>((block * 31u + offset) & 0xffu);
		}
	}
	return image;
}

Model makeReadyModel(uint32_t blocks = 256) {
	Model model;
	model.image = makeImage(blocks);
	model.state = Model::Tran;
	model.rca = 1;
	return model;
}

uint32_t blockNumberAt(const uint8_t *buffer, uint32_t index) {
	uint32_t value = 0;
	memcpy(&value, buffer + index * sd::kBlockBytes, sizeof(value));
	return value;
}

// The number of blocks a CSD v2.0 with the model's C_SIZE advertises.
constexpr uint32_t kModelBlocks = (kModelCSize + 1u) << 10;

// ---------------------------------------------------------------- the tests ---

void testIdentify() {
	// The default path: no controller reset at all. The firmware hands over a live card, and the board run
	// on 2026-11-02 proved that resetting it costs the card's power supply (CTRL_RESET_ALL clears PWREN) and
	// makes it unanswerable. So the default must not reset, and this test pins that down two ways: no reset
	// happened, and the card is still powered afterwards.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	CHECK(host.usable());
	CHECK(host.rca() == 1);
	CHECK(host.geometry().valid);
	CHECK(host.geometry().sdhc);
	CHECK(host.geometry().blockCount == kModelBlocks);
	CHECK(!host.geometry().writeProtected);
	CHECK(model.hostResets == 0);        // <- the whole point: the gentle path resets nothing
	CHECK(model.powered);                // <- and therefore keeps the card powered
	CHECK(model.commandsWhileUnpowered == 0);
	CHECK(model.narrowAccesses == 0);
	CHECK(model.staleCmdDone == 0);
	CHECK(model.cmdWhileBusy == 0);
	// Only commands the firmware already validated on this card may be issued during identification.
	for (unsigned seen : model.commandsSeen) {
		CHECK(seen == sd::kCmdSendStatus || seen == sd::kCmdSendCsd || seen == sd::kCmdStopTransmission);
	}
}

void testCsdByteOrderIsProvenNotGuessed() {
	// The board run on 2026-11-02 delivered a card whose CSD, read "naturally", decoded as a 24 MiB SDSC
	// device on a 30727 MiB SDHC card. The capacity was the only symptom; every block read was correct. The
	// driver now decides the byte order by checking the CSD's own CRC7, so this test pins down that the
	// search runs and lands on the layout the model emitted -- and that the opposite layout, which is the
	// one that produced the wrong answer, is rejected.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	CHECK(host.geometry().sdhc);
	CHECK(host.geometry().blockCount == kModelBlocks);

	// A card that presents its CSD least-significant-first must decode identically. Without the CRC7 check
	// this reading is the one that halves the capacity, so getting the same answer here is evidence the
	// search is doing real work rather than always taking the first candidate.
	Model reversed = makeReadyModel();
	reversed.csdWordOrder = Model::CsdWordOrder::LeastSignificantFirst;
	sd::Host reversedHost = makeHost(reversed);
	CHECK(reversedHost.identify() == sd::kOk);
	CHECK(reversedHost.geometry().sdhc);
	CHECK(reversedHost.geometry().blockCount == kModelBlocks);
}

void testIdentifySoftResetRestoresPower() {
	// The opt-in path. A caller that asks for a reset must still end up with a usable card, which means the
	// driver has to rewrite PWREN afterwards. This is the regression test for the defect the first board run
	// exposed: reset happened, PWREN was never restored, every CMD13 went unanswered, identify() returned
	// "no card" with cmd=0.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify(true) == sd::kOk);
	CHECK(host.usable());
	CHECK(model.hostResets == 1);
	CHECK(model.powered);                  // restorePower() rewrote PWREN
	CHECK(model.commandsWhileUnpowered == 0);
	CHECK(host.geometry().blockCount == kModelBlocks);
}

void testResetWithoutPowerLosesTheCard() {
	// The failure mode itself, made explicit: if a reset is followed by commands and PWREN is never
	// restored, the card must go silent and identify() must fail. Without this test the fix above could rot
	// back into the shipped bug and nothing would notice.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	model.powered = false;
	model.cardSilent = true;
	CHECK(host.identify() == sd::kErrNoCard);
	CHECK(!host.usable());
	CHECK(model.commandsWhileUnpowered > 0);   // the card genuinely ignored us rather than being found
}

void testIdentifyFindsShiftingRca() {
	Model model = makeReadyModel();
	model.rca = 3;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	CHECK(host.rca() == 3);
}

void testIdentifyNoCard() {
	Model model = makeReadyModel();
	model.state = Model::Idle;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kErrNoCard);
	CHECK(!host.usable());
}

void testReadSingleBlock() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	uint8_t block[sd::kBlockBytes] = {};
	CHECK(host.readBlocks(7, 1, block) == sd::kOk);
	CHECK(blockNumberAt(block, 0) == 7);
	for (uint32_t offset = 4; offset < sd::kBlockBytes; ++offset) {
		CHECK(block[offset] == static_cast<uint8_t>((7u * 31u + offset) & 0xffu));
	}
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);
	CHECK(model.dataWithoutFifoReset == 0);
	CHECK(host.stats().blocksRead == 1);
}

void testReadMultiBlock() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	std::vector<uint8_t> buffer(4 * sd::kBlockBytes);
	CHECK(host.readBlocks(11, 4, buffer.data()) == sd::kOk);
	for (uint32_t index = 0; index < 4; ++index) {
		CHECK(blockNumberAt(buffer.data(), index) == 11 + index);
	}
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);
	CHECK(host.stats().blocksRead == 4);
}

void testLargeMultiBlockRead() {
	Model model = makeReadyModel(1024);
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	std::vector<uint8_t> buffer(64 * sd::kBlockBytes);
	CHECK(host.readBlocks(900, 64, buffer.data()) == sd::kOk);
	CHECK(blockNumberAt(buffer.data(), 0) == 900);
	CHECK(blockNumberAt(buffer.data(), 63) == 963);
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);
}

void testReadPastMediumFails() {
	// The advertised capacity comes from the CSD, but the medium in the model is smaller. The card reports a
	// data timeout; the driver must surface it rather than return stale bytes.
	Model model = makeReadyModel(16);
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	CHECK(host.readBlocks(20, 1, block) != sd::kOk);
}

void testReadOutOfRange() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	CHECK(host.readBlocks(host.geometry().blockCount, 1, block) == sd::kErrArgument);
	CHECK(host.readBlocks(0, 0, block) == sd::kErrArgument);
	CHECK(host.readBlocks(0, 1, nullptr) == sd::kErrArgument);
}

void testWriteSingleBlock() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	uint8_t block[sd::kBlockBytes];
	memset(block, 0xa5, sizeof(block));
	CHECK(host.writeBlocks(3, 1, block) == sd::kOk);
	CHECK(model.state == Model::Tran);   // polled back to TRAN before returning
	CHECK(host.stats().blocksWritten == 1);
	CHECK(model.image[3 * sd::kBlockBytes] == 0xa5);
	CHECK(model.image[3 * sd::kBlockBytes + sd::kBlockBytes - 1] == 0xa5);
}

void testWriteMultiBlock() {
	Model model = makeReadyModel();
	model.programPolls = 5;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	std::vector<uint8_t> buffer(3 * sd::kBlockBytes, 0x5a);
	CHECK(host.writeBlocks(20, 3, buffer.data()) == sd::kOk);
	CHECK(model.state == Model::Tran);
	CHECK(model.illegalCommands == 0);
	CHECK(model.fifoOverruns == 0);
	for (uint32_t offset = 0; offset < 3 * sd::kBlockBytes; ++offset) {
		CHECK(model.image[20 * sd::kBlockBytes + offset] == 0x5a);
	}
}

void testWriteNeverReadyTimesOut() {
	Model model = makeReadyModel();
	model.failNeverReady = 1;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	uint8_t block[sd::kBlockBytes];
	memset(block, 0, sizeof(block));
	CHECK(host.writeBlocks(4, 1, block) == sd::kErrTimeout);
	CHECK(host.stats().timeouts > 0);
}

void testWriteProtectedRefused() {
	Model model = makeReadyModel();
	model.writeProtected = true;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	CHECK(host.geometry().writeProtected);

	uint8_t block[sd::kBlockBytes];
	memset(block, 0, sizeof(block));
	CHECK(host.writeBlocks(0, 1, block) == sd::kErrState);
	CHECK(host.stats().blocksWritten == 0);
}

void testWriteOutOfRange() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	memset(block, 0, sizeof(block));
	CHECK(host.writeBlocks(host.geometry().blockCount, 1, block) == sd::kErrArgument);
	CHECK(host.writeBlocks(0, 0, block) == sd::kErrArgument);
	CHECK(host.writeBlocks(0, 1, nullptr) == sd::kErrArgument);
}

void testResponseTimeout() {
	Model model = makeReadyModel();
	model.failNoResponse = 1;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kErrNoCard);
	CHECK(host.stats().timeouts > 0);
}

void testResponseCrcError() {
	Model model = makeReadyModel();
	model.failResponseCrc = 1;
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kErrNoCard);
	CHECK(host.stats().commandErrors > 0);
}

void testDataTimeout() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	model.failDataTimeout = 1;
	uint8_t block[sd::kBlockBytes];
	CHECK(host.readBlocks(0, 1, block) != sd::kOk);
}

void testFifoAccountingMatchesController() {
	// The regression this test exists for: treating STATUS[28:17] as free space instead of filled space.
	// A model that enforces the filled semantics catches it as an overrun or a starvation.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	std::vector<uint8_t> buffer(8 * sd::kBlockBytes);
	CHECK(host.readBlocks(0, 8, buffer.data()) == sd::kOk);
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);

	std::vector<uint8_t> payload(8 * sd::kBlockBytes, 0x33);
	CHECK(host.writeBlocks(40, 8, payload.data()) == sd::kOk);
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);
}

void testWideRegisters() {
	// BLKSIZ and BYTCNT are 32-bit; a 16-bit store leaves the upper half holding the previous count.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	CHECK(host.readBlocks(0, 1, block) == sd::kOk);
	CHECK(model.blkSiz == sd::kBlockBytes);
	CHECK(model.bytCnt == sd::kBlockBytes);
	CHECK(model.narrowAccesses == 0);
}

void testEveryDataCommandResetsFifo() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	for (uint32_t index = 0; index < 6; ++index) {
		CHECK(host.readBlocks(index, 1, block) == sd::kOk);
	}
	CHECK(model.dataWithoutFifoReset == 0);
	CHECK(model.dataCommands == 6);
}

void testCmdDoneClearedBetweenCommands() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	uint8_t block[sd::kBlockBytes];
	for (uint32_t index = 0; index < 5; ++index) {
		CHECK(host.readBlocks(index, 1, block) == sd::kOk);
	}
	CHECK(model.staleCmdDone == 0);
	CHECK(model.cmdWhileBusy == 0);
}

void testInterleavedReadWrite() {
	// A realistic boot sequence: read a header, write a manifest, read it back.
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);

	uint8_t header[sd::kBlockBytes];
	CHECK(host.readBlocks(0, 1, header) == sd::kOk);

	std::vector<uint8_t> manifest(4 * sd::kBlockBytes, 0x77);
	CHECK(host.writeBlocks(100, 4, manifest.data()) == sd::kOk);

	std::vector<uint8_t> readback(4 * sd::kBlockBytes, 0);
	CHECK(host.readBlocks(100, 4, readback.data()) == sd::kOk);
	CHECK(memcmp(readback.data(), manifest.data(), manifest.size()) == 0);
	CHECK(model.illegalCommands == 0);
	CHECK(model.fifoOverruns == 0);
	CHECK(model.fifoUnderruns == 0);
}

void testFlushCache() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	CHECK(host.flushCache() == sd::kOk);
}

void testDumpDoesNotCrash() {
	Model model = makeReadyModel();
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kOk);
	host.dump("test");
	CHECK(gLogLines > 0);
}

void testNoCardReadFails() {
	Model model;
	model.state = Model::Idle;
	model.image = makeImage(16);
	sd::Host host = makeHost(model);
	CHECK(host.identify() == sd::kErrNoCard);
	uint8_t block[sd::kBlockBytes];
	CHECK(host.readBlocks(0, 1, block) == sd::kErrState);
	CHECK(host.writeBlocks(0, 1, block) == sd::kErrState);
	CHECK(host.flushCache() == sd::kErrState);
}

void testResultNames() {
	CHECK(strcmp(sd::resultName(sd::kOk), "ok") == 0);
	CHECK(strcmp(sd::resultName(sd::kErrTimeout), "timeout") == 0);
	CHECK(strcmp(sd::resultName(sd::kErrNoCard), "no card") == 0);
	CHECK(strcmp(sd::resultName(-99), "unknown") == 0);
}

}   // namespace

int main() {
	testIdentify();
	testCsdByteOrderIsProvenNotGuessed();
	testIdentifySoftResetRestoresPower();
	testResetWithoutPowerLosesTheCard();
	testIdentifyFindsShiftingRca();
	testIdentifyNoCard();
	testReadSingleBlock();
	testReadMultiBlock();
	testLargeMultiBlockRead();
	testReadPastMediumFails();
	testReadOutOfRange();
	testWriteSingleBlock();
	testWriteMultiBlock();
	testWriteNeverReadyTimesOut();
	testWriteProtectedRefused();
	testWriteOutOfRange();
	testResponseTimeout();
	testResponseCrcError();
	testDataTimeout();
	testFifoAccountingMatchesController();
	testWideRegisters();
	testEveryDataCommandResetsFifo();
	testCmdDoneClearedBetweenCommands();
	testInterleavedReadWrite();
	testFlushCache();
	testDumpDoesNotCrash();
	testNoCardReadFails();
	testResultNames();

	if (failures == 0) {
		printf("PASS: all SdHost tests passed\n");
		return 0;
	}
	printf("FAILED: %d check(s)\n", failures);
	return 1;
}
