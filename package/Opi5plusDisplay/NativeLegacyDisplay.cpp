#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" int opi5plus_fill_vram_table(void *surfaceRoot);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
// A swap the request validator accepted but the copy refused: the surface half and the firmware console half of the
// test, both recorded, so the periodic report can say which one disagreed without a debugger.
extern "C" void native_legacy_transfer_diagnosed(uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,
    uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,uint32_t,uint64_t);
// The pid of the framebuffer user client, captured at newUserClient (LegacyDisplayProbe.cpp): the source
// process a validated or rejected swap belongs to.
extern "C" volatile int native_legacy_client_pid;
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// Swaps the family gave up on after waiting five seconds for them (cancelSwapsAfterTimeout below).
extern "C" volatile uint64_t native_legacy_swap_timeouts;
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
// The compositor's swapped surfaces, for a display that fetches them itself: [0] distinct surfaces seen, [1] swaps checked; for the first four, at
// [2 + 3 * k]: allocation bytes, physical segments, largest segment (KiB). The pages are wired for the count and released.
extern "C" {volatile uint32_t native_legacy_surfaces[14];}
// Direct scanout (boot-arg evb10-vop-direct=1; this file is the RK3588 EVB10 fork of drivers/GopFramebuffer/NativeLegacyDisplay.cpp, whose
// SM8550 MDP pipes became the VOP2 window behind the VOP's IOMMU, package/LegacyDisplayProbe.cpp): the window fetches the compositor's
// surface itself, through an IOMMU mapping of its pages, so nothing is copied. [0] flips, [1] surfaces mapped, [2] map failures,
// [3] latches that took over 50 ms, [4] longest latch (us), [5] latch total (us, for the mean), [6] surfaces let go, [7] swaps that fell
// back to the copy, [8] flips back to the boot memory.
// With evb10-vop-fence=1: [9] swaps that had to wait for the GPU, [10] their waits' total (us), [11] the longest (us), [12] waits that gave up (250 ms),
// [13] swaps seen when they arrived, [14] of them, swaps that arrived with the render server's GPU work not done.
extern "C" {volatile uint32_t native_legacy_direct[16];}
// GOP mirror (boot-arg opi5-gop-mirror=1, default on): the frame the window has just latched is also copied into the
// firmware's GOP frame buffer, and the window is pointed back at that buffer. The direct path's only unmeasured link is
// the VOP's IOMMU translation of the compositor's surface; this puts the same pixels in the one buffer the firmware
// itself scanned, and it runs the content scan, so the report says what the compositor produced. [0] blits,
// [1] refusals, [2] bytes of the last blit, [3] bytes in total, [4] the last blit in microseconds, [5] the longest.
extern "C" {volatile uint64_t native_legacy_gop_mirror[6];}
extern "C" int mali_fence_take(uint64_t *entries,unsigned cap);
extern "C" int mali_fence_done(const uint64_t *entries,unsigned count);
extern "C" void mali_fence_describe(const uint64_t *entries,unsigned count);
extern "C" void evb10_vop_direct_check(uint32_t iova);
extern "C" bool evb10_vop_direct_init(void);
extern "C" uint32_t evb10_vop_direct_map(unsigned slot,const uint64_t *pages,unsigned count);
extern "C" void evb10_vop_direct_flip(uint32_t iova,uint32_t stride);
extern "C" bool evb10_vop_direct_latched(void);
extern "C" void evb10_vop_direct_boot(uint32_t *iova,uint32_t *stride);
static void noteSurface(OSObject *surface,IOMemoryDescriptor *memory)
{
    static OSObject *seen[4];
    native_legacy_surfaces[1]=native_legacy_surfaces[1]+1;
    for(OSObject *known:seen)if(known==surface)return;
    const unsigned k=native_legacy_surfaces[0];
    native_legacy_surfaces[0]=k+1;
    if(k>=4)return;
    seen[k]=surface;
    if(memory->prepare()!=kIOReturnSuccess)return;
    uint32_t segments=0;uint64_t largest=0;
    for(IOByteCount offset=0,length=0;offset<memory->getLength() && segments<100000;offset+=length,++segments) {
        if(!memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone) || !length)break;
        if(length>largest)largest=length;
    }
    memory->complete();
    native_legacy_surfaces[2+3*k]=uint32_t(memory->getLength());native_legacy_surfaces[3+3*k]=segments;native_legacy_surfaces[4+3*k]=uint32_t(largest>>10);
}
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
// Physical-word read, for the readback below: the surface's first pixels are read through their physical address, the
// same way package/Rk3588VopDirect reads the page it is about to scan.
extern "C" unsigned int ml_phys_read_word(unsigned long paddr);
// The instance the probe's late paint and readback reach. A void * because the class is defined below.
static void *lateDisplayInstance=nullptr;
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spellings ('&BGA', one plane; 'b3a8', the compositor's two-plane
// surfaces): the GPU that renders the surface here writes plain rows either way (no compression is implemented), so the bytes are the
// same. A two-plane surface is copied out of its first plane only, at the row nativeSourceStride below derives for it.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1) ||
        (format==0x62336138 && planes<=2);
}
// The row the copy reads, not the row the surface reports. The compositor's two-plane 'b3a8' surface reports the last plane's row
// (width bytes, the one-byte plane) through native_legacy_surface_stride, while the pixels sit in plane 0: its allocation is exactly
// height*(width*4+width), so plane 0 is a width*4-byte row and no padding stands between the two planes. Only a surface exactly as
// wide as the display is derived (a wider one's visible row is its own width, which this backend cannot see); every other surface
// reports the row it is copied with.
static inline uint64_t nativeSourceStride(OSObject *surface,uint32_t width) {
    if(native_legacy_surface_format(surface)==0x62336138 && native_legacy_surface_planes(surface)==2 &&
       native_legacy_surface_width(surface)==width)return (uint64_t)width*4;
    return native_legacy_surface_stride(surface);
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    uint32_t scanBudget=32;
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    // Direct scanout state, used on the work loop only. VOP2's window on video port 3 (esmart 3; package/LegacyDisplayProbe.cpp sets it up for
    // the boot framebuffer): the frame's base address and stride change, VOP_CTRL's config-done latches them at the next frame start. The
    // base is an address in the VOP IOMMU's space, where the compositor's surfaces are mapped four slots of 16 MiB each, and where the boot
    // framebuffer is mapped at its own physical address.
    struct DirectSlot {OSObject *surface;IOMemoryDescriptor *memory;uint32_t iova;uint64_t used;};
    static constexpr unsigned kDirectSlots=4;
    DirectSlot directSlots[kDirectSlots]={};
    uint64_t *directPages=nullptr;
    IOTimerEventSource *latchTimer=nullptr;
    const OriginalLegacySwapRequest *latchRequest=nullptr;
    uint64_t latchStart=0,directClock=0,latchLength=0;
    int shownSlot=-1,pendingSlot=-1;
    bool directOn=false,directBoot=true,directFlush=false,gopMirror=false;
    uint32_t bootIova=0,bootStride=0;
    // The fence (boot-arg evb10-vop-fence=1): the compositor swaps a surface as soon as its GPU work is queued, not finished, and the panel
    // would show the half-drawn frame. The swap's flip waits until every job submitted before the swap arrived has completed (the Mali
    // driver's syncobjs), polling on a timer so that the work loop and the compositor's thread stay free.
    static constexpr unsigned kFenceCap=24;
    bool fenceOn=false,fenceCheck=false;
    IOTimerEventSource *fenceTimer=nullptr;
    // What the GPU still had to do when each swap arrived (taken in validateSwap: jobs that were submitted after it are not its business, and
    // some of them wait for the swap's own present): per request, until its flip.
    struct FenceSnap {const OriginalLegacySwapRequest *request;int count;uint64_t entries[3*kFenceCap];};
    FenceSnap fenceSnaps[8]={};
    unsigned fenceSnapNext=0;
    const OriginalLegacySwapRequest *fenceRequest=nullptr;
    uint64_t fenceEntries[3*kFenceCap]={};
    unsigned fenceCount=0;
    uint64_t fenceStart=0;
    FenceSnap *fenceSnapFor(const OriginalLegacySwapRequest *request) {
        for(auto &snap: fenceSnaps)if(snap.request==request)return &snap;
        return nullptr;
    }
    void fenceNote(const OriginalLegacySwapRequest *request) {
        if(!fenceOn || !request || fenceSnapFor(request))return;
        FenceSnap &snap=fenceSnaps[fenceSnapNext++%8];
        const int n=mali_fence_take(snap.entries,kFenceCap);
        if(n==-1)return;   // the driver was busy: the snapshot is taken when the flip comes
        snap.request=request;snap.count=n<0?-2:n;
        native_legacy_direct[13]=native_legacy_direct[13]+1;
        if(n!=0)native_legacy_direct[14]=native_legacy_direct[14]+1;
    }
    unsigned checkedFlips=0;
    void startDirect() {
        unsigned mode=0;PE_parse_boot_argn("evb10-vop-direct",&mode,sizeof(mode));
        if(mode<1)PE_parse_boot_argn("rk3588-vop-direct",&mode,sizeof(mode));
        if(mode<1 || !primary)return;
        directPages=static_cast<uint64_t *>(IOMallocData(8192*sizeof(uint64_t)));
        IOWorkLoop *loop=getWorkLoop();
        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;
        if(!directPages || !latchTimer || loop->addEventSource(latchTimer)!=kIOReturnSuccess || !evb10_vop_direct_init()) {
            OSSafeReleaseNULL(latchTimer);
            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
            return;
        }
        evb10_vop_direct_boot(&bootIova,&bootStride);
        unsigned flush=0;PE_parse_boot_argn("evb10-vop-direct-flush",&flush,sizeof(flush));directFlush=flush==1;
        // On by default: without it nothing in this kernel writes the GOP frame buffer once the direct path owns
        // scanout (opi5_gop_present() in package/LegacyDisplayProbe.cpp is the letterbox of a portrait kernel surface,
        // and its gate needs evb10-fb-w/-h, which the payload does not pass). opi5-gop-mirror=0 keeps the old behaviour.
        unsigned mirror=1;PE_parse_boot_argn("opi5-gop-mirror",&mirror,sizeof(mirror));
        gopMirror=mirror==1 && bootIova && bootStride;
        unsigned fence=0;PE_parse_boot_argn("evb10-vop-fence",&fence,sizeof(fence));
        if(fence<1)PE_parse_boot_argn("rk3588-vop-fence",&fence,sizeof(fence));
        unsigned check=0;PE_parse_boot_argn("evb10-vop-fence-check",&check,sizeof(check));
        if(check<1)PE_parse_boot_argn("rk3588-vop-fence-check",&check,sizeof(check));
        fenceCheck=check==1;
        if(fence==1) {
            fenceTimer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::fenceFired);
            if(fenceTimer && loop->addEventSource(fenceTimer)==kIOReturnSuccess)fenceOn=true;
            else OSSafeReleaseNULL(fenceTimer);
        }
        directOn=true;
    }
    static void fenceFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->kickSwapQueue();}
    // True when the flip may go ahead: nothing the GPU was given before this swap is still running. False (and the timer armed) otherwise.
    bool fenceClear(const OriginalLegacySwapRequest *request) {
        if(!fenceOn)return true;
        if(fenceRequest!=request) {
            if(FenceSnap *snap=fenceSnapFor(request)) {
                fenceCount=snap->count<0?~0u:unsigned(snap->count);
                memcpy(fenceEntries,snap->entries,sizeof(fenceEntries));
            } else {
                const int n=mali_fence_take(fenceEntries,kFenceCap);
                if(n==-1){fenceTimer->setTimeoutUS(200);return false;}
                fenceCount=n<0?~0u:unsigned(n);
            }
            fenceRequest=request;fenceStart=mach_absolute_time();
        }
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-fenceStart,&ns);
        if(fenceCount) {
            const int done=mali_fence_done(fenceEntries,fenceCount);
            if(done!=1 && ns<250000000ull){fenceTimer->setTimeoutUS(300);return false;}
            if(done!=1){native_legacy_direct[12]=native_legacy_direct[12]+1;mali_fence_describe(fenceEntries,fenceCount);}
        }
        if(ns>500000ull) {   // it had to wait (the take and the first poll alone take less)
            const uint32_t us=uint32_t(ns/1000);
            native_legacy_direct[9]=native_legacy_direct[9]+1;native_legacy_direct[10]=native_legacy_direct[10]+us;
            if(us>native_legacy_direct[11])native_legacy_direct[11]=us;
        }
        if(FenceSnap *snap=fenceSnapFor(request))snap->request=nullptr;
        fenceRequest=nullptr;fenceCount=0;
        return true;
    }
    void flip(uint32_t iova,uint32_t stride) {evb10_vop_direct_flip(iova,stride);}
    // The slot that holds this surface, mapping it into a free one (never the one on screen or the one waiting to latch) the first time.
    int directSlot(OSObject *surface,IOMemoryDescriptor *memory) {
        ++directClock;
        int pick=-1;
        for(unsigned k=0;k<kDirectSlots;++k)if(directSlots[k].surface==surface && directSlots[k].memory==memory){directSlots[k].used=directClock;return int(k);}
        for(unsigned k=0;k<kDirectSlots;++k) {
            if(int(k)==shownSlot || int(k)==pendingSlot)continue;
            if(pick<0 || !directSlots[k].surface || (directSlots[pick].surface && directSlots[k].used<directSlots[pick].used))pick=int(k);
        }
        if(pick<0)return -1;
        DirectSlot &slot=directSlots[pick];
        if(slot.surface) {
            slot.memory->complete();OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);
            native_legacy_direct[6]=native_legacy_direct[6]+1;
        }
        if(memory->getLength()>8192ull*4096 || memory->prepare()!=kIOReturnSuccess){native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        unsigned count=0;bool ok=true;
        for(IOByteCount offset=0;offset<memory->getLength() && ok;) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!pa || (pa&0xfff) || !length){ok=false;break;}
            for(IOByteCount at=0;at<length && offset+at<memory->getLength();at+=4096)directPages[count++]=pa+at;
            offset+=length;
        }
        const uint32_t iova=ok?evb10_vop_direct_map(unsigned(pick),directPages,count):0;
        if(!iova){memory->complete();native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        surface->retain();memory->retain();
        slot={surface,memory,iova,directClock};
        native_legacy_direct[1]=native_legacy_direct[1]+1;
        return pick;
    }
    static void latchFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->latchTick();}
    // The frame the window has just latched, copied into the firmware's GOP frame buffer with the window pointed back at
    // it (opi5-gop-mirror, default on, see startDirect). The destination is the mapping the boot framebuffer already has
    // (bootMapping), the stride is the firmware's own row, and the source is the compositor's surface the VOP just
    // scanned, so the panel shows the same pixels through a physical address the firmware itself lit. The content scan
    // of the direct path runs here (it used to run only on the CPU copy), so "frames" finally describes the frames the
    // direct path presents. Returns true when the blit happened.
    bool gopMirrorPresent(int slot) {
        if(!gopMirror || !bootMapping || slot<0 || slot>=int(kDirectSlots))return false;
        DirectSlot &direct=directSlots[slot];
        if(!direct.surface || !direct.memory)return false;
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        const uint64_t stride=direct.surface?nativeSourceStride(direct.surface,width):0;
        PE_Video video={};
        const bool shaped=width && height && stride>=(uint64_t)width*4 && stride<=UINT64_MAX/height &&
            !PE_current_console(&video) && video.v_baseAddr==bootMapping->getPhysicalAddress() &&
            video.v_width==width && video.v_height==height && video.v_depth==32 && !video.v_rotate &&
            (uint64_t)video.v_rowBytes*height<=bootMapping->getLength();
        IOMemoryMap *source=shaped?direct.memory->map(kIOMapAnywhere|kIOMapReadOnly):nullptr;
        const bool mapped=source && source->getVirtualAddress() && source->getLength()>=stride*height;
        if(!mapped) {
            OSSafeReleaseNULL(source);
            native_legacy_gop_mirror[1]=native_legacy_gop_mirror[1]+1;
            const uint64_t n=native_legacy_gop_mirror[1];
            if(n<=3)
                IOLog("opi5plus: display: gop blit: refused #%llu (%s%ux%u stride %llu, window %ux%u%d%d)\n",
                    (unsigned long long)n,shaped?"map failed: ":"shape refuses: ",width,height,(unsigned long long)stride,
                    (unsigned)video.v_width,(unsigned)video.v_height,(int)video.v_depth,(int)video.v_rotate);
            return false;
        }
        const void *pixels=reinterpret_cast<const void *>(source->getVirtualAddress());
        // The two content scans the CPU copy path runs, now also for a directly scanned frame: the first frames only.
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(pixels,source->getLength(),stride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(pixels,source->getLength(),stride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        uint64_t start;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(start));
        const bool copied=native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),(uint64_t)video.v_rowBytes,pixels,source->getLength(),stride,width,height)!=0;
        if(!copied) {
            OSSafeReleaseNULL(source);
            native_legacy_gop_mirror[1]=native_legacy_gop_mirror[1]+1;
            const uint64_t n=native_legacy_gop_mirror[1];
            if(n<=3)IOLog("opi5plus: display: gop blit: refused #%llu (copy layout, %ux%u stride %llu)\n",
                (unsigned long long)n,width,height,(unsigned long long)stride);
            return false;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        OSSafeReleaseNULL(source);
        shadowValid=false;
        // The window had the compositor's surface; it takes the frame buffer that now holds the same pixels.
        flip(bootIova,bootStride);directBoot=true;shownSlot=-1;
        uint64_t end,frequency;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(end));
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
        const uint64_t bytes=stride*height,us=frequency?(end-start)*1000000ull/frequency:0;
        native_legacy_gop_mirror[0]=native_legacy_gop_mirror[0]+1;
        const uint64_t n=native_legacy_gop_mirror[0];
        native_legacy_gop_mirror[2]=bytes;native_legacy_gop_mirror[3]=native_legacy_gop_mirror[3]+bytes;
        native_legacy_gop_mirror[4]=us;if(us>native_legacy_gop_mirror[5])native_legacy_gop_mirror[5]=us;
        if(n<=3 || (n%200)==0)
            IOLog("opi5plus: display: gop blit: %llu bytes #%llu into %#llx %ux%u row %u (%llu us)\n",
                (unsigned long long)bytes,(unsigned long long)n,(unsigned long long)bootMapping->getPhysicalAddress(),
                width,height,(unsigned)video.v_rowBytes,(unsigned long long)us);
        return true;
    }
    void latchTick() {
        if(!latchRequest)return;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchStart,&ns);
        if(!evb10_vop_direct_latched() && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}
        if(ns>=50000000ull)native_legacy_direct[3]=native_legacy_direct[3]+1;
        const uint32_t us=uint32_t(ns/1000);
        if(us>native_legacy_direct[4])native_legacy_direct[4]=us;
        native_legacy_direct[5]=native_legacy_direct[5]+us;
        shownSlot=pendingSlot;pendingSlot=-1;
        const OriginalLegacySwapRequest *request=latchRequest;latchRequest=nullptr;
        gopMirrorPresent(shownSlot);
        native_legacy_transfer_observed(kIOReturnSuccess,request,latchLength);
        finishTransfer(request,true);
    }
    // Back to the boot loader's memory (the blank frame and the copy are drawn there).
    void directToBoot() {
        if(!directOn || directBoot)return;
        flip(bootIova,bootStride);directBoot=true;shownSlot=-1;
        native_legacy_direct[8]=native_legacy_direct[8]+1;
    }
    // What the window is being told to scan, and what the two candidate buffers hold at their first pixel. Read-only:
    // every word comes from state the direct path already keeps, so this can run on any report beat without changing
    // what the system does (evidence/m7/M7-06). The five answers it separates:
    //   window iova that was written           -> row 1 of the report table
    //   the surface's first physical segment   -> does the IOMMU translate what we think it does
    //   the surface's first four pixels        -> all zero: the GPU never drew into it; equal to the boot pixels:
    //                                             the slot maps the boot framebuffer, not the compositor's surface
    //   the boot framebuffer's first pixels    -> the control for the row above
    void readback(uint64_t *out) {
        for(unsigned i=0;i<11;++i)out[i]=0;
        out[0]=shownSlot>=0?directSlots[shownSlot].iova:bootIova;
        IOMemoryDescriptor *memory=shownSlot>=0?directSlots[shownSlot].memory:nullptr;
        if(memory) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
            if(pa && length) {
                out[1]=pa;out[10]=1;
                for(unsigned i=0;i<4;++i)out[2+i]=ml_phys_read_word(static_cast<unsigned long>(pa+4*i));
            }
        }
        if(bootMapping && bootMapping->getVirtualAddress()) {
            const volatile uint32_t *pixels=reinterpret_cast<const volatile uint32_t *>(bootMapping->getVirtualAddress());
            for(unsigned i=0;i<4;++i)out[6+i]=pixels[i];
        }
    }
    // Take the window back to the boot framebuffer and fill it with a colour nothing else draws. Called once by the
    // probe (offline-legacy-paint=1) after the direct path has flipped and then stopped moving, so one look at the
    // panel answers "does the VOP read the address we program" without waiting for the compositor.
    int latePaint(uint32_t color) {
        if(!directOn || !bootMapping || !primary)return 0;
        PE_Video video={};
        if(PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress())return 0;
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        if(!width || !height || (uint64_t)width*height*4>bootMapping->getLength())return 0;
        if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,width,height,color))return 0;
        __asm__ volatile("dsb sy" ::: "memory");
        shadowValid=false;
        directToBoot();
        return 1;
    }
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // The probe's two windows into the direct path (both declared in the private section, where the state they read
    // lives): the readback that says what the window is told to scan, and the one-shot paint that says whether the
    // panel follows it (evidence/m7/M7-06).
    void probeReadback(uint64_t *out) {readback(out);}
    int probeLatePaint(uint32_t color) {return latePaint(color);}
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {stopVsync();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;gpuRetired=false;
        directToBoot();latchRequest=nullptr;pendingSlot=-1;
        if(fenceTimer){fenceTimer->cancelTimeout();if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(fenceTimer);OSSafeReleaseNULL(fenceTimer);}
        fenceOn=false;fenceRequest=nullptr;
        if(latchTimer){latchTimer->cancelTimeout();if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(latchTimer);OSSafeReleaseNULL(latchTimer);}
        directOn=false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();startDirect();
        lateDisplayInstance=this;
        return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        supplyDefaultSurface();
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    // The user client's default_fb_surface() reads the surface pointer at +5656, which only the platform driver
    // on a real device writes; the surface the family itself creates is kept at +5664 instead, and the creator
    // (slot 299) returns immediately unless the byte at +6016 is set -- another field that belongs to the
    // platform driver this port does not ship (evidence/m5/M5-16 sections 2 and 3).
    //
    // +6016 is set here and not during start() on purpose: the first attempt set it before the original start(),
    // the creator ran with no IOSurfaceRoot service behind it, and the family dereferenced NULL inside its layout
    // path (panic 0xfffffff00578d110, esr 0x96000005, far 0, x1 0xa00 x2 0x5a0 -- this port's 2560x1440).
    // So: only when the service the creator needs is really there, and only once, with the result checked.
    void supplyDefaultSurface() {
        uint8_t *base=reinterpret_cast<uint8_t *>(this);
        void *surfaceRoot=nullptr;
        OSObject *created=nullptr,*installed=nullptr;
        memcpy(&surfaceRoot,base+5632,sizeof(surfaceRoot));memcpy(&installed,base+5656,sizeof(installed));
        if(installed) {
            IOLog("opi5plus: display: default surface already present %p, left alone\n",installed);return;
        }
        if(!surfaceRoot) {
            // This port loads the IOSurface kext from the probe, after the family's start() has already looked
            // for the service, so the family's own lookup can lose the race and leave +5632 empty. Resolve it
            // here and store it where the family expects it; the creator reads the same field.
            OSDictionary *matching=IOService::nameMatching("IOCoreSurfaceRoot");
            IOService *service=matching?IOService::waitForMatchingService(matching,2000000000ULL):nullptr;
            if(matching)matching->release();
            if(service) {
                surfaceRoot=service;memcpy(base+5632,&surfaceRoot,sizeof(surfaceRoot));
                IOLog("opi5plus: display: IOSurfaceRoot %p stored at +5632 for the family\n",service);
            }
        }
        if(!surfaceRoot) {
            IOLog("opi5plus: display: no IOSurfaceRoot service at +5632, so the family cannot create a default "
                "surface and default_fb_surface() keeps answering kIOReturnError\n");return;
        }
        // The region table, before anything else asks for a surface. Without an entry every IOSurface this port
        // makes is region-less, which is what stops adoptDefaultSurface() from publishing one and what made
        // layout_buffers() fault on the family's own surface (evidence/m5/M5-17). IOSurfaceRoot::start() fills
        // the entry only from IORegistryEntry::fromPath("/vram"), which answers 0 here (evidence/m5/M5-18), so
        // write the same fields start() writes and run the installer it keeps at +0x188. Verified against the
        // service's own code at 0xfffffff004ba93ac..0xfffffff004ba93e0: names +0x158 (8-byte stride), aliases
        // +0x168, cache word +0x178 (4-byte stride), count +0x180, installer +0x188, always set by start().
        //
        // OFF by default, because it was tried on the board and it stops userland surface allocation instead of
        // helping it (run of 2026-11-10, evidence/m7/M7-07): the table took ("region count after installer 1")
        // and from then on the render server created no GPU surface at all -- "mali: user: status: J surface
        // cache n=0 bytes=0", no swap ever submitted in 1330 s, where the same payload without the entry makes
        // three frames at t+935. An entry whose region has no memory behind it (there is no /vram node for the
        // installer to attach a range to) is worse than no entry, so the honest default is the empty table.
        unsigned fillRegions=0;PE_parse_boot_argn("offline-legacy-vram-region",&fillRegions,sizeof(fillRegions));
        if(fillRegions) {
            uint32_t present=0;memcpy(&present,reinterpret_cast<uint8_t *>(surfaceRoot)+0x180,sizeof(present));
            if(present) IOLog("opi5plus: display: region table already holds %u entries, left alone\n",present);
            else (void)opi5plus_fill_vram_table(surfaceRoot);
        }
        else IOLog("opi5plus: display: region table left empty (offline-legacy-vram-region=0)\n");
        // Forcing the gate on is an experiment, not the default: with it on, the family's creator ran (it did not
        // crash) and produced nothing, and a later family path then dereferenced NULL at 0xfffffff00578d110 with
        // these very dimensions (esr 0x96000005, far 0) and reset the board. Two runs, same signature. So the byte
        // stays 0 unless offline-legacy-default-surface=1 asks for the experiment.
        unsigned experiment=0;PE_parse_boot_argn("offline-legacy-default-surface",&experiment,sizeof(experiment));
        if(!experiment) {
            IOLog("opi5plus: display: default surface not attempted (offline-legacy-default-surface=0); the "
                "creator is gated and forcing it on panics later at 0xfffffff00578d110\n");return;
        }
        *reinterpret_cast<uint8_t *>(base+6016)=1;
        // =2 sets the gate and does not call the creator: that separates "the byte itself enables a path that
        // faults" from "our call leaves state behind that faults", which the two panicking runs cannot.
        if(experiment>=3) {
            // No gate: run display39 showed that the byte alone is enough to make the family create a
            // region-less surface and fault at 0xfffffff00578d110, whether or not this code calls the creator.
            // The table is already filled above; this path asks the creator for a region-backed surface and
            // publishes it only when it carries one, so the family's own creator never runs.
            publishSystemMemorySurface(surfaceRoot);
            return;
        }
        {
            // The table was filled above, synchronously, by the same installer start() schedules. Calling
            // start() again here (as display44..50 did) rebuilds every object the service owns a second time and
            // is not needed for the region: only the count is worth reading back.
            uint32_t regions=0;
            memcpy(&regions,reinterpret_cast<uint8_t *>(surfaceRoot)+0x180,sizeof(regions));
            IOLog("opi5plus: display: IOSurfaceRoot region count %u before the creator\n",regions);
        }
        if(experiment==2) {
            IOLog("opi5plus: display: gate +6016 set, creator not called (offline-legacy-default-surface=2)\n");
            return;
        }
        createDefaultFBSurface();
        memcpy(&created,base+5664,sizeof(created));
        if(!created) {
            IOLog("opi5plus: display: create_default_fb_surface() produced no surface (+5664 null) even with "
                "IOSurfaceRoot %p; trying manual creator with region key\n",surfaceRoot);
            publishSystemMemorySurface(surfaceRoot);
            memcpy(&created,base+5656,sizeof(created));
            if(created) {
                IOLog("opi5plus: display: manual creator produced region-backed surface %p, adopting as default\n",created);
                return;
            }
            IOLog("opi5plus: display: manual creator also produced nothing; default surface unavailable\n");
            return;
        }
        uint32_t identifier=0;memcpy(&identifier,reinterpret_cast<uint8_t *>(created)+16,sizeof(identifier));
        void *region=nullptr;memcpy(&region,reinterpret_cast<uint8_t *>(created)+0x28,sizeof(region));
        if(!region) {IOLog("opi5plus: display: created surface has no memory region, not publishing\n");return;}
        created->retain();
        memcpy(base+5656,&created,sizeof(created));
        IOLog("opi5plus: display: default surface %p (id %u) published at +5656, where default_fb_surface looks\n",
            created,identifier);
    }
    static void setSurfaceString(OSDictionary *dict,const char *key,const char *value) {
        OSString *string=OSString::withCString(value);
        if(!string)return;
        dict->setObject(key,string);string->release();
    }
    static void setSurfaceNumber(OSDictionary *dict,const char *key,uint64_t value) {
        OSNumber *number=OSNumber::withNumber(value,32);
        if(!number)return;
        dict->setObject(key,number);number->release();
    }
    // =3: the family's creator asks IOSurface for a surface in the "PurpleGfxMem" region, which on a device is
    // the /vram node the platform driver publishes and which this port does not have (evidence/m5/M5-17). A
    // surface made without that key falls back to system memory, which is all this port's presentation needs:
    // the frame reaches the screen by CPU copy, not by a display-list fetch out of /vram.
    //
    // Two things are checked before anything is published: the creator's first instruction (so a layout change
    // cannot turn this into a call into nowhere) and the surface's memory region at +0x28. The family's
    // layout_buffers() dereferences that region unconditionally at 0xfffffff00578d110, so publishing a surface
    // without one is exactly the reset loop of runs display35c and display36.
    void publishSystemMemorySurface(void *surfaceRoot) {
        uint8_t *base=reinterpret_cast<uint8_t *>(this);
        uint32_t width=0,height=0;
        memcpy(&width,base+5808,sizeof(width));memcpy(&height,base+5812,sizeof(height));
        if(!width || !height) {
            // Run display39 reached this point before the family's own start() had published dimensions
            // (+5808/+5812 were still zero), so take them from the console the firmware handed over.
            PE_Video video;
            if(!PE_current_console(&video)) {
                width=video.v_width;height=video.v_height;
            }
        }
        if(!width || !height) {
            IOLog("opi5plus: display: no boot dimensions (%ux%u), no default surface\n",width,height);return;
        }
        typedef OSObject *(*Creator)(void *,void *,OSDictionary *,void *);
        Creator creator=reinterpret_cast<Creator>(static_cast<uintptr_t>(0xfffffff004baaa24ull));
        if(*reinterpret_cast<const uint32_t *>(creator)!=0xd2800003u) {
            IOLog("opi5plus: display: the IOSurface creator routine is not where this build expects it "
                "(%#x), not calling it\n",*reinterpret_cast<const uint32_t *>(creator));return;
        }
        OSDictionary *properties=OSDictionary::withCapacity(7);
        if(!properties) {
            IOLog("opi5plus: display: manual creator: no properties dict\n");return;
        }
        IOLog("opi5plus: display: manual creator: dims %ux%u root %p\n",width,height,surfaceRoot);
        // Region key removed 2026-11-08 (display58 hangs the probe thread inside the creator when the key is present).
        // The creator with the key blocks waiting for a region object that never materializes; without it the call fails fast.
        // setSurfaceString(properties,"IOSurfaceMemoryRegion","PurpleGfxMem");
        setSurfaceNumber(properties,"IOSurfaceWidth",width);
        setSurfaceNumber(properties,"IOSurfaceHeight",height);
        setSurfaceNumber(properties,"IOSurfaceBytesPerRow",static_cast<uint64_t>(width)*4);
        setSurfaceNumber(properties,"IOSurfaceBytesPerElement",4);
        setSurfaceNumber(properties,"IOSurfaceAllocSize",static_cast<uint64_t>(width)*4*height);
        setSurfaceNumber(properties,"IOSurfacePixelFormat",0x42475241u);  // 'BGRA'
        IOLog("opi5plus: display: manual creator: calling %p...\n",creator);
        OSObject *surface=creator(surfaceRoot,nullptr,properties,nullptr);
        IOLog("opi5plus: display: manual creator: creator returned %p\n",surface);
        properties->release();
        if(!surface) {
            IOLog("opi5plus: display: the IOSurface creator returned nothing for %ux%u\n",width,height);return;
        }
        uint8_t *bytes=reinterpret_cast<uint8_t *>(surface);
        void *region=nullptr,*descriptor=nullptr;uint32_t identifier=0;
        memcpy(&region,bytes+0x28,sizeof(region));memcpy(&descriptor,bytes+0x30,sizeof(descriptor));
        memcpy(&identifier,bytes+0x10,sizeof(identifier));
        IOLog("opi5plus: display: created surface %p id %u region %p descriptor %p for %ux%u\n",surface,
            identifier,region,descriptor,width,height);
        if(!region) {
            IOLog("opi5plus: display: that surface has no memory region, so layout_buffers() would fault on it; "
                "not publishing\n");surface->release();return;
        }
        memcpy(base+5656,&surface,sizeof(surface));
        IOLog("opi5plus: display: default surface %p published at +5656 (system memory)\n",surface);
    }
    // The family's default_fb_surface() needs an IOSurface at +5656 and that surface must carry a memory region:
    // the family's layout_buffers() dereferences the region unconditionally at 0xfffffff00578d110, which is what
    // reset runs display35c, display36, display39 and display40 (evidence/m5/M5-17 section 5). The surfaces this
    // port can get are the compositor's own, so adopt the first one the display is asked to present -- and only
    // after reading its region, so a region-less surface is never published.
    void adoptDefaultSurface(OSObject *surface) {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-adopt-surface",&enabled,sizeof(enabled));
        static bool reported=false;
        void *installed=nullptr;
        if(!enabled || !surface)return;
        memcpy(&installed,reinterpret_cast<uint8_t *>(this)+5656,sizeof(installed));
        if(installed)return;
        void *region=nullptr;memcpy(&region,reinterpret_cast<uint8_t *>(surface)+0x28,sizeof(region));
        if(!region) {
            if(!reported) {
                reported=true;
                IOLog("opi5plus: display: the compositor's surface %p has no memory region either, so the family "
                    "still has no default surface to lay out\n",surface);
            }
            return;
        }
        surface->retain();
        void *value=surface;memcpy(reinterpret_cast<uint8_t *>(this)+5656,&value,sizeof(value));
        reported=true;
        uint32_t identifier=0;memcpy(&identifier,reinterpret_cast<uint8_t *>(surface)+16,sizeof(identifier));
        IOLog("opi5plus: display: adopted the compositor's surface %p (id %u, region %p) as the default surface\n",
            surface,identifier,region);
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>=60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // The original maximum interval is 1/24 s (int-240), which offers the render server a 24 to sheng-vsync Hz range. The panel has no
    // variable refresh, so sheng-vsync-fixed=1 reports the maximum interval equal to the minimum: one fixed rate.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                // A blank swap (no surface, every layer off) refused before any store: record the same console half the
                // surface path records, with a zero surface, so the report says which of the two halves disagreed.
                native_legacy_transfer_diagnosed(0,0,0,0,0,0,0,(uint64_t)blank.v_baseAddr,(uint32_t)blank.v_width,
                    (uint32_t)blank.v_height,(uint32_t)blank.v_depth,(uint32_t)blank.v_rotate,
                    (uint64_t)bootMapping->getPhysicalAddress(),bw,bh,(uint64_t)blank.v_rowBytes);
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            directToBoot();
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=nativeSourceStride(surface,width);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_diagnosed(memory?memory->getLength():0,native_legacy_surface_width(surface),
                native_legacy_surface_height(surface),native_legacy_surface_format(surface),native_legacy_surface_planes(surface),
                sourceStride,allocation,(uint64_t)video.v_baseAddr,(uint32_t)video.v_width,(uint32_t)video.v_height,
                (uint32_t)video.v_depth,(uint32_t)video.v_rotate,(uint64_t)bootMapping->getPhysicalAddress(),width,height,
                (uint64_t)video.v_rowBytes);
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        noteSurface(surface,memory);
        adoptDefaultSurface(surface);
        if(directOn && !gpuBypass && !latchRequest) {
            const int slot=directSlot(surface,memory);
            if(slot>=0) {
                if(!fenceClear(request))return;
                native_legacy_swap_stage(1);
                // The VOP reads memory itself: the CPU's dirty lines of a frame the CPU drew must reach it first (a frame the GPU drew was
                // flushed by the renderer; evb10-vop-direct-flush=1 does it here for the frames that were not).
                if(directFlush)memory->performOperation(kIOMemoryIncoherentIOFlush,0,memory->getLength());
                if(fenceCheck && checkedFlips<400) {++checkedFlips;evb10_vop_direct_check(directSlots[slot].iova);}
                flip(directSlots[slot].iova,uint32_t(sourceStride));directBoot=false;
                native_legacy_direct[0]=native_legacy_direct[0]+1;
                latchLength=length;
                shadowValid=false;pendingSlot=slot;latchRequest=request;transferred=true;
                latchStart=mach_absolute_time();latchTimer->setTimeoutUS(500);
                return;
            }
            native_legacy_direct[7]=native_legacy_direct[7]+1;
        }
        directToBoot();
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof(early));
            if(early==1 && retireSwap(request,true)) {
                gpuRetired=true;native_legacy_early_retires=native_legacy_early_retires+1;
            }
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired=false;
            if(status==0)native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            else {native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_gpu_fallback_observed(status);gpuBypass=true;}
            transferred=false;kickSwapQueue();return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!retireSwap(request,force))return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied. The row bounded here is the row the copy
                    // will read (nativeSourceStride: plane 0 of a two-plane surface), not the last plane's row the accessor reports.
                    const uint64_t sourceStride=nativeSourceStride(surface,width);
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,sourceStride,
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                        // Once, for the first two-plane 'b3a8' frame: the row derived for the copy, the row the surface reported, and the
                        // allocation the derivation rests on. A picture that skews evenly across rows says plane 0's row is padded.
                        static bool biplanarReported=false;
                        if(!biplanarReported && native_legacy_surface_format(surface)==0x62336138 && native_legacy_surface_planes(surface)==2) {
                            biplanarReported=true;
                            printf("opi5: display: biplanar surface accepted, derived stride %llu (accessor said %llu), allocation %llu\n",
                                (unsigned long long)sourceStride,(unsigned long long)native_legacy_surface_stride(surface),
                                (unsigned long long)native_legacy_surface_allocation(surface));
                        }
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(!accepted) {
            // The rejected swap's own evidence, for the white rounded squares: the geometry of the surface the request
            // carried (the original IOSurface getters, null-checked here) and the pid of the user client that opened the
            // framebuffer. A rejecting surface can recur at swap rate (the early touch3 surface rejected every frame),
            // so this logs at most once per minute and says how many rejections it swallowed since; the totals stay in
            // the existing counters (native_legacy_swap_rejected_count and the telemetry call below).
            static uint64_t rejectLoggedUs,rejectsSkipped;
            uint64_t nowNs=0;absolutetime_to_nanoseconds(mach_absolute_time(),&nowNs);
            const uint64_t nowUs=nowNs/1000;
            if(nowUs-rejectLoggedUs>=60000000ull) {
                rejectLoggedUs=nowUs;
                if(surface)IOLog("opi5plus: display: swap rejected (reason %u, skipped %llu since): surface %llu x %llu format %#x rowBytes %llu, request %ux%u, client pid %d\n",
                    reason,(unsigned long long)rejectsSkipped,(unsigned long long)native_legacy_surface_width(surface),
                    (unsigned long long)native_legacy_surface_height(surface),native_legacy_surface_format(surface),
                    (unsigned long long)native_legacy_surface_stride(surface),word(816),word(820),native_legacy_client_pid);
                else IOLog("opi5plus: display: swap rejected (reason %u, skipped %llu since): no surface, request %ux%u, client pid %d\n",
                    reason,(unsigned long long)rejectsSkipped,word(816),word(820),native_legacy_client_pid);
                rejectsSkipped=0;
            } else ++rejectsSkipped;
        }
        if(accepted){addValidated(request);fenceNote(request);}else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=true;printf("opi5: display: startHardware %ux%u v_display %u -> primary forced\n",(unsigned)video.v_width,(unsigned)video.v_height,(unsigned)video.v_display);
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
    // swap_wait_gated gave up on a swap that did not complete in five seconds ("CommandWake didn't happen even
    // after 5 seconds ... cancelling swaps") and calls this before cancelling its own queue. Forget the swaps this
    // class tracks so a copy that ends later finds no request, and so the next frame is not held behind a cancelled
    // one. This slot used to be a guard, which turned the family's timeout into a kernel panic and killed the run
    // before the next client could try (GopFramebuffer's reference backend implements it the same way).
    bool cancelSwapsAfterTimeout() override {
        native_legacy_swap_timeouts=native_legacy_swap_timeouts+1;
        if(fenceTimer)fenceTimer->cancelTimeout();
        fenceOn=false;fenceRequest=nullptr;
        transferred=false;for(auto &entry: validated)entry=nullptr;
        gpuRequest=nullptr;gpuRetired=false;
        printf("opi5: display: swap cancelled after the 5 s gate timeout (%llu so far)\n",
            (unsigned long long)native_legacy_swap_timeouts);
        return true;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}

// /vram: the display memory region IOSurface builds its "PurpleGfxMem" pool from.
//
// IOSurfaceRoot::start() (0xfffffff004ba90bc) fills its region table only when IORegistryEntry::fromPath("/vram")
// finds a node, and installMemoryRegions() then builds the pool from that node; with no node the table stays
// empty, every IOSurface is region-less, and the family's layout_buffers() faults on the null region at
// 0xfffffff00578d110 (evidence/m5/M5-17). This board has no platform driver that publishes the node, so the
// display archive publishes it: a plain IOService named "vram" under the service root, carrying the range the
// pool may hand out. The range is the 4 GiB DRAM bank at 0x1_0000_0000, which the kernel does NOT own --
// [payload.adt] owned_memory_size is 2 GiB inside the firmware's low bank -- so nothing the VM allocates can
// overlap it.
class Opi5plusVram : public IOService {
    OSDeclareDefaultStructors(Opi5plusVram)
public:
    bool start(IOService *provider) override {
        if(!IOService::start(provider))return false;
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(Opi5plusVram,IOService)

// IOKit/IODeviceTreeSupport.h is not in this archive's include set; the plane object it declares is.
extern const IORegistryPlane *gIODTPlane;

// Fill IOSurfaceRoot's region table directly and run the installer it keeps at +0x188. The path lookup that
// would normally fill it (IORegistryEntry::fromPath("/vram") inside the service's start()) answers 0 on this
// port even with the node attached to the registry root, so the table is written where start() would have
// written it (evidence/m5/M5-17 sections 4 and 8).
extern "C" int opi5plus_fill_vram_table(void *surfaceRoot) {
    static char regionName[]="vram";
    static char regionAlias[]="PurpleGfxMem";
    if(!surfaceRoot)return 0;
    uint8_t *root=static_cast<uint8_t *>(surfaceRoot);
    uint64_t namePointer=reinterpret_cast<uint64_t>(regionName);
    uint64_t aliasPointer=reinterpret_cast<uint64_t>(regionAlias);
    memcpy(root+0x158,&namePointer,sizeof(namePointer));
    memcpy(root+0x168,&aliasPointer,sizeof(aliasPointer));
    uint32_t cacheWord=0x100;memcpy(root+0x178,&cacheWord,sizeof(cacheWord));
    uint32_t count=1;memcpy(root+0x180,&count,sizeof(count));
    typedef void (*Installer)(void *);
    Installer installer=nullptr;memcpy(&installer,root+0x188,sizeof(installer));
    IOLog("opi5plus: display: region table filled (1 entry %s/%s), installer %p\n",regionName,regionAlias,installer);
    if(installer)installer(surfaceRoot);
    {
        uint32_t after=0;
        memcpy(&after,root+0x180,sizeof(after));
        IOLog("opi5plus: display: region count after installer %u\n",after);
        for(int i=0;i<20;i++) {
            IOSleep(50);
            uint32_t now=0;
            memcpy(&now,root+0x180,sizeof(now));
            if(now!=after) {
                IOLog("opi5plus: display: region count changed %u -> %u at wait %d\n",after,now,i);
                after=now;
                break;
            }
        }
        IOLog("opi5plus: display: region count settled %u after installer wait\n",after);
    }
    return 1;
}

extern "C" int opi5plus_publish_vram(void) {
    static int result=-1;
    if(result>=0)return result;
    result=0;
    IOService *root=IOService::getServiceRoot();
    if(!root) {
        IOLog("opi5plus: display: no service root, /vram not published\n");
        return 0;
    }
    Opi5plusVram *vram=new Opi5plusVram;
    if(!vram || !vram->init()) {
        IOLog("opi5plus: display: /vram allocation failed\n");
        if(vram)vram->release();
        return 0;
    }
    vram->setName("vram");
    const uint64_t range[2]={0x100000000ull,0x10000000ull};  // 256 MiB at 0x1_0000_0000
    OSData *reg=OSData::withBytes(range,sizeof(range));
    if(reg) {vram->setProperty("reg",reg);reg->release();}
    vram->setProperty("device_type","vram");
    // fromPath("/vram") wants a child of the ROOT entry, and getServiceRoot() hands back the platform expert
    // ("J120AP"), one level too deep: run display45 showed the node under 'J120AP' and fromPath("/vram") == 0.
    IORegistryEntry *topEntry=IORegistryEntry::getRegistryRoot();
    if(!topEntry) {
        IOLog("opi5plus: display: no root entry above the platform expert, /vram not published\n");
        vram->release();
        return 0;
    }
    if(!vram->attachToParent(topEntry,gIOServicePlane)) {
        IOLog("opi5plus: display: /vram could not attach to the root entry\n");
        vram->release();
        return 0;
    }
    if(!vram->start(root)) {
        IOLog("opi5plus: display: /vram attached but did not start\n");
        return 0;
    }
    result=1;
    {
        // The real node on a device comes from the device tree, so the entry is put in that plane as well and
        // both planes are reported: run display46 attached it under the root in the service plane and
        // fromPath("/vram") still answered 0, which leaves the plane in question as the thing to pin down.
        IORegistryEntry *treeRoot=root->getParentEntry(gIODTPlane);
        if(treeRoot)vram->attachToParent(treeRoot,gIODTPlane);
        // getPath() is the registry's own answer: if registerService() re-parented the node under the provider
        // (the platform expert), the path says so even though attachToParent() put it at the root.
        char pathBuffer[128]={0};int pathLength=sizeof(pathBuffer);
        vram->getPath(pathBuffer,&pathLength,gIOServicePlane);
        IORegistryEntry *inService=IORegistryEntry::fromPath("/vram",gIOServicePlane);
        IORegistryEntry *inTree=IORegistryEntry::fromPath("/vram",gIODTPlane);
        const char *parentName=root->getName();
        IOLog("opi5plus: display: /vram path '%s' (provider '%s'); fromPath service %p tree %p\n",
            pathBuffer,parentName?parentName:"?",inService,inTree);
    }
    IOLog("opi5plus: display: /vram published, 256 MiB at 0x100000000, for IOSurface's region table\n");
    return 1;
}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
// Called by the probe once the IOSurface kext is loaded and IOCoreSurfaceRoot answers: the family's start() ran
// before that, so the default surface has to be created afterwards (evidence/m5/M5-16).
extern "C" void native_legacy_publish_default_surface(void *display) {
    if(display)static_cast<NativeLegacyDisplay *>(display)->supplyDefaultSurface();
}
// Called by the probe on a report beat: what the window is told to scan and what the two candidate buffers hold.
// Read-only, so a failing answer cannot make a working system worse (evidence/m7/M7-06).
extern "C" int native_legacy_readback(uint64_t *out,unsigned words) {
    if(!out || words<11 || !lateDisplayInstance)return 0;
    static_cast<NativeLegacyDisplay *>(lateDisplayInstance)->probeReadback(out);
    return 1;
}
// Called by the probe once (offline-legacy-paint=1): fill the boot framebuffer green and point the window at it.
extern "C" int native_legacy_late_paint(uint32_t color) {
    if(!lateDisplayInstance)return 0;
    return static_cast<NativeLegacyDisplay *>(lateDisplayInstance)->probeLatePaint(color);
}
