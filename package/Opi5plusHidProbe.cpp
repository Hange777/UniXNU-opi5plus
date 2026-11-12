// SPDX-License-Identifier: BSD-2-Clause-Patent
// HidInputProbe.cpp of UniXNU-sheng (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors), cut for the RK3588 EVB10
// (2026-10-28): the original IOHIDFamily and IOHIDEventDriver are loaded by LegacyDisplayProbe.cpp (evb10_hid_load), which then
// calls evb10_hid_input_start with the IOHIDResource it waited for; this file only brings the owned input device up on it.
// The registry/property dump buffers and the mailbox of sheng's probe are gone; the Home-button auto-press and the live checkpoint
// (the Built-In / PrimaryUsage properties backboardd's digitizer filter needs) stay.
//
//   opi5-hid-input=1 (or evb10-hid-input=1)  start NativeHidInput (keyboard + touch screen HID for backboardd)
//   opi5-hid-input=2 (or evb10-hid-input=2)  the same, plus the synthetic swipe/Home injector in NativeHidInput::tick
//   opi5-usb=<N>          package/Opi5plusUsb feeds the native_usb_* mailbox this probe's devices consume: whenever
//                         the USB driver is linked (the weak native_usb_mouse_seq address test), a mouse device is
//                         added next to the keyboard and addKernelPointerPersonality gives the original
//                         IOHIDEventDriver the match for USB pointers. Without the USB driver nothing of it runs.
//   sheng-touch-hid=1     give it the touch screen collection (read by NativeHidInput::start)
//   sheng-touch-split=1   a separate digitizer device as well (default: one combined device)
//   offline-hid-auto-home=<s> [-count N] [-interval s]   unattended Home presses after the second presented frame
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" IOService *native_hid_mouse_allocate(void);
// package/Opi5plusUsb: weak, so the file still links without the USB driver (the declaration form
// package/LegacyDisplayProbe.cpp uses for opi5_usb_start).
extern "C" volatile uint32_t native_usb_mouse_seq __attribute__((weak_import));
extern "C" {
// How far opi5-hid-input/evb10-hid-input asks the input path to go: 0 = not enabled, 1 = the HID device only (round 2's
// behaviour), >=2 = the device and its synthetic gesture injector. Written here before NativeHidInput is allocated or
// started, because NativeHidInput::start must know it when IOHIDDevice::start asks the class for the report descriptor.
volatile unsigned native_hid_input_mode;
volatile uint32_t native_hid_stage;
volatile int native_hid_input_meta_status=-1;
volatile uint32_t native_hid_class_size;
volatile uint64_t native_hid_input_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
// The original IOHIDDevice metaclass, found by name in this file's entry point and read by NativeHidInput's
// metaclass constructor when the deferred initializer runs (package/Opi5plusHid/NativeHidInput.cpp).
const OSMetaClass *opi5plus_hid_superclass;
extern volatile uint32_t native_legacy_transfer_calls;
void native_hid_input_observed(unsigned event,int result);
void native_hid_live_checkpoint(IOService *device);
bool native_hid_input_requested(void);
void evb10_hid_input_start(IOService *resource);
}
// No touch controller on this board yet: empty finger set so NativeHidInput still publishes a digitizer.
extern "C" {
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
void native_touch_set_sink(void (*)(void *), void *) {}
}
static unsigned autoConfigured,autoFirst,autoCount,autoInterval,autoSent;
static uint64_t autoNext,autoFrequency;
static uint64_t counterNow(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
bool native_hid_input_requested(void) {
    if(native_legacy_transfer_calls<2)return false;
    if(!autoConfigured) {
        autoConfigured=1;autoFirst=0;autoCount=1;autoInterval=20;
        PE_parse_boot_argn("offline-hid-auto-home",&autoFirst,sizeof(autoFirst));
        PE_parse_boot_argn("offline-hid-auto-home-count",&autoCount,sizeof(autoCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval",&autoInterval,sizeof(autoInterval));
        if(autoCount>8)autoCount=8;if(autoInterval<5)autoInterval=5;
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(autoFrequency));
        if(autoFirst && autoFrequency)autoNext=counterNow()+(uint64_t)autoFirst*autoFrequency;
    }
    if(autoFirst && autoFrequency && autoSent<autoCount && counterNow()>=autoNext) {
        ++autoSent;autoNext+=(uint64_t)autoInterval*autoFrequency;return true;
    }
    return false;
}
static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}
// Once a second from the device's timer (NativeHidInput::tick).
__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    IORegistryEntry *driver=findEventDriver(device,4);
    // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it opened, not from the
    // HID device below it; publish them on the service itself. Set once only: every setProperty on a service with attached user
    // clients is a registry property-change notification, and repeating it each second keeps backboardd/hidd busy re-evaluating.
    static IORegistryEntry *publishedFor;
    if(driver && device->getProperty("Built-In") && publishedFor!=driver) {
        publishedFor=driver;
        driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
        // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:]) drops a digitizer event unless the sending
        // service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20 ("ignoring digitizer event ... from unsupported
        // service"). The combined device lists its keyboard collection first, so the services would publish (1,6); publish
        // Digitizer/Touch Screen on the services after they have matched (setting it on the device would change driver matching).
        driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",(unsigned long long)4,32);
        // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}: the flag on the interface (the event service's
        // provider) as well, not only on the device two levels down.
        if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
            interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
            interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",(unsigned long long)4,32);
        }
        printf("evb10: hid: digitizer properties published on the event service %p\n",static_cast<void *>(driver));
    }
    __asm__ volatile("" ::: "memory");
}
void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
}
// The original kernel matches a pointer (Generic Desktop Mouse / Pointer) only with the DriverKit AppleUserHIDEventDriver (score 0x4b1),
// whose dext driverkitd does not start here, so the interface of a mouse gets an event service that never works. A personality of the
// original IOHIDEventDriver (the class the keyboard and the digitizer use) for USB pointers, with a higher score, wins that match;
// the class parses the mouse's elements itself (X, Y, wheel, buttons) and dispatches Pointer / Scroll / Button events.
static bool addKernelPointerPersonality(void) {
    OSDictionary *personality=OSDictionary::withCapacity(8);
    OSArray *pairs=OSArray::withCapacity(2);
    if(!personality || !pairs){OSSafeReleaseNULL(personality);OSSafeReleaseNULL(pairs);return false;}
    for(unsigned usage=1;usage<=2;++usage) {
        OSDictionary *pair=OSDictionary::withCapacity(2);
        OSNumber *page=OSNumber::withNumber((unsigned long long)1,32),*use=OSNumber::withNumber((unsigned long long)usage,32);
        if(pair && page && use){pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",use);pairs->setObject(pair);}
        OSSafeReleaseNULL(pair);OSSafeReleaseNULL(page);OSSafeReleaseNULL(use);
    }
    OSString *bundle=OSString::withCString("com.apple.iokit.IOHIDFamily"),*klass=OSString::withCString("IOHIDEventDriver"),
             *provider=OSString::withCString("IOHIDInterface"),*category=OSString::withCString("IODefaultMatchCategory"),
             *transport=OSString::withCString("USB"),*behavior=OSString::withCString("");
    OSNumber *score=OSNumber::withNumber((unsigned long long)0x500,32);
    bool ok=bundle && klass && provider && category && transport && behavior && score && pairs->getCount()==2;
    if(ok) {
        personality->setObject("CFBundleIdentifier",bundle);personality->setObject("IOClass",klass);personality->setObject("IOProviderClass",provider);
        personality->setObject("IOMatchCategory",category);personality->setObject("IOProbeScore",score);personality->setObject("Transport",transport);
        personality->setObject("HIDDefaultBehavior",behavior);personality->setObject("DeviceUsagePairs",pairs);
        OSArray *drivers=OSArray::withCapacity(1);
        ok=drivers && drivers->setObject(personality) && gIOCatalogue && gIOCatalogue->addDrivers(drivers,true);
        OSSafeReleaseNULL(drivers);
    }
    OSSafeReleaseNULL(bundle);OSSafeReleaseNULL(klass);OSSafeReleaseNULL(provider);OSSafeReleaseNULL(category);OSSafeReleaseNULL(transport);
    OSSafeReleaseNULL(behavior);OSSafeReleaseNULL(score);OSSafeReleaseNULL(pairs);OSSafeReleaseNULL(personality);
    return ok;
}
// The input device(s), on the IOHIDResource that IOHIDFamily publishes (the original provider; nothing is replaced).
void evb10_hid_input_start(IOService *resource) {
    unsigned enabled=0,mode=0;
    if(!resource)return;
    // 1 keeps the device-only behaviour of round 2; 2 and above also start the injector. No new boot argument: the value of
    // the two arguments that already exist says how far to go, and it is recorded before the device is allocated, because
    // NativeHidInput::start reads it and IOHIDDevice::start is what asks for the report descriptor.
    if(PE_parse_boot_argn("opi5-hid-input",&enabled,sizeof(enabled)) && enabled)mode=enabled;
    else if(PE_parse_boot_argn("evb10-hid-input",&enabled,sizeof(enabled)) && enabled)mode=enabled;
    if(!mode)return;
    native_hid_input_mode=mode;
    if(native_hid_stage)return;
    native_hid_stage=1;
    const OSSymbol *name=OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    if(meta)native_hid_class_size=meta->getClassSize();
    OSSafeReleaseNULL(name);
    if(native_hid_class_size!=208) {
        printf("evb10: hid: IOHIDDevice is %u bytes, not the 208 NativeHidInput was audited against: input left off\n",native_hid_class_size);
        native_hid_stage=3;return;
    }
    // NativeHidInput's metaclass takes its superclass from here: this kernel does not export IOHIDDevice::gMetaClass,
    // and this is the one moment where the original class is both loaded and lookupable by name.
    opi5plus_hid_superclass=meta;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){printf("evb10: hid: preModLoad failed\n");native_hid_stage=3;return;}
    native_hid_static_init();native_hid_input_meta_status=OSMetaClass::postModLoad(handle);
    if(native_hid_input_meta_status){printf("evb10: hid: postModLoad %d\n",native_hid_input_meta_status);native_hid_stage=3;return;}
    IOService *device=native_hid_input_allocate();
    native_hid_input_object=(uintptr_t)device;
    const bool valid=device && device->metaCast("IOHIDDevice") && device->init();
    native_hid_input_observed(0,valid?0:kIOReturnError);
    const bool attached=valid && device->attach(resource);
    native_hid_input_observed(1,attached?0:kIOReturnError);
    const bool started=attached && device->start(resource);
    printf("evb10: hid: input device %p init %d attach %d start %d\n",static_cast<void *>(device),valid,attached,started);
    if(!started) {
        native_hid_input_observed(7,kIOReturnError);
        if(attached)device->detach(resource);
        OSSafeReleaseNULL(device);native_hid_input_object=0;
    }
    unsigned touchHid=0,split=0;
    PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split));
    if(started && split==1 && PE_parse_boot_argn("sheng-touch-hid",&touchHid,sizeof(touchHid)) && touchHid==1) {
        IOService *touch=native_hid_touch_allocate();
        const bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
        printf("evb10: hid: separate digitizer %p start %d\n",static_cast<void *>(touch),touchStarted);
        if(!touchStarted && touch){touch->detach(resource);OSSafeReleaseNULL(touch);}
    }
    // A USB mouse is a device of its own (Generic Desktop / Mouse), next to the keyboard device, whenever the USB driver
    // (package/Opi5plusUsb) is linked in -- that is what the weak native_usb_mouse_seq address tests. The kernel pointer
    // personality is registered first so the mouse interface's event service is the original IOHIDEventDriver, not the
    // DriverKit dext stand-in that never starts.
    if(started && &native_usb_mouse_seq) {
        printf("opi5: hid: kernel pointer personality %d\n",addKernelPointerPersonality());
        IOService *mouse=native_hid_mouse_allocate();
        const bool mouseStarted=mouse && mouse->metaCast("IOHIDDevice") && mouse->init() && mouse->attach(resource) && mouse->start(resource);
        printf("opi5: hid: mouse device %p start %d\n",static_cast<void *>(mouse),mouseStarted);
        if(!mouseStarted && mouse){mouse->detach(resource);OSSafeReleaseNULL(mouse);}
    }
    // The devices live for the boot: their service attachment and timer stay genuine live objects.
    native_hid_stage=3;
}
