// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors
//
// Polled OHCI host for the Orange Pi 5 Plus (RK3588 USB2 companions) and boot-protocol HID keyboard/mouse. Reports are published
// through the native_usb_* mailbox for NativeHidInput. Board copy of UniXNU-orangepi5 package/Opi5Usb @ 729ba25f4d67225ff3853ba1e435b95af97e59e3
// (Opi5Usb of a5f7a71d state, minus its xHCI glue): the port power GPIO (VBUS_EN, GPIO3_B7 here instead of GPIO3_C0 upstream) and the
// default of the opi5-usb argument (0 instead of 5) are the two behavior differences; see the comments at hostPowerEnable() and opi5_usb_start().
//
// The two USB 2.0 host blocks of the RK3588S are EHCI + OHCI pairs (one root port each). The firmware leaves the EHCI halted
// with CONFIGFLAG clear (every port belongs to the companion OHCI: PORTSC.owner = 1), so a full/low speed keyboard or mouse in
// one of the USB 2.0 Type-A ports shows up on the OHCI. EHCI is never started here. Behind a hub, or in a USB 3.0 port (xHCI), a
// device is not seen.
//
// Everything runs in one kernel thread (opi5_usb_start only spawns it): bring up both OHCIs, then poll for attach / detach
// (enumerate on attach, tear down on detach) and for HID interrupt reports. No interrupts.
//
// OHCI 1.0a, the parts used (register offsets in bytes, bit numbers from the specification):
//   HcCommandStatus 0x08: HCR 0, CLF 1 (control list filled), BLF 2, OCR 3
//   HcControl 0x04: CBSR 1:0, PLE 2, IE 3, CLE 4, BLE 5, HCFS 7:6 (0 reset, 1 resume, 2 operational, 3 suspend), IR 8, RWC 9, RWE 10
//   HcFmNumber is at 0x3c (HcPeriodicStart 0x40, HcLSThreshold 0x44, HcRhDescriptorA 0x48, B 0x4c, HcRhStatus 0x50, ports 0x54+4n)
//   ED (16 bytes): flags FA 6:0, EN 10:7, D 12:11 (0 from TD, 1 OUT, 2 IN), S 13 (low speed), K 14 (skip), F 15, MPS 26:16;
//                  tailP, headP (bit 0 halted, bit 1 data toggle carry), nextED. The HC stops when headP == tailP: the tail is a
//                  dummy TD that is never processed.
//   TD (16 bytes): flags R 18, DP 20:19 (0 SETUP, 1 OUT, 2 IN), DI 23:21, T 25:24 (0x carry from the ED, 10 DATA0, 11 DATA1),
//                  EC 27:26, CC 31:28 (0xF = not accessed yet); cbp, nextTD, be
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "MouseReport.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// Normal non-cacheable (the mapping XNU gives its panic log): unaligned accesses and DC ZVA are fine, unlike ml_io_map's Device memory.
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

extern "C" {
volatile uint32_t native_usb_hid_reports;
volatile uint32_t native_usb_kbd_seq;
volatile uint8_t native_usb_kbd[8];
volatile uint32_t native_usb_mouse_seq;
volatile uint8_t native_usb_mouse[4];
// Motion and wheel of every mouse report since the consumer last took them: native_usb_mouse above keeps only the newest report, so a
// consumer that looks every 8 ms would lose the motion of the reports in between (about three of four at a 1 kHz poll).
volatile int32_t native_usb_mouse_dx, native_usb_mouse_dy, native_usb_mouse_wheel;
// Wheel counts of every report since the boot, without sign (the poll line prints it, so a turned wheel can be seen on the serial console), and how the
// mouse is run: 1 = report protocol with the layout read from its report descriptor, 0 = boot protocol (three bytes: no wheel).
volatile uint32_t native_usb_mouse_wheel_total, native_usb_mouse_report_protocol;
}

namespace {

using opi5usb::MouseLayout;
using opi5usb::clamp8;
using opi5usb::fieldValue;
using opi5usb::parseMouseLayout;

constexpr uint64_t kOhci0Phys = 0xfc840000ull;
constexpr uint64_t kOhci1Phys = 0xfc8c0000ull;
constexpr uint64_t kEhci0Phys = 0xfc800000ull;
constexpr uint64_t kEhci1Phys = 0xfc880000ull;
constexpr uint64_t kGpio3Phys = 0xfec40000ull;
constexpr uint64_t kCruPhys = 0xfd7c0000ull;

// ---- OHCI registers (byte offsets)
constexpr uint32_t kRegRev = 0x00, kRegCtl = 0x04, kRegCmd = 0x08, kRegIntSt = 0x0c, kRegIntDis = 0x14, kRegHcca = 0x18;
constexpr uint32_t kRegCtlHead = 0x20, kRegBulkHead = 0x28, kRegFmInt = 0x34, kRegPerStart = 0x40, kRegLsThr = 0x44;
constexpr uint32_t kRegRhA = 0x48, kRegRhB = 0x4c, kRegRhSt = 0x50, kRegRhPort = 0x54;

constexpr uint32_t kCmdHcr = 1u << 0, kCmdClf = 1u << 1;
constexpr uint32_t kCtlCbsr = 3u << 0, kCtlPle = 1u << 2, kCtlCle = 1u << 4, kCtlHcfsShift = 6, kHcfsOperational = 2;
constexpr uint32_t kRhStLpsc = 1u << 16;            // SetGlobalPower
constexpr uint32_t kPortCcs = 1u << 0, kPortPes = 1u << 1, kPortPrs = 1u << 4, kPortPps = 1u << 8, kPortLsda = 1u << 9;
constexpr uint32_t kPortPrsc = 1u << 20;               // reset complete (a change bit: write 1 to clear)

constexpr uint32_t kEdSkip = 1u << 14, kEdHalt = 1u;
constexpr uint32_t kTdCcNew = 0xfu << 28, kTdDi = 7u << 21, kTdR = 1u << 18;
constexpr uint32_t kTdSetup = 0u << 19, kTdOut = 1u << 19, kTdIn = 2u << 19;
constexpr uint32_t kTdData0 = 2u << 24, kTdData1 = 3u << 24;

constexpr uint32_t kUsbReqGetDescriptor = 6, kUsbReqSetAddress = 5, kUsbReqSetConfiguration = 9;
constexpr uint32_t kHidReqSetIdle = 0x0a, kHidReqSetProtocol = 0x0b;

// ---- the DMA pool (one per host, 0x2000 bytes below 4 GiB)
constexpr uint32_t kPoolBytes = 0x2000;
constexpr uint32_t kOffHcca = 0x000;                 // 256 bytes, 256-aligned
constexpr uint32_t kOffCtrlEd = 0x100;
constexpr uint32_t kOffHidEd = 0x110;                // + 0x10 * hid (0 keyboard, 1 mouse)
constexpr uint32_t kOffCtrlTd = 0x200;               // setup, data, status, dummy
constexpr uint32_t kOffHidTd = 0x240;                // + 0x30 * hid: a ring of three TDs
constexpr uint32_t kOffSetup = 0x300;
constexpr uint32_t kOffData = 0x340;                 // up to 0x1c0 bytes
constexpr uint32_t kOffHidBuf = 0x500;               // + 0x30 * hid: three 16-byte report buffers
constexpr uint32_t kDataMax = 255;

struct OhciEd {
  uint32_t flags, tailP, headP, nextEd;
};
struct OhciTd {
  uint32_t flags, cbp, nextTd, be;
};

struct HidDev {
  bool inUse = false;
  bool isMouse = false;
  uint8_t iface = 0, ep = 0, maxPacket = 8;
  uint16_t reportDescLen = 0;                        // from the HID class descriptor in the configuration
  MouseLayout layout;                                // valid: the mouse runs in report protocol and publishReport reads this layout
  unsigned cur = 0;                                  // the TD of the ring the HC is working on
};

struct DmaPool {
  uint8_t *virt = nullptr;
  uint32_t bus = 0;
  IOMemoryDescriptor *mem = nullptr;
};

struct Host {
  volatile uint32_t *regs = nullptr;
  const char *name = nullptr;
  uint64_t ehciPhys = 0;
  DmaPool dma;
  bool started = false, enumerated = false, low = false;
  uint8_t addr = 0, mps0 = 8;
  unsigned ports = 0;
  uint64_t lastTry = 0;
  unsigned failures = 0, tries = 0;
  HidDev hid[2];
};

Host gHost0, gHost1;
unsigned gMode = 0;
uint32_t gPollFrames = 0, gPollReports = 0, gPollErrors = 0;

// ---- helpers
uint32_t rd(volatile uint32_t *p) { return *p; }
void wr(volatile uint32_t *p, uint32_t v) { *p = v; }
uint32_t rdr(Host *h, uint32_t off) { return h->regs[off / 4]; }
void wrr(Host *h, uint32_t off, uint32_t v) {
  h->regs[off / 4] = v;
  __asm__ volatile("dsb sy" ::: "memory");
}

void dmaFlush(volatile void *virt, size_t bytes) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
  const uintptr_t end = reinterpret_cast<uintptr_t>(virt) + bytes;
  __asm__ volatile("dsb sy" ::: "memory");
  for (uintptr_t address = reinterpret_cast<uintptr_t>(virt) & ~(line - 1); address < end; address += line)
    __asm__ volatile("dc civac, %0" ::"r"(address) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t bootArg(const char *name, uint32_t fallback) {
  uint32_t value = fallback;
  if (!PE_parse_boot_argn(name, &value, sizeof(value))) return fallback;
  return value;
}

uint64_t nowUs() {
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

void delayUs(uint32_t us) {
  if (us >= 1000) {
    IOSleep((us + 999) / 1000);
    return;
  }
  const uint64_t until = nowUs() + us;
  while (nowUs() < until) {
  }
}

void bumpSeq(volatile uint32_t *seq) {
  __atomic_fetch_add(seq, 1, __ATOMIC_SEQ_CST);   // atomic: any second publisher (the xHCI glue of the upstream board, if it
                                                  // is taken in a later step) publishes to the same mailbox
  __asm__ volatile("dmb sy" ::: "memory");
}

void mmioWrite32(uint64_t phys, uint32_t value) {
  vm_offset_t mapped = ml_io_map(static_cast<vm_offset_t>(phys & ~0xfffull), 0x1000);
  if (!mapped) return;
  wr(reinterpret_cast<volatile uint32_t *>(mapped + (phys & 0xfff)), value);
}

// ---- pool accessors
volatile OhciEd *edAt(Host *h, uint32_t off) { return reinterpret_cast<volatile OhciEd *>(h->dma.virt + off); }
volatile OhciTd *tdAt(Host *h, uint32_t off) { return reinterpret_cast<volatile OhciTd *>(h->dma.virt + off); }
volatile uint8_t *bufAt(Host *h, uint32_t off) { return h->dma.virt + off; }
uint32_t busAt(Host *h, uint32_t off) { return h->dma.bus + off; }

uint32_t edFlags(uint8_t addr, uint8_t ep, uint32_t dir, bool low, uint16_t mps) {
  return (addr & 0x7fu) | ((ep & 0xfu) << 7) | (dir << 11) | (low ? (1u << 13) : 0) | (static_cast<uint32_t>(mps & 0x7ff) << 16);
}

// ---- platform power and PHY
void phyResume() {
  // The same writes as the firmware's Usb2PhyResume (UniXNUOrangePi5 RockchipPlatformLib.c): the four USB2 PHY GRFs and the PHP GRF.
  mmioWrite32(0xfd5d0008, 0x20000000);
  mmioWrite32(0xfd5d4008, 0x20000000);
  mmioWrite32(0xfd5d8008, 0x20000000);
  mmioWrite32(0xfd5dc008, 0x20000000);
  mmioWrite32(0xfd7f0a10, 0x07000700);
  mmioWrite32(0xfd7f0a10, 0x07000000);
}

// GPIO3_B7 (vcc5v0-host-en) high powers the 5 V rail of the USB 2.0 host ports (the board dts: /vcc5v0-host regulator-fixed,
// gpio = <&gpio3 15 0>, pinctrl /pinctrl/usb/vcc5v0-host-en rockchip,pins = <3 15 0>; upstream opi5 uses GPIO3_C0 / TYPEC_EN).
// RK3588 GPIO data and direction registers are 16 bits wide with a write-enable in the upper half: pin 15 is bit 15 of the
// _L registers. The Type-C connector VBUS (vbus5v0_typec, GPIO4_B0) is a separate rail that only the Type-C port needs; the
// OHCI keyboard / mouse ports sit on the vcc5v0_host rail, so it stays off.
void hostPowerEnable() {
  vm_offset_t gpio = ml_io_map(static_cast<vm_offset_t>(kGpio3Phys), 0x100);
  if (!gpio) {
    printf("opi5: usb: VBUS_EN: gpio3 map failed\n");
    return;
  }
  volatile uint32_t *drL = reinterpret_cast<volatile uint32_t *>(gpio + 0x0);
  volatile uint32_t *ddrL = reinterpret_cast<volatile uint32_t *>(gpio + 0x8);
  wr(drL, (1u << 16) | (1u << 15));
  wr(ddrL, (1u << 16) | (1u << 15));
  __asm__ volatile("dsb sy" ::: "memory");
  printf("opi5: usb: VBUS_EN GPIO3_B7 high (dr_l %#x ddr_l %#x)\n", rd(drL) & 0xffff, rd(ddrL) & 0xffff);
}

// Why the last reset happened, for the log: CRU GLB_RST_ST (0xfd7c0c04, sticky until the next reset: first/second global soft
// reset, watchdogs, TSADC shutdown ...). dvfsusb2 saw a reset with no panic and no BL31 "system reset" line 2.7 s after a
// watchdog feed; the bit that is set says which kind it was.
void logResetStatus() {
  vm_offset_t cru = ml_io_map(static_cast<vm_offset_t>(kCruPhys), 0x1000);
  if (cru)
    printf("opi5: reset: CRU GLB_RST_ST %#x (GLB_SRST_FST %#x SND %#x)\n", rd(reinterpret_cast<volatile uint32_t *>(cru + 0xc04)),
           rd(reinterpret_cast<volatile uint32_t *>(cru + 0xc08)), rd(reinterpret_cast<volatile uint32_t *>(cru + 0xc0c)));
}

// ---- EHCI (never started; looked at, and its port powered)
void probeEhci(Host *h) {
  vm_offset_t virt = ml_io_map(static_cast<vm_offset_t>(h->ehciPhys), 0x1000);
  if (!virt) {
    printf("opi5: usb: %s map failed\n", h->name);
    return;
  }
  volatile uint8_t *cap = reinterpret_cast<volatile uint8_t *>(virt);
  const uint8_t capLen = cap[0];
  const uint16_t hciVer = static_cast<uint16_t>(cap[2] | (cap[3] << 8));
  volatile uint32_t *op = reinterpret_cast<volatile uint32_t *>(virt + capLen);
  const uint32_t hcs = rd(reinterpret_cast<volatile uint32_t *>(virt + 4));
  // CONFIGFLAG at op+0x40 (CF=0: every port belongs to the companion OHCI), PORTSC[n] at op+0x44 (bit 13 = port owner = OHCI,
  // bit 12 = port power, there when HCSPARAMS.PPC (bit 4) is set).
  printf("opi5: usb: ehci%c caplen=%u hciVer=%#x hcsparams=%#x cmd=%#x sts=%#x cf=%#x\n", h->name[4], capLen, hciVer, hcs, rd(op + 0),
         rd(op + 1), rd(op + 0x10));
  for (unsigned p = 0; p < (hcs & 0xf) && p < 4; ++p) {
    volatile uint32_t *portsc = op + 0x11 + p;
    uint32_t v = rd(portsc);
    if ((hcs & (1u << 4)) && !(v & (1u << 12)) && gMode >= 2) {
      wr(portsc, (v & ~0x2au) | (1u << 12));   // keep the owner bit, do not write the change bits back (write 1 clears), power on
      delayUs(20000);
      printf("opi5: usb: ehci%c portsc%u=%#x -> %#x (port power on)\n", h->name[4], p, v, rd(portsc));
    } else {
      printf("opi5: usb: ehci%c portsc%u=%#x\n", h->name[4], p, v);
    }
  }
}

void printHostStatus(Host *h, const char *when) {
  printf("opi5: usb: %s %s: rev=%#x ctrl=%#x cmd=%#x intsts=%#x rhA=%#x rhB=%#x rhSt=%#x p0=%#x p1=%#x\n", h->name, when,
         rdr(h, kRegRev), rdr(h, kRegCtl), rdr(h, kRegCmd), rdr(h, kRegIntSt), rdr(h, kRegRhA), rdr(h, kRegRhB), rdr(h, kRegRhSt),
         rdr(h, kRegRhPort), rdr(h, kRegRhPort + 4));
}

// ---- DMA pool: OHCI is 32-bit DMA and the kernel's window sits above 4 GiB, so a low-mask contiguous allocation fails on this board;
// fall back to a reserved bounce in the lower DRAM bank the firmware left free (MEMMAP: ~0x40000000..0xE72DA000).
// This board keeps the upstream addresses 0x62000000 / 0x62010000: the firmware layout puts 0x62 in "System RAM (< 4GB)"
// (logs/boot-round13.out:266-269, PhysicalBase 0x9400000 Length 0xE6C00000) and nothing of the known fixed residents reaches it
// (initrd 0x50000000-0x56000000 :1,220, FDT 0x30000000 :216-218, CMA 0x10000000-0x20000000 :223, OP-TEE 0x8400000-0x9400000
// :262-265, framebuffer 0xE6740000 :1539); no boot log mentions 0x62xxxxxxxx at all, and the kernel window this port declares
// starts at 0x1_0000_0000, so the bounce is outside kernel-managed memory too. Verified on the board by opi5-usb=2: a pool the
// firmware does own shows up as a failed map / "dma window unusable" here.
bool dmaAlloc(Host *h) {
  const uint64_t bouncePhys = 0x62000000ull + (h == &gHost1 ? 0x10000ull : 0);
  IOMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone) | kIOMemoryPhysicallyContiguous,
      kPoolBytes, 0x00000000fffff000ULL);
  bool bounce = false;
  if (!mem) {
    mem = IOMemoryDescriptor::withPhysicalAddress(bouncePhys, kPoolBytes, kIODirectionInOut);
    bounce = mem != nullptr;
  }
  if (!mem) {
    printf("opi5: usb: %s dma alloc failed (%u bytes)\n", h->name, kPoolBytes);
    return false;
  }
  if (mem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    mem->release();
    printf("opi5: usb: %s dma prepare failed\n", h->name);
    return false;
  }
  IOByteCount length = 0;
  const uint64_t bus = mem->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
  uint8_t *virt = nullptr;
  if (IOBufferMemoryDescriptor *buf = OSDynamicCast(IOBufferMemoryDescriptor, mem)) {
    virt = static_cast<uint8_t *>(buf->getBytesNoCopy());
  } else if (bus) {
    // The bounce is plain DRAM used as structs and byte buffers (bzero uses DC ZVA, memcpy of 18 bytes is two overlapping 16-byte
    // accesses): Device memory faults on both ("Unaligned kernel data abort" at the first bzero, dvfsusb1). Non-cacheable also
    // makes the OHCI's DMA coherent without cache maintenance (dmaFlush stays, harmless).
    virt = reinterpret_cast<uint8_t *>(ml_io_map_wcomb(static_cast<vm_offset_t>(bus), kPoolBytes));
  }
  if (!virt || !bus || length < kPoolBytes || (bus & 0xff) || bus + kPoolBytes > (1ull << 32)) {
    mem->complete();
    mem->release();
    printf("opi5: usb: %s dma window unusable (bus %#llx len %llu)\n", h->name, static_cast<unsigned long long>(bus),
           static_cast<unsigned long long>(length));
    return false;
  }
  bzero(virt, kPoolBytes);
  dmaFlush(virt, kPoolBytes);
  h->dma.virt = virt;
  h->dma.bus = static_cast<uint32_t>(bus);
  h->dma.mem = mem;
  printf("opi5: usb: %s dma pool at bus %#llx (%s)\n", h->name, static_cast<unsigned long long>(bus), bounce ? "bounce, non-cacheable" : "kernel memory");
  return true;
}

// ---- controller
bool mapHost(Host *h, uint64_t phys, uint64_t ehci, const char *name) {
  vm_offset_t virt = ml_io_map(static_cast<vm_offset_t>(phys), 0x1000);
  if (!virt) return false;
  h->regs = reinterpret_cast<volatile uint32_t *>(virt);
  h->name = name;
  h->ehciPhys = ehci;
  return true;
}

unsigned rhPorts(Host *h) {
  unsigned n = rdr(h, kRegRhA) & 0xff;
  if (n > 15) n = 15;
  return n;
}

uint32_t portStatus(Host *h, unsigned port) { return rdr(h, kRegRhPort + 4 * port); }

void rhPower(Host *h) {
  const uint32_t a = rdr(h, kRegRhA);
  wrr(h, kRegRhSt, kRhStLpsc);                    // SetGlobalPower
  for (unsigned p = 0; p < h->ports; ++p) wrr(h, kRegRhPort + 4 * p, kPortPps);
  delayUs(((a >> 24) & 0xff) * 2000 + 20000);     // POTPGT (2 ms units) and some margin
}

bool startController(Host *h) {
  // Reset: HCR clears within 10 us; the controller is then in USBSUSPEND and must be made operational within 2 ms.
  const uint32_t fm = rdr(h, kRegFmInt);
  wrr(h, kRegCmd, kCmdHcr);
  bool done = false;
  for (unsigned i = 0; i < 100; ++i) {
    delayUs(100);
    if (!(rdr(h, kRegCmd) & kCmdHcr)) {
      done = true;
      break;
    }
  }
  if (!done) {
    printf("opi5: usb: %s controller reset failed\n", h->name);
    printHostStatus(h, "after failed reset");
    return false;
  }
  // The pool starts clean; the control ED is skipped and empty (head == tail == the dummy TD) until a transfer is queued.
  bzero(h->dma.virt, kPoolBytes);
  volatile OhciEd *ed = edAt(h, kOffCtrlEd);
  const uint32_t dummy = busAt(h, kOffCtrlTd + 0x30);
  ed->flags = kEdSkip;
  ed->tailP = dummy;
  ed->headP = dummy;
  ed->nextEd = 0;
  dmaFlush(h->dma.virt, kPoolBytes);
  wrr(h, kRegHcca, busAt(h, kOffHcca));
  wrr(h, kRegCtlHead, busAt(h, kOffCtrlEd));
  wrr(h, kRegBulkHead, 0);
  wrr(h, kRegIntDis, 0xffffffffu);
  wrr(h, kRegIntSt, 0xffffffffu);
  uint32_t fi = fm & 0x3fff;
  if (fi < 0x2000) fi = 0x2edf;                   // the specification's default (11999 bit times)
  const uint32_t fit = (rdr(h, kRegFmInt) & 0x80000000u) ^ 0x80000000u;   // FrameIntervalToggle flips after a reset (read after it)
  wrr(h, kRegFmInt, fit | ((((fi - 210) * 6 / 7) & 0x7fffu) << 16) | fi);
  wrr(h, kRegPerStart, fi * 9 / 10);
  wrr(h, kRegLsThr, 0x628);
  wrr(h, kRegCtl, kCtlCbsr | kCtlPle | kCtlCle | (kHcfsOperational << kCtlHcfsShift));
  const uint32_t ctl = rdr(h, kRegCtl);
  if (((ctl >> kCtlHcfsShift) & 3) != kHcfsOperational) {
    printf("opi5: usb: %s did not go operational (ctrl %#x)\n", h->name, ctl);
    return false;
  }
  h->ports = rhPorts(h);
  rhPower(h);
  h->started = true;
  printHostStatus(h, "started");
  return true;
}

// ---- root hub ports
bool rhResetPort(Host *h, unsigned port) {
  wrr(h, kRegRhPort + 4 * port, kPortPrs);
  uint32_t ps = 0;
  for (unsigned i = 0; i < 40; ++i) {
    delayUs(5000);
    ps = portStatus(h, port);
    if (ps & kPortPrsc) break;
  }
  wrr(h, kRegRhPort + 4 * port, kPortPrsc);       // clear the reset-complete change bit
  delayUs(20000);                                 // reset recovery
  ps = portStatus(h, port);
  if (!(ps & kPortPes)) {
    wrr(h, kRegRhPort + 4 * port, kPortPes);
    delayUs(10000);
    ps = portStatus(h, port);
  }
  return (ps & kPortPes) != 0;
}

// ---- control transfers on the default pipe: SETUP, optional DATA, STATUS, then the dummy TD
bool ctrlTransfer(Host *h, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void *data, uint16_t length) {
  if (length > kDataMax) return false;
  const bool in = (bmRequestType & 0x80) != 0;
  volatile OhciEd *ed = edAt(h, kOffCtrlEd);
  volatile OhciTd *setup = tdAt(h, kOffCtrlTd), *dat = tdAt(h, kOffCtrlTd + 0x10), *status = tdAt(h, kOffCtrlTd + 0x20),
                  *dummy = tdAt(h, kOffCtrlTd + 0x30);
  const uint32_t setupBus = busAt(h, kOffCtrlTd), dataBus = busAt(h, kOffCtrlTd + 0x10), statusBus = busAt(h, kOffCtrlTd + 0x20),
                 dummyBus = busAt(h, kOffCtrlTd + 0x30);
  const uint32_t pktBus = busAt(h, kOffSetup), bufBus = busAt(h, kOffData);
  const uint32_t edf = edFlags(h->addr, 0, 0, h->low, h->mps0);

  // Take the ED out of play (a transfer that timed out may still be queued), then rebuild it while it is skipped.
  ed->flags = edf | kEdSkip;
  dmaFlush(ed, sizeof(OhciEd));
  delayUs(3000);                                  // the skip bit is looked at once per frame

  volatile uint8_t *pkt = bufAt(h, kOffSetup);
  pkt[0] = bmRequestType;
  pkt[1] = bRequest;
  pkt[2] = static_cast<uint8_t>(wValue);
  pkt[3] = static_cast<uint8_t>(wValue >> 8);
  pkt[4] = static_cast<uint8_t>(wIndex);
  pkt[5] = static_cast<uint8_t>(wIndex >> 8);
  pkt[6] = static_cast<uint8_t>(length);
  pkt[7] = static_cast<uint8_t>(length >> 8);

  setup->cbp = pktBus;
  setup->be = pktBus + 7;
  setup->nextTd = length ? dataBus : statusBus;
  setup->flags = kTdCcNew | kTdDi | kTdSetup | kTdData0;
  if (length) {
    volatile uint8_t *buf = bufAt(h, kOffData);
    if (!in)
      for (unsigned i = 0; i < length; ++i) buf[i] = static_cast<uint8_t *>(data)[i];
    else
      for (unsigned i = 0; i < length; ++i) buf[i] = 0;
    dat->cbp = bufBus;
    dat->be = bufBus + length - 1;
    dat->nextTd = statusBus;
    dat->flags = kTdCcNew | kTdDi | kTdData1 | (in ? (kTdIn | kTdR) : kTdOut);
  }
  status->cbp = 0;
  status->be = 0;
  status->nextTd = dummyBus;
  status->flags = kTdCcNew | kTdDi | kTdData1 | (in && length ? kTdOut : kTdIn);
  dummy->flags = 0;
  dummy->cbp = 0;
  dummy->be = 0;
  dummy->nextTd = 0;
  dmaFlush(h->dma.virt + kOffCtrlTd, 0x40);
  dmaFlush(h->dma.virt + kOffSetup, 0x200);

  ed->tailP = dummyBus;
  ed->headP = setupBus;                           // halted and carry bits cleared
  ed->nextEd = 0;
  dmaFlush(ed, sizeof(OhciEd));
  ed->flags = edf;                                // skip off: the HC takes it from here
  dmaFlush(ed, sizeof(OhciEd));
  wrr(h, kRegCmd, kCmdClf);

  bool done = false, halted = false;
  for (unsigned i = 0; i < 500; ++i) {
    delayUs(1000);
    dmaFlush(h->dma.virt + kOffCtrlTd, 0x40);
    dmaFlush(ed, sizeof(OhciEd));
    if ((status->flags >> 28) != 0xf) {
      done = true;
      break;
    }
    if (ed->headP & kEdHalt) {
      halted = true;
      break;
    }
  }
  const uint32_t ccSetup = setup->flags >> 28, ccData = length ? dat->flags >> 28 : 0, ccStatus = status->flags >> 28;
  wrr(h, kRegIntSt, rdr(h, kRegIntSt));
  if (!done || ccSetup || ccData || ccStatus) {
    if (h->failures++ < 8)
      printf("opi5: usb: %s control request %#x/%#x failed (%s): cc setup %#x data %#x status %#x, ed head %#x\n", h->name, bmRequestType,
             bRequest, halted ? "halted" : done ? "bad completion" : "timeout", ccSetup, ccData, ccStatus, ed->headP);
    return false;
  }
  if (in && length) {
    dmaFlush(bufAt(h, kOffData), length);
    volatile uint8_t *buf = bufAt(h, kOffData);
    for (unsigned i = 0; i < length; ++i) static_cast<uint8_t *>(data)[i] = buf[i];
  }
  return true;
}

// ---- HID boot devices on interrupt IN endpoints
void armTd(Host *h, unsigned hid, unsigned idx, unsigned next) {
  HidDev *d = &h->hid[hid];
  volatile OhciTd *t = tdAt(h, kOffHidTd + 0x30 * hid + 0x10 * idx);
  const uint32_t buf = busAt(h, kOffHidBuf + 0x30 * hid + 0x10 * idx);
  for (unsigned i = 0; i < 16; ++i) bufAt(h, kOffHidBuf + 0x30 * hid + 0x10 * idx)[i] = 0;
  t->cbp = buf;
  t->be = buf + d->maxPacket - 1;
  t->nextTd = busAt(h, kOffHidTd + 0x30 * hid + 0x10 * next);
  t->flags = kTdCcNew | kTdDi | kTdR | kTdIn;     // toggle from the ED's carry (DATA0 after SET_CONFIGURATION)
  dmaFlush(t, sizeof(OhciTd));
  dmaFlush(bufAt(h, kOffHidBuf + 0x30 * hid + 0x10 * idx), 16);
}

void scheduleHid(Host *h, unsigned hid) {
  HidDev *d = &h->hid[hid];
  volatile OhciEd *ed = edAt(h, kOffHidEd + 0x10 * hid);
  ed->flags = edFlags(h->addr, d->ep, 2, h->low, d->maxPacket) | kEdSkip;
  ed->tailP = busAt(h, kOffHidTd + 0x30 * hid + 0x10);
  ed->headP = busAt(h, kOffHidTd + 0x30 * hid);
  ed->nextEd = 0;
  d->cur = 0;
  armTd(h, hid, 0, 1);
  dmaFlush(ed, sizeof(OhciEd));
  ed->flags = edFlags(h->addr, d->ep, 2, h->low, d->maxPacket);
  dmaFlush(ed, sizeof(OhciEd));
  d->inUse = true;
}

void linkPeriodic(Host *h) {
  volatile uint32_t *table = reinterpret_cast<volatile uint32_t *>(h->dma.virt + kOffHcca);
  uint32_t first = 0;
  if (h->hid[0].inUse) first = busAt(h, kOffHidEd);
  if (h->hid[1].inUse) {
    if (first) edAt(h, kOffHidEd)->nextEd = busAt(h, kOffHidEd + 0x10);
    else first = busAt(h, kOffHidEd + 0x10);
  }
  dmaFlush(h->dma.virt + kOffHidEd, 0x20);
  for (unsigned i = 0; i < 32; ++i) table[i] = first;   // every frame
  dmaFlush(table, 128);
}

void publishReport(HidDev *d, const uint8_t *data) {
  if (d->isMouse && d->layout.valid) {
    // report protocol: the fields where the report descriptor put them (a report of another ID, or a short one, is not a mouse report)
    const MouseLayout &l = d->layout;
    const unsigned skip = l.hasId ? 1 : 0;
    if (l.hasId && data[0] != l.reportId) return;
    const unsigned length = (d->maxPacket > skip ? d->maxPacket - skip : 0);
    const int32_t buttons = fieldValue(data + skip, length, l.buttons), dx = fieldValue(data + skip, length, l.x),
                  dy = fieldValue(data + skip, length, l.y), wheel = fieldValue(data + skip, length, l.wheel);
    native_usb_mouse[0] = static_cast<uint8_t>(buttons & 7);
    native_usb_mouse[1] = static_cast<uint8_t>(clamp8(dx));
    native_usb_mouse[2] = static_cast<uint8_t>(clamp8(dy));
    native_usb_mouse[3] = static_cast<uint8_t>(clamp8(wheel));
    __atomic_fetch_add(&native_usb_mouse_dx, dx, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_usb_mouse_dy, dy, __ATOMIC_RELAXED);
    if (wheel) {
      __atomic_fetch_add(&native_usb_mouse_wheel, wheel, __ATOMIC_RELAXED);
      __atomic_fetch_add(&native_usb_mouse_wheel_total, static_cast<uint32_t>(wheel < 0 ? -wheel : wheel), __ATOMIC_RELAXED);
    }
    bumpSeq(&native_usb_mouse_seq);
  } else if (d->isMouse) {
    for (unsigned i = 0; i < 4; ++i) native_usb_mouse[i] = i < d->maxPacket ? data[i] : 0;
    if (d->maxPacket > 2) {   // boot protocol: buttons, X, Y (signed 8 bit, relative), then the wheel on mice that send four bytes
      __atomic_fetch_add(&native_usb_mouse_dx, (int32_t)(int8_t)data[1], __ATOMIC_RELAXED);
      __atomic_fetch_add(&native_usb_mouse_dy, (int32_t)(int8_t)data[2], __ATOMIC_RELAXED);
      if (d->maxPacket > 3 && data[3]) {
        __atomic_fetch_add(&native_usb_mouse_wheel, (int32_t)(int8_t)data[3], __ATOMIC_RELAXED);
        __atomic_fetch_add(&native_usb_mouse_wheel_total, (uint32_t)((int8_t)data[3] < 0 ? -(int8_t)data[3] : (int8_t)data[3]), __ATOMIC_RELAXED);
      }
    }
    bumpSeq(&native_usb_mouse_seq);
  } else {
    for (unsigned i = 0; i < 8; ++i) native_usb_kbd[i] = data[i];
    bumpSeq(&native_usb_kbd_seq);
  }
  bumpSeq(&native_usb_hid_reports);
}

void serviceHid(Host *h, unsigned hid) {
  HidDev *d = &h->hid[hid];
  if (!d->inUse) return;
  volatile OhciTd *t = tdAt(h, kOffHidTd + 0x30 * hid + 0x10 * d->cur);
  dmaFlush(t, sizeof(OhciTd));
  const uint32_t cc = t->flags >> 28;
  if (cc == 0xf) return;                                  // the HC has not retired it yet (a NAK keeps it queued)
  const unsigned nxt = (d->cur + 1) % 3, nn = (d->cur + 2) % 3;
  volatile OhciEd *ed = edAt(h, kOffHidEd + 0x10 * hid);
  if (cc == 0) {
    uint8_t report[16];
    volatile uint8_t *buf = bufAt(h, kOffHidBuf + 0x30 * hid + 0x10 * d->cur);
    dmaFlush(buf, 16);
    for (unsigned i = 0; i < 16; ++i) report[i] = buf[i];
    publishReport(d, report);
    ++gPollReports;
  } else {
    if (gPollErrors++ < 8) printf("opi5: usb: %s hid %u transfer failed, condition code %#x\n", h->name, hid, cc);
    ed->headP = busAt(h, kOffHidTd + 0x30 * hid + 0x10 * nxt);   // clears the halt bit (and the toggle carry)
  }
  // The retired TD's successor is the ED's dummy tail: fill it in, give it a new dummy behind it, move the tail.
  armTd(h, hid, nxt, nn);
  ed->tailP = busAt(h, kOffHidTd + 0x30 * hid + 0x10 * nn);
  dmaFlush(ed, sizeof(OhciEd));
  d->cur = nxt;
}

// ---- enumeration
bool parseHidBoot(const uint8_t *cfg, unsigned total, HidDev *slot, bool wantMouse) {
  unsigned off = 0;
  bool inIface = false;
  uint8_t iface = 0;
  uint16_t reportDescLen = 0;
  while (off + 2 <= total) {
    const uint8_t len = cfg[off];
    if (!len || off + len > total) break;
    const uint8_t type = cfg[off + 1];
    if (type == 4 && len >= 9) {
      inIface = cfg[off + 5] == 3 && cfg[off + 6] == 1 && cfg[off + 7] == (wantMouse ? 2 : 1);   // HID, boot subclass, keyboard 1 / mouse 2
      iface = cfg[off + 2];
      reportDescLen = 0;
    } else if (type == 0x21 && inIface && len >= 9 && cfg[off + 6] == 0x22) {   // the HID class descriptor: the size of the report descriptor
      reportDescLen = static_cast<uint16_t>(cfg[off + 7] | (cfg[off + 8] << 8));
    } else if (type == 5 && inIface && len >= 7 && (cfg[off + 2] & 0x80) && (cfg[off + 3] & 3) == 3) {
      slot->isMouse = wantMouse;
      slot->iface = iface;
      slot->reportDescLen = reportDescLen;
      slot->ep = cfg[off + 2] & 0x0f;
      slot->maxPacket = static_cast<uint8_t>((cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7ff);
      if (slot->maxPacket > 16) slot->maxPacket = 16;
      if (!slot->maxPacket) slot->maxPacket = wantMouse ? 4 : 8;
      return true;
    }
    off += len;
  }
  return false;
}

void teardown(Host *h) {
  for (unsigned i = 0; i < 2; ++i) {
    if (h->hid[i].inUse) edAt(h, kOffHidEd + 0x10 * i)->flags = edAt(h, kOffHidEd + 0x10 * i)->flags | kEdSkip;
    h->hid[i].inUse = false;
  }
  volatile uint32_t *table = reinterpret_cast<volatile uint32_t *>(h->dma.virt + kOffHcca);
  for (unsigned i = 0; i < 32; ++i) table[i] = 0;
  dmaFlush(h->dma.virt, kOffCtrlEd);
  dmaFlush(h->dma.virt + kOffHidEd, 0x20);
  delayUs(3000);
  h->enumerated = false;
  h->addr = 0;
  h->mps0 = 8;
  printf("opi5: usb: %s device removed\n", h->name);
}

bool enumerate(Host *h, unsigned port) {
  delayUs(150000);                                // the device has been attached for at least 100 ms before the reset
  if (!(portStatus(h, port) & kPortCcs)) return false;
  h->low = (portStatus(h, port) & kPortLsda) != 0;
  h->addr = 0;
  h->mps0 = 8;
  if (!rhResetPort(h, port)) {
    printf("opi5: usb: %s port %u: not enabled after reset (status %#x)\n", h->name, port, portStatus(h, port));
    return false;
  }
  printf("opi5: usb: %s port %u: %s speed device, status %#x\n", h->name, port, h->low ? "low" : "full", portStatus(h, port));
  uint8_t d8[8] = {};
  if (!ctrlTransfer(h, 0x80, kUsbReqGetDescriptor, 0x0100, 0, d8, 8)) return false;   // the first packet tells the control endpoint's size
  h->mps0 = d8[7] ? d8[7] : 8;
  if (!ctrlTransfer(h, 0x00, kUsbReqSetAddress, 1, 0, nullptr, 0)) return false;
  delayUs(10000);
  h->addr = 1;
  uint8_t dev[18] = {};
  if (!ctrlTransfer(h, 0x80, kUsbReqGetDescriptor, 0x0100, 0, dev, 18)) return false;
  printf("opi5: usb: %s device vid %#x pid %#x class %u mps0 %u\n", h->name, dev[8] | (dev[9] << 8), dev[10] | (dev[11] << 8), dev[4], h->mps0);
  if (dev[4] == 9) {
    printf("opi5: usb: %s a hub: not supported (plug the keyboard / mouse into the board directly)\n", h->name);
    return false;
  }
  if (gMode < 3) return true;
  uint8_t cfg[kDataMax] = {};
  if (!ctrlTransfer(h, 0x80, kUsbReqGetDescriptor, 0x0200, 0, cfg, 9)) return false;
  unsigned total = cfg[2] | (cfg[3] << 8);
  if (total > kDataMax) total = kDataMax;
  if (total > 9 && !ctrlTransfer(h, 0x80, kUsbReqGetDescriptor, 0x0200, 0, cfg, static_cast<uint16_t>(total))) return false;
  HidDev kbd, mouse;
  const bool haveKbd = parseHidBoot(cfg, total, &kbd, false);
  const bool haveMouse = parseHidBoot(cfg, total, &mouse, true);
  printf("opi5: usb: %s configuration %u bytes: boot keyboard %s (iface %u ep %u mps %u), boot mouse %s (iface %u ep %u mps %u)\n", h->name, total,
         haveKbd ? "yes" : "no", kbd.iface, kbd.ep, kbd.maxPacket, haveMouse ? "yes" : "no", mouse.iface, mouse.ep, mouse.maxPacket);
  if (!haveKbd && !haveMouse) return false;
  if (!ctrlTransfer(h, 0x00, kUsbReqSetConfiguration, cfg[5], 0, nullptr, 0)) return false;
  if (haveKbd) {
    ctrlTransfer(h, 0x21, kHidReqSetProtocol, 0, kbd.iface, nullptr, 0);   // boot protocol; a device that stalls these still works
    ctrlTransfer(h, 0x21, kHidReqSetIdle, 0, kbd.iface, nullptr, 0);
  }
  if (haveMouse) {
    // Report protocol, read from the report descriptor, unless opi5-usb-boot-mouse=1 or the descriptor is not one this parser understands: the boot
    // protocol is three bytes (buttons, X, Y), a wheel is only in the report protocol (a mouse set to boot protocol never sent it).
    bool report = false;
    if (!bootArg("opi5-usb-boot-mouse", 0) && mouse.reportDescLen) {
      uint8_t desc[kDataMax] = {};
      const unsigned want = mouse.reportDescLen > kDataMax ? kDataMax : mouse.reportDescLen;
      if (ctrlTransfer(h, 0x81, kUsbReqGetDescriptor, 0x2200, mouse.iface, desc, static_cast<uint16_t>(want))) {
        char text[3 * 40 + 1] = {};
        for (unsigned i = 0; i < want && i < 40; ++i) snprintf(text + 3 * i, 4, "%02x ", desc[i]);
        printf("opi5: usb: %s mouse report descriptor %u bytes: %s%s\n", h->name, mouse.reportDescLen, text, want > 40 ? "..." : "");
        MouseLayout layout;
        if (want == mouse.reportDescLen && parseMouseLayout(desc, want, &layout) && layout.length <= mouse.maxPacket) {
          mouse.layout = layout;
          report = true;
          printf("opi5: usb: %s mouse report protocol: id %d, %u bytes, buttons@%d/%u x@%d/%u y@%d/%u wheel@%d/%u\n", h->name,
                 layout.hasId ? layout.reportId : -1, layout.length, layout.buttons.bit, layout.buttons.size, layout.x.bit, layout.x.size, layout.y.bit,
                 layout.y.size, layout.wheel.bit, layout.wheel.size);
        } else {
          printf("opi5: usb: %s mouse report descriptor not understood (or longer than the endpoint's packet), boot protocol\n", h->name);
        }
      } else {
        printf("opi5: usb: %s mouse report descriptor not readable, boot protocol\n", h->name);
      }
    }
    ctrlTransfer(h, 0x21, kHidReqSetProtocol, report ? 1 : 0, mouse.iface, nullptr, 0);
    ctrlTransfer(h, 0x21, kHidReqSetIdle, 0, mouse.iface, nullptr, 0);
    native_usb_mouse_report_protocol = report ? 1 : 0;
    if (!report) printf("opi5: usb: %s mouse in boot protocol: no wheel\n", h->name);
  }
  if (gMode < 4) return true;
  if (haveKbd) {
    h->hid[0] = kbd;
    scheduleHid(h, 0);
  }
  if (haveMouse) {
    h->hid[1] = mouse;
    scheduleHid(h, 1);
  }
  linkPeriodic(h);
  h->enumerated = true;
  printf("opi5: usb: %s HID polling started (keyboard %d, mouse %d)\n", h->name, haveKbd, haveMouse);
  return true;
}

void pollHost(Host *h) {
  if (!h->started || !h->ports) return;
  const uint32_t ps = portStatus(h, 0);
  if (h->enumerated) {
    if (!(ps & kPortCcs)) {
      teardown(h);
      return;
    }
    serviceHid(h, 0);
    serviceHid(h, 1);
    return;
  }
  if (!(ps & kPortCcs)) {
    h->failures = 0;
    h->tries = 0;
    h->lastTry = 0;
    return;
  }
  const uint64_t now = nowUs();
  if (h->lastTry && now - h->lastTry < 2000000) return;   // after a failure, try again every 2 s
  h->lastTry = now;
  if (!enumerate(h, 0) && h->tries++ < 3) printf("opi5: usb: %s enumeration failed (try %u)\n", h->name, h->tries);
}

void usbThread(void *, wait_result_t) {
  phyResume();
  hostPowerEnable();
  delayUs(50000);
  Host *hosts[2] = {&gHost0, &gHost1};
  if (!mapHost(&gHost0, kOhci0Phys, kEhci0Phys, "ohci0") || !mapHost(&gHost1, kOhci1Phys, kEhci1Phys, "ohci1")) {
    printf("opi5: usb: cannot map the OHCI registers\n");
    return;
  }
  for (Host *h : hosts) {
    probeEhci(h);
    if (!dmaAlloc(h)) continue;
    printHostStatus(h, "as the firmware left it");
    if (gMode >= 2 && startController(h) && gMode == 2) printHostStatus(h, "mode 2");
  }
  if (gMode < 3) return;
  printf("opi5: usb: waiting for devices (mode %u)\n", gMode);
  uint64_t last = nowUs();
  while (true) {
    for (Host *h : hosts) pollHost(h);
    ++gPollFrames;
    delayUs(1000);
    const uint64_t now = nowUs();
    if (now - last > 10000000) {
      if (gHost0.enumerated || gHost1.enumerated)
        printf("opi5: usb: poll frames=%u reports=%u errors=%u hid_total=%u kbd_seq=%u mouse_seq=%u wheel=%u proto=%u\n", gPollFrames, gPollReports, gPollErrors,
               native_usb_hid_reports, native_usb_kbd_seq, native_usb_mouse_seq, native_usb_mouse_wheel_total, native_usb_mouse_report_protocol);
      gPollFrames = gPollReports = 0;
      last = now;
    }
  }
}

}  // namespace

// opi5-usb=N: 0 off (this board's default: the argument has to be given; upstream opi5 defaults to 5); 1 look only (EHCI / OHCI
// registers as the firmware left them); 2 also start the OHCIs and power the ports; 3 also enumerate what is plugged in (device
// and configuration descriptors); 4 also configure it as a boot keyboard / mouse; 5 also poll its interrupt endpoint and publish
// the reports.
extern "C" void opi5_usb_start(void) {
  logResetStatus();
  gMode = bootArg("opi5-usb", 0);
  if (!gMode) return;
  thread_t thread = nullptr;
  if (kernel_thread_start(usbThread, nullptr, &thread) == KERN_SUCCESS && thread) thread_deallocate(thread);
}
