// Master plug-in logic
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <Backlight.hpp>
#include <DebugEnabler.hpp>
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <GPUDriversAMD/SMU.hpp>
#include <GPUDriversAMD/TTL/SWIP/SMU.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Hotfixes/AGDP.hpp>
#include <Hotfixes/X6000FB.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/RuntimeMC.hpp>
#include <iVega/AppleGFXHDA.hpp>
#include <iVega/DriverInjector.hpp>
#include <iVega/HWLibs.hpp>
#include <iVega/Regs/GC.hpp>
#include <iVega/Regs/NBIO.hpp>
#include <iVega/Regs/SMU.hpp>
#include <iVega/X5000.hpp>
#include <iVega/X6000FB.hpp>
#include <kern/clock.h>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/i386/vm_types.h>

static NRed moduleInstance;

NRed& NRed::singleton() { return moduleInstance; }

void NRed::init()
{
    SYSLOG("NRed", "|-----------------------------------------------------------------|");
    SYSLOG("NRed", "| Copyright 2022-2025 ChefKiss.                                   |");
    SYSLOG("NRed", "| If you've paid for this, you've been scammed. Ask for a refund! |");
    SYSLOG("NRed", "| Do not support tonymacx86. Support us, we truly care.           |");
    SYSLOG("NRed", "| Change the world for the better.                                |");
    SYSLOG("NRed", "|-----------------------------------------------------------------|");

    Backlight::singleton().init();

    lilu.onKextLoadForce(&kextRadeonX6000Framebuffer);
    lilu.onKextLoadForce(&kextRadeonX5000HWLibs);
    lilu.onKextLoadForce(&kextRadeonX5000);
    lilu.onKextLoadForce(&kextAGDP);
    lilu.onKextLoadForce(&kextAppleGFXHDA);

    lilu.onPatcherLoadForce(
        [](void* const, KernelPatcher& patcher)
        {
            singleton().processPatcher();
            if (singleton().getAttributes().isPhoenix()) {
                singleton().probePhoenix();
                SYSLOG("NRed", "Phoenix probe mode active; incompatible GFX9 driver injection is disabled");
                return;
            }
            iVega::DriverInjector::singleton().processPatcher(patcher);
            PenguinWizardry::RuntimeMCManager::singleton().processPatcher(patcher);
        },
        nullptr);

    lilu.onKextLoadForce(
        nullptr, 0,
        [](void* const, KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
        {
            if (singleton().getAttributes().isPhoenix()) { return; }
            Hotfixes::AGDP::singleton().processKext(patcher, id, slide, size);
            Hotfixes::X6000FB::singleton().processKext(patcher, id, slide, size);
            Backlight::singleton().processKext(patcher, id, slide, size);
            DebugEnabler::singleton().processKext(patcher, id, slide, size);
            iVega::X6000FB::singleton().processKext(patcher, id, slide, size);
            iVega::AppleGFXHDA::singleton().processKext(patcher, id, slide, size);
            iVega::X5000HWLibs::singleton().processKext(patcher, id, slide, size);
            iVega::X5000::singleton().processKext(patcher, id, slide, size);
        },
        nullptr);
}

void NRed::hwLateInit()
{
    if (this->rmmio != nullptr) { return; }

    this->iGPU->setMemoryEnable(true);
    this->iGPU->setBusMasterEnable(true);

    if (this->getVBIOS()) {
        this->vbiosData->appendByte(0, ATOMBIOS_IMAGE_SIZE - this->vbiosData->getLength());
        this->iGPU->setProperty("ATY,bin_image", this->vbiosData);
    }
    else {
        SYSLOG("NRed", "Failed to get VBIOS!");
    }

    this->rmmio =
        this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5, kIOMapInhibitCache | kIOMapAnywhere);
    PANIC_COND(this->rmmio == nullptr || this->rmmio->getLength() == 0, "NRed", "Failed to map RMMIO");
    this->rmmioPtr = reinterpret_cast<volatile UInt32*>(this->rmmio->getVirtualAddress());

    this->fbOffset    = static_cast<UInt64>(this->readReg32(GC_BASE_0 + MC_VM_FB_OFFSET) & 0xFFFFFF) << 24;
    this->devRevision = (this->readReg32(NBIO_BASE_2 + RCC_DEV0_EPF0_STRAP0) & RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK)
                        >> RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT;

    if (this->attributes.isRenoir()) {
        if (!this->attributes.isGreenSardine() && this->devRevision == 0 && this->pciRevision >= 0x80
            && this->pciRevision <= 0x84)
        {
            this->attributes.setRenoirE();
        }
    }
    else {
        if (this->devRevision >= 0x8) {
            this->attributes.setRaven2();
            this->enumRevision = 0x79;
        }
        else if (this->attributes.isPicasso()) {
            this->enumRevision = 0x41;
        }
        else if (this->devRevision == 1) {
            this->enumRevision = 0x20;
        }
        else {
            this->enumRevision = 0x1;
        }
    }

    DBGLOG("NRed", "deviceID = 0x%X", this->deviceID);
    DBGLOG("NRed", "pciRevision = 0x%X", this->pciRevision);
    DBGLOG("NRed", "fbOffset = 0x%llX", this->fbOffset);
    DBGLOG("NRed", "devRevision = 0x%X", this->devRevision);
    DBGLOG("NRed", "isPicasso = %s", this->attributes.isPicasso() ? "true" : "false");
    DBGLOG("NRed", "isRaven2 = %s", this->attributes.isRaven2() ? "true" : "false");
    DBGLOG("NRed", "isRenoir = %s", this->attributes.isRenoir() ? "true" : "false");
    DBGLOG("NRed", "isGreenSardine = %s", this->attributes.isGreenSardine() ? "true" : "false");
    DBGLOG("NRed", "enumRevision = 0x%X", this->enumRevision);
}

void NRed::processPatcher()
{
    const auto devInfo = DeviceInfo::create();
    assert(devInfo != nullptr);

    devInfo->processSwitchOff();

    PANIC_COND(devInfo->videoBuiltin == nullptr, "NRed", "No iGPU detected by Lilu");
    this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
    PANIC_COND(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigVendorID) != WIOKit::VendorID::ATIAMD, "NRed",
               "iGPU is not an AMD one");

    WIOKit::renameDevice(this->iGPU, "IGPU");
    WIOKit::awaitPublishing(this->iGPU);
    UInt8 builtInBytes[] = {0x00};
    this->iGPU->setProperty("built-in", builtInBytes, sizeof(builtInBytes));
    char slotNameBytes[] = "built-in";
    this->iGPU->setProperty("AAPL,slot-name", slotNameBytes, sizeof(slotNameBytes));
    char hdaGfxBytes[] = "onboard-1";
    this->iGPU->setProperty("hda-gfx", hdaGfxBytes, sizeof(hdaGfxBytes));

    this->deviceID = static_cast<UInt16>(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID));
    switch (this->deviceID) {
        case 0x15D8: {
            this->attributes.setPicasso();
        } break;
        case 0x15DD: {
        } break;
        case 0x164C:
        case 0x1636: {
            this->attributes.setRenoir();
            this->enumRevision = 0x91;
        } break;
        case 0x15E7:
        case 0x1638: {
            this->attributes.setRenoir();
            this->attributes.setGreenSardine();
            this->enumRevision = 0xA1;
        } break;
        case 0x15BF: {
            // Phoenix1 / Radeon 780M (GFX11.0.3). Do not route this through
            // the GFX9 Vega implementation: its register maps, firmware and
            // display engine are incompatible. Probe mode gathers the PCI
            // topology needed for a dedicated bring-up path.
            this->attributes.setPhoenix();
        } break;
        default: PANIC("NRed", "Unknown device ID: 0x%X", this->deviceID);
    }
    this->pciRevision = static_cast<UInt8>(WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigRevisionID));

    char name[128];
    for (size_t i = 0, ii = 0; i < devInfo->videoExternal.size(); i++) {
        auto device = OSDynamicCast(IOPCIDevice, devInfo->videoExternal[i].video);
        if (device == nullptr) { continue; }

        snprintf(name, arrsize(name), "GFX%zu", ii++);
        WIOKit::renameDevice(device, name);
        WIOKit::awaitPublishing(device);
    }

    DeviceInfo::deleter(devInfo);
}

void NRed::probePhoenix()
{
    static constexpr char model[] = "AMD Radeon 780M (Phoenix experimental probe)";
    static constexpr char architecture[] = "GFX11.0.3";
    static constexpr UInt32 probeVersion = 2;

    this->iGPU->setProperty("model", model);
    this->setProp32("NRed,phoenix-probe", probeVersion);
    this->iGPU->setProperty("NRed,phoenix-architecture", architecture);

    SYSLOG("NRed", "Phoenix1 detected: device=0x%04X revision=0x%02X", this->deviceID, this->pciRevision);
    for (UInt8 bar = 0; bar < 6; bar += 1) {
        const auto reg = static_cast<UInt8>(kIOPCIConfigBaseAddress0 + (bar * sizeof(UInt32)));
        const auto value = WIOKit::readPCIConfigValue(this->iGPU, reg);
        SYSLOG("NRed", "Phoenix PCI BAR%u = 0x%08X", bar, value);
    }

    if (!checkKernelArgument("-NRedPhoenixMMIOProbe")) {
        SYSLOG("NRed", "Phoenix MMIO probe disabled; use -NRedPhoenixMMIOProbe to enable read-only discovery reads");
        SYSLOG("NRed", "Phoenix acceleration remains disabled until a GFX11 driver path is available");
        return;
    }

    // These DWORD offsets are defined by AMDGPU as consistent across all
    // supported SoCs.  Keep this stage strictly read-only: no reset, firmware,
    // VM, interrupt, or ring register may be touched from probe mode.
    static constexpr UInt32 IP_DISCOVERY_VERSION = 0x16A00;
    static constexpr UInt32 RCC_CONFIG_MEMSIZE   = 0xDE3;
    static constexpr UInt32 DRIVER_SCRATCH_0     = 0x94;
    static constexpr UInt32 DRIVER_SCRATCH_1     = 0x95;
    static constexpr UInt32 DRIVER_SCRATCH_2     = 0x96;

    auto* const mmio = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5,
                                                               kIOMapInhibitCache | kIOMapAnywhere);
    if (mmio == nullptr) {
        SYSLOG("NRed", "Phoenix read-only MMIO probe could not map BAR5");
        return;
    }
    if (mmio->getLength() == 0) {
        SYSLOG("NRed", "Phoenix read-only MMIO probe mapped an empty BAR5");
        mmio->release();
        return;
    }

    auto* const ptr = reinterpret_cast<volatile UInt32*>(mmio->getVirtualAddress());
    const auto read = [mmio, ptr](const UInt32 reg, UInt32& value) -> bool {
        if ((static_cast<UInt64>(reg) * sizeof(UInt32)) >= mmio->getLength()) { return false; }
        value = ptr[reg];
        return true;
    };

    UInt32 discoveryVersion = 0, vramSizeMiB = 0, scratch0 = 0, scratch1 = 0, scratch2 = 0;
    const bool complete = read(IP_DISCOVERY_VERSION, discoveryVersion) && read(RCC_CONFIG_MEMSIZE, vramSizeMiB)
                       && read(DRIVER_SCRATCH_0, scratch0) && read(DRIVER_SCRATCH_1, scratch1)
                       && read(DRIVER_SCRATCH_2, scratch2);
    if (complete) {
        SYSLOG("NRed", "Phoenix discovery: version=%u VRAM=%u MiB scratch=[0x%08X 0x%08X 0x%08X]",
               discoveryVersion, vramSizeMiB, scratch0, scratch1, scratch2);
        this->setProp32("NRed,phoenix-ip-discovery-version", discoveryVersion);
        this->setProp32("NRed,phoenix-vram-size-mib", vramSizeMiB);
        this->setProp32("NRed,phoenix-discovery-scratch0", scratch0);
        this->setProp32("NRed,phoenix-discovery-scratch1", scratch1);
        this->setProp32("NRed,phoenix-discovery-scratch2", scratch2);
    }
    else {
        SYSLOG("NRed", "Phoenix BAR5 is too small for the read-only discovery probe (length=0x%llX)",
               mmio->getLength());
    }

    if (complete && checkKernelArgument("-NRedPhoenixPSPProbe")) {
        // IP discovery v3 reports MP0 base segment 1 at DWORD 0x16000.
        // These C2PMSG offsets come from AMD's public MP 13.0.4 register
        // headers. This stage observes PSP boot/ring state without writes.
        static constexpr UInt32 MP0_BASE1  = 0x16000;
        static constexpr UInt32 C2PMSG_35  = MP0_BASE1 + 0x63;
        static constexpr UInt32 C2PMSG_64  = MP0_BASE1 + 0x80;
        static constexpr UInt32 C2PMSG_67  = MP0_BASE1 + 0x83;
        static constexpr UInt32 C2PMSG_81  = MP0_BASE1 + 0x91;
        UInt32 msg35 = 0, msg64 = 0, msg67 = 0, msg81 = 0;
        const bool pspComplete = read(C2PMSG_35, msg35) && read(C2PMSG_64, msg64)
                              && read(C2PMSG_67, msg67) && read(C2PMSG_81, msg81);
        if (pspComplete) {
            this->setProp32("NRed,phoenix-psp-c2pmsg35", msg35);
            this->setProp32("NRed,phoenix-psp-c2pmsg64", msg64);
            this->setProp32("NRed,phoenix-psp-c2pmsg67", msg67);
            this->setProp32("NRed,phoenix-psp-c2pmsg81", msg81);
            this->iGPU->setProperty("NRed,phoenix-psp-bootloader-ready", (msg35 & 0x80000000U) != 0);
            this->iGPU->setProperty("NRed,phoenix-psp-sos-alive", msg81 != 0);
            SYSLOG("NRed", "Phoenix PSP read-only probe: C2PMSG[35]=0x%08X [64]=0x%08X [67]=0x%08X [81]=0x%08X ready=%s sos=%s",
                   msg35, msg64, msg67, msg81, (msg35 & 0x80000000U) != 0 ? "true" : "false",
                   msg81 != 0 ? "true" : "false");
        }
        else {
            SYSLOG("NRed", "Phoenix PSP read-only probe registers exceed BAR5 length=0x%llX", mmio->getLength());
        }
    }

    if (complete && checkKernelArgument("-NRedPhoenixGMCProbe")) {
        // Read the currently programmed GPU physical framebuffer window from
        // both GFXHUB 3.0 and MMHUB 3.0.1. Values are in 16 MiB units.
        static constexpr UInt32 GC_BASE0 = 0x1260;
        static constexpr UInt32 GCMC_VM_FB_LOCATION_BASE = GC_BASE0 + 0x1688;
        static constexpr UInt32 GCMC_VM_FB_LOCATION_TOP  = GC_BASE0 + 0x1689;
        static constexpr UInt32 MMHUB_BASE1 = 0x1A000;
        static constexpr UInt32 MMMC_VM_FB_OFFSET        = MMHUB_BASE1 + 0x08D7;
        static constexpr UInt32 MMMC_VM_FB_LOCATION_BASE = MMHUB_BASE1 + 0x08EC;
        static constexpr UInt32 MMMC_VM_FB_LOCATION_TOP  = MMHUB_BASE1 + 0x08ED;
        UInt32 gfxBase = 0, gfxTop = 0, mmOffset = 0, mmBase = 0, mmTop = 0;
        const bool gmcComplete = read(GCMC_VM_FB_LOCATION_BASE, gfxBase)
                              && read(GCMC_VM_FB_LOCATION_TOP, gfxTop)
                              && read(MMMC_VM_FB_OFFSET, mmOffset)
                              && read(MMMC_VM_FB_LOCATION_BASE, mmBase)
                              && read(MMMC_VM_FB_LOCATION_TOP, mmTop);
        if (gmcComplete) {
            gfxBase &= 0x00FFFFFFU; gfxTop &= 0x00FFFFFFU;
            mmOffset &= 0x00FFFFFFU; mmBase &= 0x00FFFFFFU; mmTop &= 0x00FFFFFFU;
            this->setProp32("NRed,phoenix-gfxhub-fb-base-16m", gfxBase);
            this->setProp32("NRed,phoenix-gfxhub-fb-top-16m", gfxTop);
            this->setProp32("NRed,phoenix-mmhub-fb-offset-16m", mmOffset);
            this->setProp32("NRed,phoenix-mmhub-fb-base-16m", mmBase);
            this->setProp32("NRed,phoenix-mmhub-fb-top-16m", mmTop);
            SYSLOG("NRed", "Phoenix GMC read-only probe: GFXHUB FB=[0x%06X..0x%06X] MMHUB FB=[0x%06X..0x%06X] offset=0x%06X (16MiB units)",
                   gfxBase, gfxTop, mmBase, mmTop, mmOffset);
        }
        else {
            SYSLOG("NRed", "Phoenix GMC read-only probe registers exceed BAR5 length=0x%llX", mmio->getLength());
        }
    }

    if (complete && checkKernelArgument("-NRedPhoenixVRAMAliasTest")) {
        // Reversible one-DWORD test in an otherwise unused part of the visible
        // BAR0 aperture. This establishes whether PSP/GPU physical addresses
        // use a VRAM-relative offset or MMHUB's programmed FB base.
        static constexpr UInt64 TEST_OFFSET = 0x0F000000ULL;
        static constexpr UInt64 MMHUB_FB_BASE = 0x8000000000ULL;
        static constexpr UInt32 MM_INDEX = 0x0;
        static constexpr UInt32 MM_DATA = 0x1;
        static constexpr UInt32 MM_INDEX_HI = 0x6;
        auto* const vram = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                                   kIOMapInhibitCache | kIOMapAnywhere);
        if (vram != nullptr && TEST_OFFSET + sizeof(UInt32) <= vram->getLength()) {
            auto* const vramPtr = reinterpret_cast<volatile UInt32*>(vram->getVirtualAddress());
            const auto indexedRead = [ptr](const UInt64 address) -> UInt32 {
                ptr[MM_INDEX] = static_cast<UInt32>(address) | 0x80000000U;
                ptr[MM_INDEX_HI] = static_cast<UInt32>(address >> 31);
                OSSynchronizeIO();
                return ptr[MM_DATA];
            };
            const size_t testIndex = static_cast<size_t>(TEST_OFFSET / sizeof(UInt32));
            const UInt32 saved = vramPtr[testIndex];
            const UInt32 marker = saved ^ 0x4E526564U;
            vramPtr[testIndex] = marker;
            OSSynchronizeIO();
            const UInt32 relativeRead = indexedRead(TEST_OFFSET);
            const UInt32 absoluteRead = indexedRead(MMHUB_FB_BASE + TEST_OFFSET);
            vramPtr[testIndex] = saved;
            OSSynchronizeIO();
            const UInt32 restored = vramPtr[testIndex];
            const bool relativeAlias = relativeRead == marker;
            const bool absoluteAlias = absoluteRead == marker;
            const bool restoreValid = restored == saved;
            this->iGPU->setProperty("NRed,phoenix-vram-relative-alias", relativeAlias);
            this->iGPU->setProperty("NRed,phoenix-vram-absolute-alias", absoluteAlias);
            this->iGPU->setProperty("NRed,phoenix-vram-alias-restored", restoreValid);
            this->setProp32("NRed,phoenix-vram-alias-offset", static_cast<UInt32>(TEST_OFFSET));
            SYSLOG("NRed", "Phoenix VRAM alias test: offset=0x%08X saved=0x%08X relative=0x%08X absolute=0x%08X restored=%s alias=[relative:%s absolute:%s]",
                   static_cast<UInt32>(TEST_OFFSET), saved, relativeRead, absoluteRead,
                   restoreValid ? "true" : "false", relativeAlias ? "true" : "false",
                   absoluteAlias ? "true" : "false");
        }
        else {
            SYSLOG("NRed", "Phoenix VRAM alias test could not map BAR0 offset 0x%llX", TEST_OFFSET);
        }
        if (vram != nullptr) { vram->release(); }
    }

    if (complete && checkKernelArgument("-NRedPhoenixPSPRingCreate")) {
        // First state-changing Phoenix stage. Reserve one 4 KiB page in the
        // visible VRAM aperture and expose its absolute MC address to PSP's
        // already-running secure OS as a kernel-mode (GPCOM) ring.
        static constexpr UInt64 VRAM_BASE = 0x8000000000ULL;
        static constexpr UInt64 RING_OFFSET = 0x0E000000ULL;
        static constexpr UInt32 RING_SIZE = 0x1000;
        static constexpr UInt32 MP0_BASE1 = 0x16000;
        static constexpr UInt32 C2PMSG_64 = MP0_BASE1 + 0x80;
        static constexpr UInt32 C2PMSG_67 = MP0_BASE1 + 0x83;
        static constexpr UInt32 C2PMSG_69 = MP0_BASE1 + 0x85;
        static constexpr UInt32 C2PMSG_70 = MP0_BASE1 + 0x86;
        static constexpr UInt32 C2PMSG_71 = MP0_BASE1 + 0x87;
        static constexpr UInt32 READY_MASK = 0x8000FFFFU;
        static constexpr UInt32 READY_VALUE = 0x80000000U;
        static constexpr UInt32 KM_RING_COMMAND = 2U << 16;
        auto* const ringVram = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                                       kIOMapInhibitCache | kIOMapAnywhere);
        UInt32 initialMailbox = 0;
        if (ringVram != nullptr && RING_OFFSET + RING_SIZE <= ringVram->getLength()
            && read(C2PMSG_64, initialMailbox) && (initialMailbox & READY_MASK) == READY_VALUE) {
            auto* const ring = reinterpret_cast<volatile UInt32*>(
                static_cast<UInt8*>(reinterpret_cast<void*>(ringVram->getVirtualAddress())) + RING_OFFSET);
            for (UInt32 i = 0; i < RING_SIZE / sizeof(UInt32); i += 1) { ring[i] = 0; }
            OSSynchronizeIO();

            const UInt64 ringAddress = VRAM_BASE + RING_OFFSET;
            ptr[C2PMSG_69] = static_cast<UInt32>(ringAddress);
            ptr[C2PMSG_70] = static_cast<UInt32>(ringAddress >> 32);
            ptr[C2PMSG_71] = RING_SIZE;
            OSSynchronizeIO();
            ptr[C2PMSG_64] = KM_RING_COMMAND;
            OSSynchronizeIO();
            IOSleep(20);

            UInt32 response = 0;
            bool created = false;
            for (UInt32 attempt = 0; attempt < 1000; attempt += 1) {
                if (read(C2PMSG_64, response) && (response & READY_MASK) == READY_VALUE) {
                    created = true;
                    break;
                }
                IOSleep(1);
            }
            UInt32 writePointer = 0;
            read(C2PMSG_67, writePointer);
            this->iGPU->setProperty("NRed,phoenix-psp-ring-created", created);
            this->setProp32("NRed,phoenix-psp-ring-response", response);
            this->setProp32("NRed,phoenix-psp-ring-wptr", writePointer);
            this->setProp32("NRed,phoenix-psp-ring-offset", static_cast<UInt32>(RING_OFFSET));
            SYSLOG("NRed", "Phoenix PSP KM ring create: address=0x%llX size=0x%X initial=0x%08X response=0x%08X wptr=0x%08X created=%s",
                   ringAddress, RING_SIZE, initialMailbox, response, writePointer, created ? "true" : "false");
        }
        else {
            this->iGPU->setProperty("NRed,phoenix-psp-ring-created", false);
            SYSLOG("NRed", "Phoenix PSP KM ring create precondition failed: BAR0=%s length=0x%llX mailbox=0x%08X",
                   ringVram != nullptr ? "mapped" : "unmapped", ringVram != nullptr ? ringVram->getLength() : 0,
                   initialMailbox);
        }
        if (ringVram != nullptr) { ringVram->release(); }
    }

    if (complete && checkKernelArgument("-NRedPhoenixPSPRingQuery")) {
        // Submit BOOTCFG_CMD_GET as a non-destructive end-to-end validation of
        // PSP ring memory, command memory, fence writes, and mailbox wptr.
        static constexpr UInt64 VRAM_BASE = 0x8000000000ULL;
        static constexpr UInt64 RING_OFFSET = 0x0E000000ULL;
        static constexpr UInt64 COMMAND_OFFSET = RING_OFFSET + 0x1000;
        static constexpr UInt64 FENCE_OFFSET = RING_OFFSET + 0x2000;
        static constexpr UInt32 MP0_BASE1 = 0x16000;
        static constexpr UInt32 C2PMSG_64 = MP0_BASE1 + 0x80;
        static constexpr UInt32 C2PMSG_67 = MP0_BASE1 + 0x83;
        static constexpr UInt32 GFX_CMD_ID_BOOT_CFG = 0x22;
        static constexpr UInt32 BOOTCFG_CMD_GET = 2;
        static constexpr UInt32 FRAME_DWORDS = 16;
        static constexpr UInt32 RESPONSE_DWORD = 864 / sizeof(UInt32);
        static constexpr UInt32 BOOTCFG_RESPONSE_DWORD = (864 + 64) / sizeof(UInt32);
        auto* const queryVram = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                                        kIOMapInhibitCache | kIOMapAnywhere);
        UInt32 ringStatus = 0;
        if (queryVram != nullptr && FENCE_OFFSET + 0x1000 <= queryVram->getLength()
            && read(C2PMSG_64, ringStatus) && (ringStatus & 0x8000FFFFU) == 0x80000000U) {
            auto* const base = reinterpret_cast<volatile UInt32*>(queryVram->getVirtualAddress());
            auto* const ring = base + (RING_OFFSET / sizeof(UInt32));
            auto* const command = base + (COMMAND_OFFSET / sizeof(UInt32));
            auto* const fence = base + (FENCE_OFFSET / sizeof(UInt32));
            for (UInt32 i = 0; i < 0x1000 / sizeof(UInt32); i += 1) {
                command[i] = 0;
                fence[i] = 0;
            }
            command[2] = GFX_CMD_ID_BOOT_CFG;
            command[8] = BOOTCFG_CMD_GET;

            const UInt64 commandAddress = VRAM_BASE + COMMAND_OFFSET;
            const UInt64 fenceAddress = VRAM_BASE + FENCE_OFFSET;
            for (UInt32 i = 0; i < FRAME_DWORDS; i += 1) { ring[i] = 0; }
            ring[0] = static_cast<UInt32>(commandAddress);
            ring[1] = static_cast<UInt32>(commandAddress >> 32);
            ring[3] = static_cast<UInt32>(fenceAddress);
            ring[4] = static_cast<UInt32>(fenceAddress >> 32);
            ring[5] = 1;
            OSSynchronizeIO();
            ptr[C2PMSG_67] = FRAME_DWORDS;
            OSSynchronizeIO();

            bool consumed = false;
            for (UInt32 attempt = 0; attempt < 2000; attempt += 1) {
                OSSynchronizeIO();
                if (fence[0] == 1) {
                    consumed = true;
                    break;
                }
                IOSleep(1);
            }
            const UInt32 responseStatus = command[RESPONSE_DWORD];
            const UInt32 bootConfig = command[BOOTCFG_RESPONSE_DWORD];
            UInt32 writePointer = 0;
            read(C2PMSG_67, writePointer);
            const bool queryValid = consumed && responseStatus == 0;
            this->iGPU->setProperty("NRed,phoenix-psp-ring-query-valid", queryValid);
            this->setProp32("NRed,phoenix-psp-ring-query-fence", fence[0]);
            this->setProp32("NRed,phoenix-psp-ring-query-status", responseStatus);
            this->setProp32("NRed,phoenix-psp-ring-query-boot-config", bootConfig);
            this->setProp32("NRed,phoenix-psp-ring-query-wptr", writePointer);
            SYSLOG("NRed", "Phoenix PSP BOOT_CFG query: consumed=%s fence=0x%08X status=0x%08X config=0x%08X wptr=0x%08X valid=%s",
                   consumed ? "true" : "false", fence[0], responseStatus, bootConfig, writePointer,
                   queryValid ? "true" : "false");
        }
        else {
            this->iGPU->setProperty("NRed,phoenix-psp-ring-query-valid", false);
            SYSLOG("NRed", "Phoenix PSP BOOT_CFG query precondition failed: BAR0=%s ring=0x%08X",
                   queryVram != nullptr ? "mapped" : "unmapped", ringStatus);
        }
        if (queryVram != nullptr) { queryVram->release(); }
    }

    if (complete && checkKernelArgument("-NRedPhoenixIPDiscovery")) {
        // AMD's public discovery format places a 10 KiB blob 64 KiB below
        // the end of VRAM when DRIVER_SCRATCH_2 does not provide an override.
        // BAR0 exposes only the first 256 MiB in this VM, so use the stable
        // MM_INDEX/MM_DATA aperture to read the header.  MM_INDEX and
        // MM_INDEX_HI are address selectors; this never writes VRAM or a GPU
        // engine register.
        static constexpr UInt32 MM_INDEX              = 0x0;
        static constexpr UInt32 MM_DATA               = 0x1;
        static constexpr UInt32 MM_INDEX_HI           = 0x6;
        static constexpr UInt64 DISCOVERY_TMR_OFFSET  = 64ULL << 10;
        static constexpr UInt32 BINARY_SIGNATURE      = 0x28211407;
        static constexpr UInt32 HEADER_DWORDS         = 16;

        const UInt64 vramBytes = static_cast<UInt64>(vramSizeMiB) << 20;
        if (vramBytes >= DISCOVERY_TMR_OFFSET) {
            const UInt64 discoveryOffset = vramBytes - DISCOVERY_TMR_OFFSET;
            UInt32 header[HEADER_DWORDS] {};
            UInt32 currentHigh = ~0U;
            for (UInt32 i = 0; i < HEADER_DWORDS; i += 1) {
                const UInt64 position = discoveryOffset + (static_cast<UInt64>(i) * sizeof(UInt32));
                const UInt32 high = static_cast<UInt32>(position >> 31);
                ptr[MM_INDEX] = static_cast<UInt32>(position) | 0x80000000U;
                if (high != currentHigh) {
                    ptr[MM_INDEX_HI] = high;
                    currentHigh = high;
                }
                OSSynchronizeIO();
                header[i] = ptr[MM_DATA];
            }

            const auto* const bytes = reinterpret_cast<const UInt8*>(header);
            const auto read16 = [bytes](const size_t offset) -> UInt16 {
                return static_cast<UInt16>(bytes[offset])
                     | static_cast<UInt16>(static_cast<UInt16>(bytes[offset + 1]) << 8);
            };
            const UInt16 major = read16(4);
            const UInt16 minor = read16(6);
            const UInt16 expectedChecksum = read16(8);
            const UInt16 binarySize = read16(10);
            const UInt16 tableCount = major >= 2 ? read16(12) : 6;
            const size_t tableListOffset = major >= 2 ? 16 : 12;
            const UInt16 ipTableOffset = read16(tableListOffset);

            SYSLOG("NRed", "Phoenix IP discovery header: signature=0x%08X version=%u.%u size=%u tables=%u",
                   header[0], major, minor, binarySize, tableCount);
            this->setProp32("NRed,phoenix-discovery-signature", header[0]);
            this->setProp32("NRed,phoenix-discovery-major", major);
            this->setProp32("NRed,phoenix-discovery-minor", minor);
            this->setProp32("NRed,phoenix-discovery-size", binarySize);
            this->setProp32("NRed,phoenix-discovery-table-count", tableCount);
            this->setProp32("NRed,phoenix-ip-table-offset", ipTableOffset);
            this->iGPU->setProperty("NRed,phoenix-discovery-header-valid", header[0] == BINARY_SIGNATURE);

            if (header[0] == BINARY_SIGNATURE && binarySize >= 20 && binarySize <= (10U << 10)
                && ipTableOffset < binarySize)
            {
                auto* const blob = IONew(UInt8, binarySize);
                if (blob == nullptr) {
                    SYSLOG("NRed", "Phoenix could not allocate %u bytes for IP discovery", binarySize);
                }
                else {
                    for (UInt32 offset = 0; offset < binarySize; offset += sizeof(UInt32)) {
                        const UInt64 position = discoveryOffset + offset;
                        const UInt32 high = static_cast<UInt32>(position >> 31);
                        ptr[MM_INDEX] = static_cast<UInt32>(position) | 0x80000000U;
                        if (high != currentHigh) {
                            ptr[MM_INDEX_HI] = high;
                            currentHigh = high;
                        }
                        OSSynchronizeIO();
                        const UInt32 value = ptr[MM_DATA];
                        for (UInt32 byte = 0; byte < sizeof(UInt32) && (offset + byte) < binarySize; byte += 1) {
                            blob[offset + byte] = static_cast<UInt8>(value >> (byte * 8));
                        }
                    }

                    UInt16 calculatedChecksum = 0;
                    for (UInt32 offset = 10; offset < binarySize; offset += 1) {
                        calculatedChecksum = static_cast<UInt16>(calculatedChecksum + blob[offset]);
                    }
                    const bool checksumValid = calculatedChecksum == expectedChecksum;
                    this->iGPU->setProperty("NRed,phoenix-discovery-checksum-valid", checksumValid);
                    SYSLOG("NRed", "Phoenix discovery checksum: calculated=0x%04X expected=0x%04X valid=%s",
                           calculatedChecksum, expectedChecksum, checksumValid ? "true" : "false");

                    const auto blob16 = [blob, binarySize](const size_t offset, UInt16& value) -> bool {
                        if ((offset + 2) > binarySize) { return false; }
                        value = static_cast<UInt16>(blob[offset])
                              | static_cast<UInt16>(static_cast<UInt16>(blob[offset + 1]) << 8);
                        return true;
                    };
                    const auto blob32 = [blob, binarySize](const size_t offset, UInt32& value) -> bool {
                        if ((offset + 4) > binarySize) { return false; }
                        value = static_cast<UInt32>(blob[offset])
                              | (static_cast<UInt32>(blob[offset + 1]) << 8)
                              | (static_cast<UInt32>(blob[offset + 2]) << 16)
                              | (static_cast<UInt32>(blob[offset + 3]) << 24);
                        return true;
                    };

                    UInt32 ipSignature = 0, ipTableID = 0;
                    UInt16 ipVersion = 0, ipSize = 0, dieCount = 0;
                    const bool ipHeaderValid = blob32(ipTableOffset, ipSignature)
                                            && blob16(ipTableOffset + 4, ipVersion)
                                            && blob16(ipTableOffset + 6, ipSize)
                                            && blob32(ipTableOffset + 8, ipTableID)
                                            && blob16(ipTableOffset + 12, dieCount)
                                            && ipSignature == 0x53445049U
                                            && ipSize >= 80
                                            && (static_cast<UInt32>(ipTableOffset) + ipSize) <= binarySize;
                    this->iGPU->setProperty("NRed,phoenix-ip-table-valid", ipHeaderValid);
                    this->setProp32("NRed,phoenix-ip-table-version", ipVersion);
                    this->setProp32("NRed,phoenix-ip-table-size", ipSize);
                    this->setProp32("NRed,phoenix-ip-table-die-count", dieCount);
                    SYSLOG("NRed", "Phoenix IP table: signature=0x%08X version=%u size=%u id=0x%08X dies=%u valid=%s",
                           ipSignature, ipVersion, ipSize, ipTableID, dieCount, ipHeaderValid ? "true" : "false");

                    if (ipHeaderValid && dieCount > 0 && dieCount <= 16) {
                        UInt16 dieOffset = 0, ipCount = 0;
                        // die_info starts at byte 14; the first die_offset is
                        // its second UInt16 and is relative to the binary.
                        if (blob16(ipTableOffset + 16, dieOffset) && blob16(dieOffset + 2, ipCount)) {
                            this->setProp32("NRed,phoenix-ip-count", ipCount);
                            size_t ipOffset = static_cast<size_t>(dieOffset) + 4;
                            const bool addresses64Bit = ipVersion == 4 && (blob[ipTableOffset + 78] & 1U) != 0;
                            const size_t addressSize = addresses64Bit ? sizeof(UInt64) : sizeof(UInt32);
                            for (UInt16 i = 0; i < ipCount && (ipOffset + 8) <= binarySize; i += 1) {
                                UInt16 hwID = 0;
                                if (!blob16(ipOffset, hwID)) { break; }
                                const UInt8 instance = blob[ipOffset + 2];
                                const UInt8 baseCount = blob[ipOffset + 3];
                                const UInt8 ipMajor = blob[ipOffset + 4];
                                const UInt8 ipMinor = blob[ipOffset + 5];
                                const UInt8 ipRevision = blob[ipOffset + 6];
                                const UInt8 versionExtension = blob[ipOffset + 7];
                                const UInt8 ipSubRevision = ipVersion >= 3 ? (versionExtension & 0x0FU) : 0;
                                const UInt8 ipVariant = ipVersion >= 3 ? (versionExtension >> 4) : 0;
                                const size_t nextOffset = ipOffset + 8 + (static_cast<size_t>(baseCount) * addressSize);
                                if (nextOffset > binarySize) { break; }

                                UInt32 base0 = 0;
                                if (baseCount > 0) { blob32(ipOffset + 8, base0); }
                                const UInt32 version = (static_cast<UInt32>(ipMajor) << 16)
                                                     | (static_cast<UInt32>(ipMinor) << 8) | ipRevision;
                                const UInt32 fullVersion = (static_cast<UInt32>(ipMajor) << 24)
                                                         | (static_cast<UInt32>(ipMinor) << 16)
                                                         | (static_cast<UInt32>(ipRevision) << 8)
                                                         | (static_cast<UInt32>(ipVariant) << 4)
                                                         | ipSubRevision;
                                char versionKey[48], fullVersionKey[56], variantKey[48], subRevisionKey[56], baseCountKey[52], baseKey[48];
                                snprintf(versionKey, sizeof(versionKey), "NRed,phoenix-ip-%u-%u-version", hwID, instance);
                                snprintf(fullVersionKey, sizeof(fullVersionKey), "NRed,phoenix-ip-%u-%u-full-version", hwID, instance);
                                snprintf(variantKey, sizeof(variantKey), "NRed,phoenix-ip-%u-%u-variant", hwID, instance);
                                snprintf(subRevisionKey, sizeof(subRevisionKey), "NRed,phoenix-ip-%u-%u-sub-revision", hwID, instance);
                                snprintf(baseCountKey, sizeof(baseCountKey), "NRed,phoenix-ip-%u-%u-base-count", hwID, instance);
                                this->setProp32(versionKey, version);
                                this->setProp32(fullVersionKey, fullVersion);
                                this->setProp32(variantKey, ipVariant);
                                this->setProp32(subRevisionKey, ipSubRevision);
                                this->setProp32(baseCountKey, baseCount);
                                // Phoenix uses multiple register segments per IP. In
                                // particular MP0/PSP status registers are not based at
                                // segment zero, so retain every v3 32-bit base.
                                if (!addresses64Bit) {
                                    for (UInt8 baseIndex = 0; baseIndex < baseCount; baseIndex += 1) {
                                        UInt32 baseAddress = 0;
                                        if (!blob32(ipOffset + 8 + (static_cast<size_t>(baseIndex) * sizeof(UInt32)), baseAddress)) { break; }
                                        snprintf(baseKey, sizeof(baseKey), "NRed,phoenix-ip-%u-%u-base%u", hwID, instance, baseIndex);
                                        this->setProp32(baseKey, baseAddress);
                                    }
                                }
                                SYSLOG("NRed", "Phoenix IP[%u]: hw=%u instance=%u version=%u.%u.%u variant=%u subrev=%u full=0x%08X bases=%u base0=0x%08X",
                                       i, hwID, instance, ipMajor, ipMinor, ipRevision, ipVariant, ipSubRevision,
                                       fullVersion, baseCount, base0);
                                ipOffset = nextOffset;
                            }
                        }
                    }
                    IODelete(blob, UInt8, binarySize);
                }
            }
        }
        else {
            SYSLOG("NRed", "Phoenix IP discovery skipped because VRAM size is smaller than its reserved offset");
        }
    }
    mmio->release();

    SYSLOG("NRed", "Phoenix acceleration remains disabled until a GFX11 driver path is available");
}

void NRed::setProp32(const char* const key, const UInt32 value) const { this->iGPU->setProperty(key, value, 32); }

UInt32 NRed::readReg32(const UInt32 reg) const
{
    if ((reg * sizeof(UInt32)) < this->rmmio->getLength()) { return this->rmmioPtr[reg]; }
    else {
        this->rmmioPtr[PCIE_INDEX2] = reg;
        return this->rmmioPtr[PCIE_DATA2];
    }
}

void NRed::writeReg32(const UInt32 reg, const UInt32 val) const
{
    if ((reg * sizeof(UInt32)) < this->rmmio->getLength()) { this->rmmioPtr[reg] = val; }
    else {
        this->rmmioPtr[PCIE_INDEX2] = reg;
        this->rmmioPtr[PCIE_DATA2]  = val;
    }
}

static UInt64 getCurTimeInNS()
{
    UInt64 uptime, uptimeNS;
    clock_get_uptime(&uptime);
    absolutetime_to_nanoseconds(uptime, &uptimeNS);
    return uptimeNS;
}

CAILResult NRed::waitForFunc(void* handle, bool (*func)(void* handle), const UInt32 timeoutMS)
{
    if (timeoutMS == 0) {
        while (!func(handle)) { }
        return kCAILResultOK;
    }

    const auto startTime = getCurTimeInNS();
    const auto timeoutNS = static_cast<UInt64>(timeoutMS) * 1000000;
    do {
        if (func(handle)) { return kCAILResultOK; }
    }
    while (getCurTimeInNS() - startTime <= timeoutNS);

    return kCAILResultNoResponse;
}

static bool smuWaitForResponseFunc(void* handle)
{
    const auto outResp = static_cast<UInt32*>(handle);

    const auto fwResp = NRed::singleton().readReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_90);
    if (fwResp != kSMUFWResponseNoResponse) {
        if (outResp != nullptr) { *outResp = fwResp; }
        return true;
    }

    return false;
}

CAILResult NRed::smuWaitForResponse(UInt32* outResp) const { return waitForFunc(outResp, smuWaitForResponseFunc); }

CAILResult NRed::sendMsgToSmc(const UInt32 msg, const UInt32 param, UInt32* const outParam) const
{
    this->smuWaitForResponse();

    this->writeReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_90, 0);
    this->writeReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_82, param);
    this->writeReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_66, msg);

    UInt32     resp = kSMUFWResponseNoResponse;
    const auto res  = this->smuWaitForResponse(&resp);

    if (res == kCAILResultOK && outParam != nullptr) { *outParam = this->readReg32(MP0_BASE_0 + MP1_SMN_C2PMSG_82); }

    return processSMUFWResponse(msg, resp);
}

static bool checkAtomBios(const UInt8* const bios, const size_t size)
{
    if (size < 0x49) {
        DBGLOG("NRed", "VBIOS size is invalid");
        return false;
    }

    if (bios[0] != 0x55 || bios[1] != 0xAA) {
        DBGLOG("NRed", "VBIOS signature <%x %x> is invalid", bios[0], bios[1]);
        return false;
    }

    UInt16 bios_header_start = bios[0x48] | static_cast<UInt16>(bios[0x49] << 8);
    if (!bios_header_start) {
        DBGLOG("NRed", "Unable to locate VBIOS header");
        return false;
    }

    UInt16 tmp = bios_header_start + 4;
    if (size < tmp) {
        DBGLOG("NRed", "BIOS header is broken");
        return false;
    }

    if (!memcmp(bios + tmp, "ATOM", 4) || !memcmp(bios + tmp, "MOTA", 4)) {
        DBGLOG("NRed", "ATOMBIOS detected");
        return true;
    }

    return false;
}

// Hack
class AppleACPIPlatformExpert : IOACPIPlatformExpert
{ friend class NRed; };

bool NRed::getVBIOSFromVFCT(const bool strict)
{
    DBGLOG("NRed", "Fetching VBIOS from VFCT table");
    auto expert = reinterpret_cast<AppleACPIPlatformExpert*>(this->iGPU->getPlatform());
    assert(expert != nullptr);

    auto vfctData = expert->getACPITableData("VFCT", 0);
    if (vfctData == nullptr) {
        DBGLOG("NRed", "No VFCT from AppleACPIPlatformExpert");
        return false;
    }

    auto vfct = static_cast<const VFCT*>(vfctData->getBytesNoCopy());
    assert(vfct != nullptr);

    if (sizeof(VFCT) > vfctData->getLength()) {
        DBGLOG("NRed", "VFCT table present but broken (too short).");
        return false;
    }

    auto vendor  = WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigVendorID);
    auto busNum  = this->iGPU->getBusNumber();
    auto devNum  = this->iGPU->getDeviceNumber();
    auto devFunc = this->iGPU->getFunctionNumber();

    for (auto offset = vfct->vbiosImageOffset; offset < vfctData->getLength();) {
        auto vHdr =
            static_cast<const GOPVideoBIOSHeader*>(vfctData->getBytesNoCopy(offset, sizeof(GOPVideoBIOSHeader)));
        if (vHdr == nullptr) {
            DBGLOG("NRed", "VFCT header out of bounds");
            return false;
        }

        auto vContent =
            static_cast<const UInt8*>(vfctData->getBytesNoCopy(offset + sizeof(GOPVideoBIOSHeader), vHdr->imageLength));
        if (vContent == nullptr) {
            DBGLOG("NRed", "VFCT VBIOS image out of bounds");
            return false;
        }

        offset += sizeof(GOPVideoBIOSHeader) + vHdr->imageLength;

        if (vHdr->imageLength != 0
            && (!strict || (vHdr->pciBus == busNum && vHdr->pciDevice == devNum && vHdr->pciFunction == devFunc))
            && vHdr->vendorID == vendor && vHdr->deviceID == this->deviceID)
        {
            if (checkAtomBios(vContent, vHdr->imageLength)) {
                this->vbiosData = OSData::withBytes(vContent, vHdr->imageLength);
                assert(this->vbiosData != nullptr);
                return true;
            }

            DBGLOG("NRed", "VFCT VBIOS is not an ATOMBIOS");
            return false;
        }
        else {
            DBGLOG("NRed",
                   "VFCT image does not match (pciBus: 0x%X pciDevice: 0x%X pciFunction: 0x%X "
                   "vendorID: 0x%X deviceID: 0x%X) or length is 0 (imageLength: 0x%X)",
                   vHdr->pciBus, vHdr->pciDevice, vHdr->pciFunction, vHdr->vendorID, vHdr->deviceID, vHdr->imageLength);
        }
    }

    DBGLOG("NRed", "VFCT table present but broken.");
    return false;
}

bool NRed::getVBIOSFromVRAM()
{
    auto bar0 =
        this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapWriteCombineCache | kIOMapAnywhere);
    if (!bar0 || !bar0->getLength()) {
        DBGLOG("NRed", "FB BAR not enabled");
        OSSafeReleaseNULL(bar0);
        return false;
    }
    auto   fb   = reinterpret_cast<const UInt8*>(bar0->getVirtualAddress());
    UInt32 size = 256 * 1024;    // ???
    if (!checkAtomBios(fb, size)) {
        DBGLOG("NRed", "VRAM VBIOS is not an ATOMBIOS");
        OSSafeReleaseNULL(bar0);
        return false;
    }
    this->vbiosData = OSData::withBytes(fb, size);
    assert(this->vbiosData != nullptr);
    OSSafeReleaseNULL(bar0);
    return true;
}

bool NRed::getVBIOSFromExpansionROM()
{
    const auto expansionROMBase = this->iGPU->extendedConfigRead32(kIOPCIConfigExpansionROMBase);
    if (expansionROMBase == 0) {
        DBGLOG("NRed", "No PCI Expansion ROM available");
        return false;
    }

    auto expansionROM =
        this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigExpansionROMBase, kIOMapInhibitCache | kIOMapAnywhere);
    if (expansionROM == nullptr) { return false; }
    const auto expansionROMLength = min(expansionROM->getLength(), ATOMBIOS_IMAGE_SIZE);
    if (expansionROMLength == 0) {
        DBGLOG("NRed", "PCI Expansion ROM is empty");
        expansionROM->release();
        return false;
    }

    // Enable reading the expansion ROMs
    this->iGPU->extendedConfigWrite32(kIOPCIConfigExpansionROMBase, expansionROMBase | 1);

    this->vbiosData = OSData::withBytes(reinterpret_cast<const void*>(expansionROM->getVirtualAddress()),
                                        static_cast<UInt32>(expansionROMLength));
    assert(this->vbiosData != nullptr);
    expansionROM->release();

    // Disable reading the expansion ROMs
    this->iGPU->extendedConfigWrite32(kIOPCIConfigExpansionROMBase, expansionROMBase);

    if (checkAtomBios(static_cast<const UInt8*>(this->vbiosData->getBytesNoCopy()), expansionROMLength)) {
        return true;
    }
    else {
        DBGLOG("NRed", "PCI Expansion ROM VBIOS is not an ATOMBIOS");
        OSSafeReleaseNULL(this->vbiosData);
        return false;
    }
}

bool NRed::getVBIOS()
{
    const auto biosImageProp = OSDynamicCast(OSData, this->iGPU->getProperty("ATY,bin_image"));
    if (biosImageProp != nullptr) {
        if (checkAtomBios(static_cast<const UInt8*>(biosImageProp->getBytesNoCopy()), biosImageProp->getLength())) {
            this->vbiosData = OSData::withData(biosImageProp);
            SYSLOG("NRed", "Warning: VBIOS manually overridden, make sure you know what you're doing.");
            return true;
        }
        else {
            SYSLOG("NRed", "Error: VBIOS override is invalid.");
        }
    }
    if (this->getVBIOSFromVFCT(true)) { DBGLOG("NRed", "Got VBIOS from VFCT."); }
    else {
        SYSLOG("NRed", "Failed to get VBIOS from VFCT, trying to get it from VRAM.");
        if (this->getVBIOSFromVRAM()) { DBGLOG("NRed", "Got VBIOS from VRAM."); }
        else {
            SYSLOG("NRed", "Failed to get VBIOS from VRAM, trying to get it from PCI Expansion ROM.");
            if (this->getVBIOSFromExpansionROM()) { DBGLOG("NRed", "Got VBIOS from PCI Expansion ROM."); }
            else {
                SYSLOG(
                    "NRed",
                    "Failed to get VBIOS from PCI Expansion ROM, trying to get it from VFCT (relaxed matches mode).");
                if (this->getVBIOSFromVFCT(false)) { DBGLOG("NRed", "Got VBIOS from VFCT (relaxed matches mode)."); }
                else {
                    SYSLOG("NRed", "Failed to get VBIOS from VFCT (relaxed matches mode).");
                    return false;
                }
            }
        }
    }
    return true;
}
