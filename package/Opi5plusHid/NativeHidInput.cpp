// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline keyboard/consumer input source. Uses the unchanged original
// HID parser and event dispatch; no user-client entitlement is substituted.
//
// NativeHidInput.cpp of ShengHidInput (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors), for the RK3588 EVB10 (2026-10-28):
//  - the finger set comes from package/Evb10Touch, which already scales to panel pixels (0..1079 x 0..1919), so the two axis maxima
//    below are the panel's and the edge band is 24 panel pixels (12 pt at the 540 x 960 pt the display runs at);
//  - this board has no hardware buttons driver, so the weak button sink is gone (nothing else of the class or its vtable changed:
//    scripts/build-evb10-hid.py audits the same ten overridden slots).
//  - 2026-11-13 (S2): the USB HID mailbox consumer of UniXNU-orangepi5's Evb10Hid (pin 729ba25f) is merged in:
//    the weak native_usb_* externs, mouseDescriptor, usbHidFlush()/mouseFlush() and the mouseOnly device form
//    (native_hid_mouse_allocate). The injector (opi5-hid-input>=2) and every report it emits are unchanged.
//  - 2026-11-13 (round 20): after the first completed gesture the injector enters app-launch tapping: it
//    stops repeating the unlock cycle and taps the App Store icon of the 2560 x 1440 panel once every 30 s
//    (injectTapTick below), so an app-open wedge reproduces unattended. The gesture itself, the Home channel
//    and the USB mailbox flush are unchanged.
//  - 2026-11-13 (round 21): the unlock gesture's trigger moves off the open gate. The kernel's own
//    IOHIDEventDriver opens this device at t+0, so the old gate released at t+3 and the gesture landed
//    before anything listened. The trigger now: backboardd has spawned (patch 0006's spawn tracker sets
//    native_spawn_seen_backboardd at the posix_spawn top) AND the display has completed at least one swap
//    (LegacyDisplayProbe's native_legacy_swap_accepted_count, the parity line's "swap ok"), then 30 s of
//    quiet before the first gesture; 10 minutes without both falls back to the old driver/open gate. The
//    open gate itself stays, bounded as before, as the second filter.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
extern "C" uint32_t evb10_fb_w,evb10_fb_h;   // the kernel's own frame buffer (0 without one), xnu-patches/userland-mounts
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" {extern volatile unsigned native_hid_input_mode;}   // package/Opi5plusHidProbe.cpp: opi5-hid-input/evb10-hid-input
extern "C" void native_touch_set_sink(void (*)(void *), void *);   // package/Evb10Touch
// The touch digitizer fed by the board touch driver (package/Evb10Touch): finger set published with a seqlock.
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
// package/Opi5plusUsb: boot-protocol HID mailbox (weak when USB is not linked, the declaration form
// package/LegacyDisplayProbe.cpp uses for opi5_usb_start). The names are the S1 producer's
// (package/Opi5plusUsb/NativeUsb.cpp:40-50): kbd_seq / mouse_seq bump on every report, kbd[8] /
// mouse[4] keep the newest report's bytes, and mouse_dx/dy/wheel accumulate the motion between
// consumer takes. The producer's own diagnostics (native_usb_mouse_wheel_total,
// native_usb_mouse_report_protocol) are not consumed here, as upstream's consumer does not either.
extern volatile uint32_t native_usb_hid_reports __attribute__((weak_import));
extern volatile uint32_t native_usb_kbd_seq __attribute__((weak_import));
extern volatile uint8_t native_usb_kbd[8] __attribute__((weak_import));
extern volatile uint32_t native_usb_mouse_seq __attribute__((weak_import));
extern volatile uint8_t native_usb_mouse[4] __attribute__((weak_import));
extern volatile int32_t native_usb_mouse_dx __attribute__((weak_import));
extern volatile int32_t native_usb_mouse_dy __attribute__((weak_import));
extern volatile int32_t native_usb_mouse_wheel __attribute__((weak_import));
// Round 21 unlock gate, the two conditions of the deferred gesture (weak for the same reason the USB
// mailbox is: both producers sit behind hand-maintained code paths and the driver must stay linkable
// without them). native_spawn_seen_backboardd is set at patch 0006's posix_spawn top the moment the
// target's basename is backboardd -- the trace switch opi5plus-trace-spawn only gates the printing,
// not this flag. native_legacy_swap_accepted_count is LegacyDisplayProbe's own swap counter, the
// "swap ok" of the parity line it prints every 30 s.
extern volatile int native_spawn_seen_backboardd __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
}
// Keyboard Application collection ensures the original generic keyboard
// personality can match. Report 2 carries the Consumer buttons, bit 0 Menu (Home),
// bit 1 Power, bit 2 Volume Increment, bit 3 Volume Decrement. Report 1 is an
// ordinary boot keyboard report.
static const unsigned char descriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
// The USB mouse (package/Opi5plusUsb) is a device of its own, as it is on hardware: backboardd and the HID event system tell a pointing
// device by the primary usage of the service (Generic Desktop / Mouse), and a mouse collection behind the keyboard's would make the
// service a keyboard. Report 4: three buttons, X, Y, wheel (relative, one byte each), the layout of a boot mouse report plus the wheel.
//
// The original kernel has no event driver personality for a pointer: a Mouse collection matches only the DriverKit
// AppleUserHIDEventDriver (IOProbeScore 0x4b1, IOUserClass of AppleUserHIDDrivers.dext), which then waits 120 s for a dext process that
// driverkitd never starts ("DK: ... user server timeout"): the reports were accepted by handleReport and no event ever came out (EVB10
// board, 2026-11-05). Opi5plusHidProbe's addKernelPointerPersonality adds a personality of the original IOHIDEventDriver (kernel) for
// USB pointers with a higher score.
static const unsigned char mouseDescriptor[]={
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x04,
    0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x05,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,
    0xc0,0xc0
};
// Report 3: ten-finger Touch Screen collection (Tip Switch, Touch Valid, Contact Identifier, X, Y per finger, then Contact Count).
// Touch Valid (0x47) is the finger confidence: IOHIDEventDriver turns a finger transducer whose value is 0 into a hand (palm), which
// multi-finger and system edge gestures ignore, so every finger, down or lifting, sends 1.
// Coordinates are normalised to 0..32767 on both axes here, from the panel pixels the producer publishes.
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02, 0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, 0x09,0x47,0x81,0x02, 0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, 0x09,0x31,0x81,0x02, 0x05,0x0d, 0xc0,
static const unsigned char touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
#define TOUCH_FINGERS 10
// Edge contacts (int-163). A contact that starts within ~3 mm of a panel edge carries kIOHIDDigitizerEventFromEdgeTip (0x800) in its
// finger events' event mask for its whole life, as Apple's multitouch stack reports it (MultitouchHID). UIKit derives UITouch's edge
// type from that bit alone (_UIEventHIDTouchEdgeTypeForHIDEvent: 0x800/0x1000/0x2000/0x40000 -> 1..4), and every screen-edge
// recognizer of SpringBoard fails at once on edge type 0: int-162 logged edge=0 for touches starting 2 pt above the bottom edge and
// SBHomeGesturePanGestureRecognizer / SBFluidSwitcherScreenEdgePanGestureRecognizer going 0->5, so no home or switcher gesture.
// The original IOHIDEventDriver (IOHIDFamily-2031.120.4) never sets the bit. Its dispatchEvent (IOHIDEventService slot 198, the slot
// 37 of IOHIDFamily's indirect calls use) is wrapped for this device's driver only, in a copy of its vtable; the wrapper adds the bit
// to the finger children of each digitizer collection and calls the original. IOHIDEvent virtuals of that version: getType 18,
// getIntegerValue 23, setIntegerValue 24, getFixedValue 25 (setIntegerValue/getIntegerValue at +0xc0/+0xb8 in the driver's own
// calls); _children at +24 (IOHIDEvent.h; appendChild stores into it and free releases it). _parent is never set (appendChild only
// stores the child, int-163 skipped every finger by comparing it), so each child is an IOHIDEvent by its class instead.
namespace edge {
enum {kSlots=256,kDispatch=198,kGetType=18,kGetInteger=23,kSetInteger=24,kGetFixed=25,kContacts=256};
enum : uint32_t {kDigitizer=11,kX=0xB0000,kY=0xB0001,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
// 24 of the panel's 1080 x 1920 pixels, in the driver's normalised IOFixed coordinates.
static const int32_t kEdgeX=24*65536/1080,kEdgeY=24*65536/1920;
static uintptr_t originalDispatch;
static bool fromEdge[kContacts];
extern "C" {volatile uint32_t native_touch_edge_stats[4];}   // [0] installed, [1] contacts begun, [2] of them at an edge, [3] skipped events
// [0] reports with two or more fingers down, [1] the most fingers down in one report, [2] tip-up entries sent for vanished fingers
extern "C" {volatile uint32_t native_touch_hid_multi[3];}
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
// One finger event; true when it now carries the bit.
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,0)!=kDigitizer)return false;   // the scan-time vendor child
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kIndex,0);
    if(index>=kContacts) {native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;return false;}
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {   // the contact begins: its position now decides
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeY || y>65536-kEdgeY;
        native_touch_edge_stats[1]=native_touch_edge_stats[1]+1;
        if(fromEdge[index])native_touch_edge_stats[2]=native_touch_edge_stats[2]+1;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;   // the lift keeps the bit; the next contact with this index starts fresh
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            // A collection (the driver's touch-screen dispatch): its mask is the union of its fingers', so it takes the bit as well.
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
                else native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);   // one transducer dispatched on its own
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
}
// This device's own event driver gets the wrapped copy; any other driver keeps the original table.
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    native_touch_edge_stats[0]=1;
}
}  // namespace edge
class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool down=false;
    uint8_t buttons=0;   // report 2 bits held by the hardware buttons
    bool touchEnabled=false;
    // The digitizer is a separate service from the keyboard: backboardd matches digitizers by primary usage page/usage
    // (Digitizer/Touch Screen) and the Built-In flag, which a combined keyboard-first descriptor would not provide.
    bool touchOnly=false;
    // The USB mouse device (native_hid_mouse_allocate): a service of its own with the report 4 descriptor.
    bool mouseOnly=false;
    bool eventDriverFound=false;
    uint32_t lastSeq=0;
    // The USB HID mailbox (package/Opi5plusUsb): the last keyboard / mouse sequence numbers this device handed to
    // handleReport, and what the mouse side has handed on, for the ~5 s log line of mouseFlush.
    uint32_t lastUsbKbdSeq=0,lastUsbMouseSeq=0;
    uint32_t ticks=0,mouseReports=0,mouseLogged=0;
    uint32_t realReports=0,realMoves=0,sumDx=0,sumDy=0;   // what the USB mouse sent, as handed to the event driver
    unsigned char lastRaw[4]={};
    IOReturn lastMouseResult=0;
    // opi5-mouse-demo=SECS: when the kernel has been up SECS seconds this device sends its own square walk (and opi5-mouse-click=1 a
    // button press at each corner, 2 a long press after each lap, 3 a drag: the button held all the way round), so a pointer can be
    // looked for on the screen without a person at the mouse. 0 = off.
    uint32_t demoSecs=0,demoClick=0,demoWheel=0,demoStartTick=0;
    bool demoStarted=false;   // opi5-mouse-wheel=1 (with the demo): no walk, a burst of 30 wheel counts every 2.5 s, four bursts one way then four the other; =2: a count every tick, 10 s one way, 10 s the other, the pointer jiggling; =3: the same with the pointer walking its square; =4: the pointer goes to the left edge and then +4 a tick for 100 ticks, the wheel from tick 300; =5: to the bottom left (the bottom edge takes iPadOS Home), up and right, a click, then the wheel in growing blocks.
    // Fingers sent as down in the previous report; a vanished identifier gets one explicit tip-up report.
    int prevIds[TOUCH_FINGERS]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    int prevX[TOUCH_FINGERS]={},prevY[TOUCH_FINGERS]={};
    // The synthetic gesture injector (opi5-hid-input=2, see injectTick below). Without a USB host stack this device is the
    // only input source, so the injector is what makes the lock screen operable; it stays off at 1.
    bool injectMode=false;
    bool injectDriverLogged=false;      // "driver appeared" is a one-off line
    bool injectGestureActive=false;     // a gesture is being walked, injectStep is its current sample
    bool injectHomePending=false;       // the Home press of the current cycle is down, the release is due
    bool injectOpenGateGivenUp=false;   // the bounded open gate expired: inject without ever having seen a client
    uint32_t injectStep=0,injectGestures=0,injectReports=0,injectFails=0,injectLast=0;
    uint32_t injectNoDriverTicks=0,injectNotOpenTicks=0;
    uint64_t injectFrequency=0,injectStarted=0,injectNextHeartbeat=0;
    // App-launch tapping (round 20, see injectTapTick): injectTapMode is set when the first gesture
    // completes -- the unlock gesture is not repeated after that -- and one tap follows every 30 s.
    // injectTapLiftPending marks the down half of the tap whose lift is still due.
    bool injectTapMode=false,injectTapLiftPending=false;
    uint32_t injectTaps=0,injectTapFails=0;
    // Round 21 deferred unlock (see the header note): injectDeferredReleased lifts the wait once
    // backboardd and a swap have both been seen and the 30 s quiet has passed; injectDeferredSeen holds
    // the tick both were first true together. injectFirstTapLogged makes the first tap's moment a log
    // line of its own.
    bool injectDeferredReleased=false,injectDeferredLogged=false,injectFirstTapLogged=false;
    uint64_t injectDeferredSeen=0;
    static unsigned transformCoordinate(int value,int maximum) {
        if(value<0)value=0;if(value>=maximum)value=maximum-1;
        return (unsigned)((long long)value*32767/(maximum-1));
    }
    // The display window inside the 1080 x 1920 panel (the kernel's own frame buffer, evb10-fb-w/-h, is centred in it; zero while the
    // firmware's is in use): a panel pixel becomes a position of the window, which is what UIKit's screen is.
    static unsigned transformPanel(int value,int panel,unsigned window) {
        if(!window || window>=(unsigned)panel)return transformCoordinate(value,panel);
        const int origin=(panel-(int)window)/2;
        return transformCoordinate(value-origin,(int)window);
    }
    // One finger set to the event driver. The touch thread (package/Evb10Touch) calls touchNotify after it publishes a frame; there is no timer
    // watching the seqlock any more (it woke the work loop every 8 ms for nothing).
    static void touchNotify(void *context) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::touchAction,self);
    }
    static IOReturn touchAction(OSObject *owner,void *,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->touchSend();
        return kIOReturnSuccess;
    }
    void touchSend() {
        NativeHidInput *self=this;
        if(!self->hasEventDriverCached())return;
        uint32_t before=native_touch_seq;
        if((before&1) || before==self->lastSeq)return;
        __asm__ volatile("dmb ish":::"memory");
        unsigned count=native_touch_count;if(count>TOUCH_FINGERS)count=TOUCH_FINGERS;
        int ids[TOUCH_FINGERS],xs[TOUCH_FINGERS],ys[TOUCH_FINGERS];
        for(unsigned i=0;i<count;++i){ids[i]=native_touch_ids[i];xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
        __asm__ volatile("dmb ish":::"memory");
        if(native_touch_seq!=before)return;      // torn read: retry on the next tick
        self->lastSeq=before;
        // The panel is 1080 x 1920 pixels, mapped to the 0..32767 range of the descriptor on both axes.
        unsigned char report[1+TOUCH_FINGERS*6+1]={3};
        unsigned slot=0;
        int nextIds[TOUCH_FINGERS],nextX[TOUCH_FINGERS],nextY[TOUCH_FINGERS];
        for(unsigned i=0;i<count && slot<TOUCH_FINGERS;++i,++slot) {
            unsigned x=transformPanel(xs[i],1080,evb10_fb_w),y=transformPanel(ys[i],1920,evb10_fb_h);
            unsigned char *f=report+1+slot*6;
            f[0]=3;f[1]=(unsigned char)(ids[i]&0xff);f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);   // tip, valid
            nextIds[slot]=ids[i];nextX[slot]=(int)x;nextY[slot]=(int)y;
        }
        const unsigned downCount=slot;
        if(downCount>=2)edge::native_touch_hid_multi[0]=edge::native_touch_hid_multi[0]+1;
        if(downCount>edge::native_touch_hid_multi[1])edge::native_touch_hid_multi[1]=downCount;
        // Identifiers that were down and are gone: tip up at the last known position. They are matched by identifier, not by slot:
        // the producer packs its fingers, so when one lifts the others move down a slot and a slot match would lift a finger still
        // down and leave the lifted one stuck.
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            const int id=self->prevIds[j];
            if(id<0)continue;
            bool kept=false;
            for(unsigned i=0;i<downCount && !kept;++i)kept=(ids[i]&0xff)==(id&0xff);
            if(kept || slot>=TOUCH_FINGERS)continue;
            unsigned char *f=report+1+slot*6;
            f[0]=2;f[1]=(unsigned char)(id&0xff);   // tip up, valid
            f[2]=(unsigned char)self->prevX[j];f[3]=(unsigned char)(self->prevX[j]>>8);
            f[4]=(unsigned char)self->prevY[j];f[5]=(unsigned char)(self->prevY[j]>>8);
            ++slot;edge::native_touch_hid_multi[2]=edge::native_touch_hid_multi[2]+1;
        }
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            self->prevIds[j]=j<downCount?nextIds[j]:-1;
            if(j<downCount){self->prevX[j]=nextX[j];self->prevY[j]=nextY[j];}
        }
        if(!slot)return;
        report[1+TOUCH_FINGERS*6]=(unsigned char)slot;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
    // The same report 3 a single-finger touchSend would send, at a position the caller names: report id, one 6-byte finger
    // entry (tip switch/valid flags, contact identifier, 16-bit little-endian X and Y), the nine unused entries left zero,
    // then the entry count. Only the flags and the two coordinates differ from touchSend's entry, which is what makes this
    // the one report layout the original event driver is known to accept here.
    IOReturn emitTouchReport(unsigned char tip,unsigned char id,unsigned short x,unsigned short y) {
        unsigned char report[1+TOUCH_FINGERS*6+1]={3};
        unsigned char *f=report+1;
        f[0]=tip;f[1]=id;
        f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);
        f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);
        report[1+TOUCH_FINGERS*6]=1;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);       // the event touchSend reports its per-report result on
        native_touch_hid_reports=native_touch_hid_reports+1;
        return result;
    }
    // The button driver's thread calls buttonsNotify on every change of the pressed set.
    static void buttonsNotify(void *context,uint32_t mask) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::buttonsAction,self,reinterpret_cast<void *>(uintptr_t(mask)));
    }
    static IOReturn buttonsAction(OSObject *owner,void *mask,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->buttons=static_cast<uint8_t>(uintptr_t(mask)&0xe);
        if(!self->hasEventDriverCached())return kIOReturnSuccess;
        unsigned char report[]={2,static_cast<unsigned char>((self->down?1:0)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(4,result);
        return kIOReturnSuccess;
    }
    bool hasEventDriverCached() {
        if(!eventDriverFound) {
            IORegistryEntry *driver=eventDriver(this,4);
            eventDriverFound=driver!=nullptr;
            if(driver && (touchEnabled || touchOnly))edge::install(driver);
        }
        return eventDriverFound;
    }
    static bool hasEventDriver(IORegistryEntry *parent,unsigned depth) {return eventDriver(parent,depth)!=nullptr;}
    // The IOHIDEventDriver below this device, if one has attached.
    static IORegistryEntry *eventDriver(IORegistryEntry *parent,unsigned depth) {
        if(!depth)return nullptr;
        OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
        IORegistryEntry *found=nullptr;
        while(OSObject *child=it->getNextObject()) {
            IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
            if(!entry)continue;
            found=entry->metaCast("IOHIDEventDriver")?entry:eventDriver(entry,depth-1);
            if(found)break;
        }
        it->release();return found;
    }
    // ---- the synthetic gesture injector (opi5-hid-input=2) -------------------------------------------------------------------
    // One finger swiped from near the bottom of the screen to near the top, 21 samples 20 ms apart, then 5 s of quiet, for as
    // long as the boot lasts: backboardd comes up around t+720 s, so a single gesture would land before anything listens.
    // 32767 is full scale on both axes and Y grows downwards, so 80% is near the bottom of the screen and 20% near the top.
    enum {kSamples=21,kSampleMs=20,kPauseMs=5000,kCenterX=16383,kStartY=32767*80/100,kEndY=32767*20/100,kOpenGateSeconds=120,
          kUnlockDelaySeconds=30,kUnlockFallbackSeconds=600};
    // The tap point (round 20): the App Store icon of the user's photo (2364 x 1773) sits at its centre
    // (1470, 540) = (0.622, 0.305) of the panel, which on the 2560 x 1440 panel is (1591, 439). The tap
    // goes through the same report 3 pipe as the gesture (emitTouchReport) and the same panel-to-window
    // transform (transformPanel), so the constants stay panel pixels; the finger id 2 cannot collide
    // with the gesture's (1).
    enum {kTapPanelX=1591,kTapPanelY=439,kTapPanelW=2560,kTapPanelH=1440,kTapPeriodMs=30000,kTapLiftMs=80};
    static uint64_t injectCounter() {uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
    // Sample i of a gesture, i in 0..kSamples-1: the finger walks from 80% to 20% of the screen's height.
    static unsigned injectY(uint32_t step) {return (unsigned)(kStartY+(kEndY-kStartY)*(int)step/(kSamples-1));}
    // Every report the injector sends is counted and its failure logged here, so all three paths mean the same thing.
    void injectCount(IOReturn result) {
        injectReports=injectReports+1;injectLast=(uint32_t)result;
        if(result==kIOReturnSuccess)return;
        injectFails=injectFails+1;
        if(injectFails<=5 || injectFails%100==0)printf("opi5: hid: inject: report %u failed: %#x\n",injectFails,(unsigned)result);
    }
    // Report 2 with the given consumer bits, the layout of buttonsAction's below: bit 0 is Menu (Home).
    IOReturn emitButtonReport(unsigned buttons) {
        unsigned char report[]={2,static_cast<unsigned char>(buttons&0x0f)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(4,result);       // the event buttonsAction reports its report on
        return result;
    }
    // One timer step of one gesture: one sample, then the next sample in 20 ms, or the 5 s pause after the lift.
    void injectSample(IOTimerEventSource *source) {
        const uint32_t step=injectStep;
        const unsigned y=injectY(step);
        // Sample 0 lands the finger (tip switch + valid); every sample in between moves it; the last one lifts it (valid only).
        injectCount(emitTouchReport((step==kSamples-1)?2:3,1,(unsigned short)kCenterX,(unsigned short)y));
        if(step+1<kSamples) {injectStep=step+1;source->setTimeoutMS(kSampleMs);return;}
        printf("opi5: hid: inject: gesture %u: lift at %u,%u, reports %u, fails %u, last result %#x\n",
               injectGestures,kCenterX,y,injectReports,injectFails,injectLast);
        injectGestureActive=false;injectStep=0;injectHomePending=false;
        if(!injectTapMode) {   // the first completed gesture: the unlock is done, the tap cycle takes over
            injectTapMode=true;
            printf("opi5: hid: inject: gesture %u complete, entering app-launch tapping: one tap at panel (%u,%u) every %d ms\n",
                   injectGestures,(unsigned)kTapPanelX,(unsigned)kTapPanelY,(int)kTapPeriodMs);
        }
        source->setTimeoutMS(kPauseMs);   // the same 5 s quiet after the lift; the next tick taps instead of re-gesturing
    }
    // The first sample of a gesture, out on the tick that starts it.
    void injectGesture(IOTimerEventSource *source) {
        injectGestures=injectGestures+1;injectGestureActive=true;injectStep=0;
        printf("opi5: hid: inject: gesture %u: down at %u,%u\n",injectGestures,kCenterX,kStartY);
        injectSample(source);
    }
    // The tap result counts and logs here, the way injectCount does for the gesture reports.
    void injectTapCount(IOReturn result) {
        if(result==kIOReturnSuccess)return;
        injectTapFails=injectTapFails+1;
        if(injectTapFails<=5 || injectTapFails%100==0)printf("opi5: hid: inject: tap report failed: %#x\n",(unsigned)result);
    }
    // One step of app-launch tapping (round 20): the finger lands on the App Store icon, and 80 ms
    // later it lifts -- a tap, down and lift in adjacent reports. Runs once per kTapPeriodMs, forever
    // after the first completed gesture; a wedge during reproduction does not stop it (a tap into a
    // wedged compositor is as harmless as the gesture was). The same wait-behind-the-driver gate the
    // gestures have: without an IOHIDEventDriver below, nothing consumes the report.
    void injectTapTick(IOTimerEventSource *source) {
        if(!eventDriver(this,4)) {source->setTimeoutMS(1000);return;}
        const unsigned x=transformPanel(kTapPanelX,kTapPanelW,evb10_fb_w),y=transformPanel(kTapPanelY,kTapPanelH,evb10_fb_h);
        if(injectTapLiftPending) {
            injectTapLiftPending=false;
            injectTaps=injectTaps+1;
            const IOReturn lift=emitTouchReport(2,2,(unsigned short)x,(unsigned short)y);   // tip up: the lift
            injectTapCount(lift);
            printf("opi5: hid: inject: tap %u: down/lift done at %u,%u (panel %u,%u), lift %#x\n",
                   injectTaps,x,y,(unsigned)kTapPanelX,(unsigned)kTapPanelY,(unsigned)lift);
            source->setTimeoutMS(kTapPeriodMs);
            return;
        }
        injectTapCount(emitTouchReport(3,2,(unsigned short)x,(unsigned short)y));   // tip + valid: the touch down
        if(!injectFirstTapLogged) {
            injectFirstTapLogged=true;
            printf("opi5: hid: inject: first tap at t+%u s (panel %u,%u)\n",
                   injectFrequency?(unsigned)((injectCounter()-injectStarted)/injectFrequency):0u,
                   (unsigned)kTapPanelX,(unsigned)kTapPanelY);
        }
        injectTapLiftPending=true;
        source->setTimeoutMS(kTapLiftMs);
    }
    // The injector owns the timer while it is armed: it waits for the event driver and for a client (backboardd's
    // IOHIDEventDriver open) and otherwise walks gestures. The t+ heartbeat is counted on the architectural counter, not on
    // timer ticks, because the period is 20 ms inside a gesture and 1 s or 5 s outside one.
    void injectTick(IOTimerEventSource *source) {
        if(injectFrequency) {
            const uint64_t now=injectCounter();
            if(now>=injectNextHeartbeat) {
                IORegistryEntry *open=eventDriver(this,4);
                injectNextHeartbeat=injectNextHeartbeat+30*injectFrequency;
                printf("opi5: hid: inject: t+%u s: gestures %u reports %u fails %u taps %u open %d driver %p\n",
                       (unsigned)((now-injectStarted)/injectFrequency),injectGestures,injectReports,injectFails,
                       injectTaps,isOpen()?1:0,static_cast<void *>(open));
            }
        }
        if(injectGestureActive) {injectSample(source);return;}
        if(injectTapMode) {injectTapTick(source);return;}
        // Round 21: the deferred unlock gate. Both conditions must hold -- backboardd spawned (patch 0006's
        // spawn tracker) and at least one display swap completed (LegacyDisplayProbe's swap counter) -- and
        // then 30 s pass before the first gesture, so the compositor is actually serving. Ten minutes
        // without both falls back to the plain driver/open gate below (and its own 120 s bounded wait).
        if(!injectDeferredReleased) {
            const uint64_t now=injectCounter();
            const uint64_t up=injectFrequency?(now-injectStarted)/injectFrequency:0;
            const bool bb=(&native_spawn_seen_backboardd && native_spawn_seen_backboardd!=0);
            const bool swapped=(&native_legacy_swap_accepted_count && native_legacy_swap_accepted_count!=0);
            if(bb && swapped) {
                if(!injectDeferredSeen)injectDeferredSeen=now;
                const uint64_t since=injectFrequency?(now-injectDeferredSeen)/injectFrequency:0;
                if(since<(uint64_t)kUnlockDelaySeconds) {
                    if(!injectDeferredLogged) {
                        injectDeferredLogged=true;
                        printf("opi5: hid: inject: unlock deferred (waiting backboardd/swap): both seen at t+%u s (swap %u), gesturing in %d s\n",
                               (unsigned)up,(&native_legacy_swap_accepted_count)?(unsigned)native_legacy_swap_accepted_count:0u,(int)kUnlockDelaySeconds);
                    }
                    source->setTimeoutMS(1000);return;
                }
                injectDeferredReleased=true;
                printf("opi5: hid: inject: unlock at t+%u s: backboardd 1, swap %u, %u s after both seen\n",
                       (unsigned)up,(&native_legacy_swap_accepted_count)?(unsigned)native_legacy_swap_accepted_count:0u,(unsigned)since);
            } else {
                if(!injectDeferredLogged) {
                    injectDeferredLogged=true;
                    printf("opi5: hid: inject: unlock deferred (waiting backboardd/swap): backboardd %d swap %u at t+%u s\n",
                           (int)(bb?1:0),(&native_legacy_swap_accepted_count)?(unsigned)native_legacy_swap_accepted_count:0u,(unsigned)up);
                }
                if(up<(uint64_t)kUnlockFallbackSeconds) {source->setTimeoutMS(1000);return;}
                injectDeferredReleased=true;
                printf("opi5: hid: inject: unlock deferred: fallback after %u s without backboardd/swap (backboardd %d swap %u), the driver/open gate applies\n",
                       (unsigned)up,(int)(bb?1:0),(&native_legacy_swap_accepted_count)?(unsigned)native_legacy_swap_accepted_count:0u);
            }
        }
        IORegistryEntry *driver=eventDriver(this,4);
        if(!driver) {
            injectNoDriverTicks=injectNoDriverTicks+1;
            // First attempt then every tenth: whether a driver ever attached must be visible at the top of the log, and this
            // gate stays hard -- with no IOHIDEventDriver below there is nothing to consume a report.
            if(injectNoDriverTicks==1 || !(injectNoDriverTicks%10))
                printf("opi5: hid: inject: waiting: no event driver under %p (tick %u)\n",
                       static_cast<void *>(this),injectNoDriverTicks);
            source->setTimeoutMS(1000);return;
        }
        if(!injectDriverLogged) {
            injectDriverLogged=true;
            printf("opi5: hid: inject: driver appeared %p open %d\n",static_cast<void *>(driver),isOpen()?1:0);
        }
        if(!injectOpenGateGivenUp && !isOpen()) {
            injectNotOpenTicks=injectNotOpenTicks+1;
            if(injectNotOpenTicks==1 || !(injectNotOpenTicks%10))
                printf("opi5: hid: inject: waiting: not open (tick %u), driver %p\n",injectNotOpenTicks,static_cast<void *>(driver));
            // The gate is bounded: nothing may ever open this device itself (backboardd opens the event service above the
            // driver), so after kOpenGateSeconds of counter time the injector stops asking and injects anyway -- early
            // reports are harmless, the cycle repeats for the whole boot. Judged once, then the branch is never taken again.
            const uint64_t waited=injectFrequency?(injectCounter()-injectStarted)/injectFrequency:injectNotOpenTicks;
            if(waited<(uint64_t)kOpenGateSeconds) {source->setTimeoutMS(1000);return;}
            injectOpenGateGivenUp=true;
            printf("opi5: hid: inject: giving up on the open gate after %u s without an open, injecting anyway\n",(unsigned)waited);
        }
        // One Home press between two gestures: a second channel in case an up-swipe does not take on this device class.
        const bool release=injectHomePending;
        injectHomePending=!injectHomePending;
        if(release) {injectCount(emitButtonReport(0));injectGesture(source);return;}
        injectCount(emitButtonReport(0x01));   // bit 0: Menu (Home)
        source->setTimeoutMS(150);
    }
    // The USB HID mailbox (package/Opi5plusUsb), once per tick. The keyboard side sends report 1 whenever
    // kbd_seq moved; the mouse side is mouseFlush below. No mailbox access happens while the address of the
    // weak symbol is 0 (USB not linked) or no event driver has attached (nothing would consume the report).
    void usbHidFlush() {
        if(mouseOnly){mouseFlush();return;}
        if(!&native_usb_kbd_seq || !hasEventDriverCached())return;
        const uint32_t kseq=native_usb_kbd_seq;
        if(kseq!=lastUsbKbdSeq) {
            lastUsbKbdSeq=kseq;
            unsigned char report[9]={1};
            for(unsigned i=0;i<8;++i)report[1+i]=native_usb_kbd[i];
            IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
            IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
            OSSafeReleaseNULL(buffer);
            native_hid_input_observed(7,result);
        }
    }
    // The mouse device: report 4 (buttons, dx, dy, wheel) for every report the USB driver published.
    void mouseFlush() {
        if(!&native_usb_mouse_seq || !hasEventDriverCached())return;
        const uint32_t mseq=native_usb_mouse_seq;
        // The motion of every report since the last tick (the USB driver adds them up), at most +-127 per report: what is left goes out
        // with the next tick. The buttons are the newest report's.
        auto take=[](volatile int32_t *total)->signed char {
            const int32_t value=__atomic_load_n(total,__ATOMIC_RELAXED);
            const int32_t part=value>127?127:(value<-127?-127:value);
            if(part)__atomic_fetch_sub(total,part,__ATOMIC_RELAXED);
            return (signed char)part;
        };
        const signed char dx=take(&native_usb_mouse_dx),dy=take(&native_usb_mouse_dy),wheel=take(&native_usb_mouse_wheel);
        if(mseq!=lastUsbMouseSeq || dx || dy || wheel) {
            lastUsbMouseSeq=mseq;
            unsigned char report[5]={4,native_usb_mouse[0],(unsigned char)dx,(unsigned char)dy,(unsigned char)wheel};
            ++realReports;if(dx||dy)++realMoves;
            sumDx+=dx<0?-dx:dx;sumDy+=dy<0?-dy:dy;
            for(unsigned i=0;i<4;++i)lastRaw[i]=native_usb_mouse[i];
            IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
            lastMouseResult=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
            OSSafeReleaseNULL(buffer);
            ++mouseReports;
            native_hid_input_observed(7,lastMouseResult);
        }
        if(demoSecs && !demoStarted) {   // the demo starts when the kernel has been up demoSecs seconds (the ticks only begin when the event driver is found, which is later)
            uint64_t ns=0;
            absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
            if(ns>=(uint64_t)demoSecs*1000000000ull){demoStarted=true;demoStartTick=ticks;printf("opi5: hid: mouse: demo starts (up %llu s, wheel mode %u)\n",(unsigned long long)(ns/1000000000ull),demoWheel);}
        }
        if(demoStarted) {   // one 8 ms tick = one report; 80 ticks a side, 6 counts a tick
            const uint32_t t=ticks-demoStartTick;
            const signed char step=6;
            signed char dx=0,dy=0;
            // opi5-mouse-click=2: after every lap of the square the button is held down 150 ticks (1.2 s) without moving, a long press
            const uint32_t cycle=demoClick==2?470:320,c=t%cycle;
            unsigned char buttons=0;
            if(c<320){
                switch((c/80)&3){case 0:dx=step;break;case 1:dy=step;break;case 2:dx=-step;break;default:dy=-step;break;}
                buttons=demoClick==3?1:(demoClick==1 && (c%80)>=70)?1:0;   // 1: pressed for the last 80 ms of every side; 3: held down the whole walk (a drag)
            }else buttons=1;
            signed char wheel=0;
            if(demoWheel){   // 30 counts in 240 ms, then nothing for 2.26 s: four bursts one way (9.9 s), four the other
                // The pointer needs a position before a scroll goes anywhere (backboardd routes it by where the pointer is): a nudge of 2 counts before each burst, back after it.
                if(demoWheel!=3)dx=dy=0;
                buttons=0;
                if(demoWheel==4){   // 4: to the left edge (the screen clamps it; not the corner: the top left one opens the Notification Center), then 100 ticks of +4 to the right (about 450 px), and the wheel runs (10 s one way, 10 s the other)
                    if(t<60)dx=-127;
                    else if(t<160)dx=4;
                    if(t>=300)wheel=(((t-300)/1250)&1)?-1:1;
                }else if(demoWheel==5){   // 5: to the bottom left, then up and right (+4,-4) 85 ticks (about 390 px each way: the Notifications row of the Settings sidebar), a click of 80 ms there at tick 260,
                    // and from tick 300 the wheel in blocks of 500 ticks (4 s) that grow: +1, -2, +4, -8, +16, -32 counts a tick (a threshold or a rounding in the scroll path would show as the first block that scrolls)
                    if(t<60)dx=-127;
                    else if(t<120)dy=127;
                    else if(t<205){dx=4;dy=-4;}
                    if(t>=260 && t<270)buttons=1;
                    if(t>=300){const uint32_t b=(t-300)/500;if(b<6){const int m=1<<b;wheel=(signed char)((b&1)?-m:m);}}
                }else if(demoWheel==2 || demoWheel==3){   // continuous: one count a tick for 10 s one way, 10 s the other; 2: the pointer jiggles a count to and fro, 3: it walks the square
                    wheel=((t/1250)&1)?-1:1;
                    if(demoWheel==2)dx=(t&1)?1:-1;
                }else{
                    const uint32_t p=t%310;
                    if(p==0)dx=2;
                    else if(p==40)dx=-2;
                    if(p>=5 && p<35)wheel=((t/1240)&1)?-1:1;
                }
            }
            unsigned char report[5]={4,buttons,(unsigned char)dx,(unsigned char)dy,(unsigned char)wheel};
            IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
            lastMouseResult=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
            OSSafeReleaseNULL(buffer);
            ++mouseReports;
        }
        // About every five seconds while the mouse is moving: what became of the reports. Board form of
        // upstream's log: the mouse-side counters only (this board's edge:: wrapper does not capture
        // dispatched event types or Pointer samples, and the registry tree dump is not ported).
        if(++ticks%625==0 && mouseReports!=mouseLogged) {
            mouseLogged=mouseReports;
            printf("opi5: hid: mouse: %u reports to handleReport, last result 0x%x\n",mouseReports,(unsigned)lastMouseResult);
            printf("opi5: hid: mouse: USB reports handed on %u, with motion %u, sum |dx| %u |dy| %u, newest raw %02x %02x %02x %02x\n",
                   realReports,realMoves,sumDx,sumDy,lastRaw[0],lastRaw[1],lastRaw[2],lastRaw[3]);
        }
    }
    static void tick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        native_hid_live_checkpoint(self);
        // The USB HID mailbox: report 1 for a moved keyboard report, report 4 for the accumulated mouse
        // motion. On the injector device (opi5-hid-input>=2) this rides the injector's own tick -- 1 s
        // while it waits, 20 ms inside a gesture; the separate mouse device re-arms 8 ms below, which is
        // the cadence the mailbox accumulators were sized for.
        self->usbHidFlush();
        if(self->injectMode) {self->injectTick(source);return;}
        if(self->mouseOnly){source->setTimeoutMS(8);return;}
        if(self->touchOnly){source->setTimeoutMS(1000);return;}
        // Prefer a short tick while USB HID may deliver reports; fall back to the
        // auto-Home probe when nothing is plugged in.
        if(&native_usb_kbd_seq || &native_usb_mouse_seq) {
            source->setTimeoutMS(8);
            if(!self->down && !native_hid_input_requested())return;
        }
        if(!self->down && !hasEventDriver(self,4)) {
            native_hid_input_observed(3,0);
            source->setTimeoutMS(1000);
            return;
        }
        native_hid_input_observed(3,1);
        if(!self->down && !native_hid_input_requested()) {source->setTimeoutMS(1000);return;}
        unsigned char report[]={2,static_cast<unsigned char>((self->down?0:1)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down?5:4,result);
        // Always attempt release, even if down returned an error: input must
        // never be left pressed by an ambiguous report completion.
        if(!self->down) {self->down=true;source->setTimeoutMS(100);}
        else {self->down=false;source->setTimeoutMS(1000);}
    }
public:
    void setTouchOnly() {touchOnly=true;}
    void setMouseOnly() {mouseOnly=true;}
    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString(touchOnly?"SPI":(mouseOnly?"USB":"Virtual"));}
    OSString *newProductString() const override {return OSString::withCString(touchOnly?"Offline touch digitizer":(mouseOnly?"USB Mouse":"Offline HID input probe"));}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        unsigned char combined[sizeof(descriptor)+sizeof(touchDescriptor)];
        size_t size;
        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}
        else if(mouseOnly){size=sizeof(mouseDescriptor);memcpy(combined,mouseDescriptor,size);}
        else{
            size=sizeof(descriptor);memcpy(combined,descriptor,size);
            // Default: one device, keyboard collection first, digitizer second (the arrangement whose event driver
            // and user client attach on hardware); the separate digitizer device is the opt-in sheng-touch-split.
            if(touchEnabled){memcpy(combined+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
        }
        *out=IOBufferMemoryDescriptor::withBytes(combined,size,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop=IOWorkLoop::workLoop();if(!inputLoop)return false;
        // The probe parsed opi5-hid-input/evb10-hid-input before it allocated this device: 2 and above also inject. A separate
        // digitizer device (sheng-touch-split) is never an injector, and neither is the USB mouse device.
        injectMode=native_hid_input_mode>=2 && !touchOnly && !mouseOnly;
        // Request the normal generic HID event personality for this owned
        // keyboard. This is a matching property, not a client entitlement.
        if(!setProperty("HIDDefaultBehavior",true))return false;
        unsigned touch=0;
        touchEnabled=!mouseOnly && PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1;   // the mouse is no digitizer
        // The injector sends report 3, so the descriptor has to carry the Touch Screen collection. IOHIDDevice::start below
        // is what asks this class for newReportDescriptor, so the flag must be set by then, not after the start returns.
        if(injectMode)touchEnabled=true;
        if(mouseOnly) {   // the demo boot arguments (see the members above): all default 0 = off
            PE_parse_boot_argn("opi5-mouse-demo",&demoSecs,sizeof(demoSecs));
            PE_parse_boot_argn("opi5-mouse-click",&demoClick,sizeof(demoClick));
            PE_parse_boot_argn("opi5-mouse-wheel",&demoWheel,sizeof(demoWheel));
        }
        // DigitizerCollectionDispatch: IOHIDEventDriver (setDigitizerProperties) reads it up the service plane and then sends each
        // report's fingers as one digitizer collection with a child per finger. Without it a touch screen gets one event per finger,
        // and backboardd takes each as the whole touch set, so no two fingers are ever down together for UIKit (no pinch, no
        // multi-finger or system gestures).
        if(touchOnly||touchEnabled){setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        if(!IOHIDDevice::start(provider))return false;
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(!timer || inputLoop->addEventSource(timer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;
        }
        if(touchOnly||touchEnabled) {
            native_touch_set_sink(&NativeHidInput::touchNotify,this);
        }
        setProperty("OfflineInputProbe",true);
        native_hid_input_observed(2,0);
        if(injectMode) {
            __asm__ volatile("mrs %0, cntfrq_el0":"=r"(injectFrequency));
            injectStarted=injectCounter();
            if(injectFrequency)injectNextHeartbeat=injectStarted+30*injectFrequency;
            printf("opi5: hid: inject: armed, mode %u, digitizer %d, %u samples x %d ms\n",
                   native_hid_input_mode,touchEnabled?1:0,(unsigned)kSamples,(int)kSampleMs);
        }
        timer->setTimeoutMS(1000);return true;
    }
    void stop(IOService *provider) override {
        if(touchOnly||touchEnabled)native_touch_set_sink(nullptr,nullptr);
        if(timer){timer->cancelTimeout();inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
        IOHIDDevice::stop(provider);
    }
    void free() override {OSSafeReleaseNULL(inputLoop);IOHIDDevice::free();}
};
// OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice), with one difference: this kernel does not export
// IOHIDDevice::gMetaClass (the kext's symbols are not in the prelinked symbol table), so the superclass comes from
// the OSMetaClass registry instead of from a linked address. The probe loads the original IOHIDFamily, looks the
// class up by name, stores it here and only then runs this translation unit's initializer (the metaclass ctor is
// the one that reads it), which is why the archive defers that initializer instead of running it at load time.
extern "C" const OSMetaClass *opi5plus_hid_superclass;
NativeHidInput::MetaClass NativeHidInput::gMetaClass;
const OSMetaClass * const NativeHidInput::metaClass = &NativeHidInput::gMetaClass;
const OSMetaClass * const NativeHidInput::superClass = nullptr;
NativeHidInput::MetaClass::MetaClass()
    : OSMetaClass("NativeHidInput", opi5plus_hid_superclass, sizeof(NativeHidInput)) {}
OSObject *NativeHidInput::MetaClass::alloc() const {return new NativeHidInput;}
NativeHidInput::NativeHidInput() : IOHIDDevice(&gMetaClass) {gMetaClass.instanceConstructed();}
NativeHidInput::NativeHidInput(const OSMetaClass *meta) : IOHIDDevice(meta) {}
NativeHidInput::~NativeHidInput() {}
const OSMetaClass *NativeHidInput::getMetaClass() const {return &gMetaClass;}
void *NativeHidInput::operator new(size_t size) {return OSObject::operator new(size);}
void NativeHidInput::operator delete(void *mem,size_t size) {return OSObject::operator delete(mem,size);}
extern "C" IOService *native_hid_input_allocate(void) {return new NativeHidInput;}
extern "C" IOService *native_hid_touch_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setTouchOnly();return device;}
extern "C" IOService *native_hid_mouse_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setMouseOnly();return device;}
