#pragma once
// Static RVA Symbol Offsets for macOS Tahoe 25G229
// Generated from official Apple 25G229 AMD kexts
#include <stdint.h>
#include <stddef.h>
#include <mach/vm_types.h>

namespace rem660 {
namespace tahoe25G229 {

struct StaticSymbol {
    const char *name;
    uint8_t groupIndex; // 0=HWLibs, 1=Framebuffer, 2=Accelerator
    uint32_t rva;
};

inline bool matchSym(const char *a, const char *b) {
    if (!a || !b) return false;
    if (*a == '_' && *b != '_') a++;
    else if (*b == '_' && *a != '_') b++;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

inline constexpr StaticSymbol kTahoeSymbols[] = {
    {"_psp_asic_type_init", 0, 0x48b14u},
    {"_psp_tmr_init", 0, 0x4d609u},
    {"_smu_sw_init", 0, 0x6a1c3u},
    {"_smu_hw_init", 0, 0x6a505u},
    {"_smu_hw_exit", 0, 0x6a7f3u},
    {"_smu_sw_exit", 0, 0x6a858u},
    {"_ttlSetDeviceCapabilityEntry", 0, 0xa7f85u},
    {"_gc_get_hw_version", 0, 0x6d2cu},
    {"_psp_ring_create", 0, 0x4c6a0u},
    {"_vm_10_1_get_uma_physical_fb_offset", 0, 0x2fb12u},
    {"_psp_np_fw_load_capability_check", 0, 0x4dbcau},
    {"_gvm_hw_init", 0, 0x182b1u},
    {"_gvm_hw_exit", 0, 0x18996u},
    {"_gvm_sw_exit", 0, 0x18267u},
    {"_gvm_get_hwip_discovery_info", 0, 0x19834u},
    {"_soc15_reg_offset", 0, 0x1985eu},
    {"_gc_10_3_4_me_ucode", 0, 0xd66b68u},
    {"_gc_10_3_4_pfp_ucode", 0, 0xd66bd8u},
    {"_gc_10_3_4_ce_ucode", 0, 0xd66ba0u},
    {"_gc_10_3_4_mec_ucode", 0, 0xd66c48u},
    {"_gc_10_3_4_mec_jt_ucode", 0, 0xd66c80u},
    {"_gc_10_3_4_rlc_ucode", 0, 0xd66c10u},
    {"_gc_10_3_4_rlc_srlist_cntl", 0, 0xd66d98u},
    {"_gc_10_3_4_rlc_srlist_gpm_mem", 0, 0xd66dd0u},
    {"_gc_10_3_4_rlc_srlist_srm_mem", 0, 0xd66e08u},
    {"_gc_10_3_4_rlc_lx6_iram_ucode", 0, 0xd66cf0u},
    {"_gc_10_3_4_rlc_lx6_dram_ucode", 0, 0xd66d28u},
    {"_ttl_hw_init", 0, 0x90b62u},
    {"_gc_10_3_initialize_registers", 0, 0x3128bu},
    {"_psp_sw_exit", 0, 0x4a199u},
    {"_psp_asd_unload", 0, 0x4f0deu},
    {"_psp_dtm_unload", 0, 0x4f7f2u},
    {"_psp_hc_unload", 0, 0x508dbu},
    {"_psp_hdcp_unload", 0, 0x4b84bu},
    {"_psp_ras_unload", 0, 0x53eb4u},
    {"_psp_tmr_unload", 0, 0x4d933u},
    {"_pcie_ip_sw_init", 0, 0x23d57au},
    {"_Phw_CommonDynamicState_ApplyClocksAdjustRules", 0, 0x222ea0u},
    {"_PEM_ExcuteEventChain", 0, 0x10288bu},
    {"_PEM_Task_EnableDynamicStateManagement", 0, 0x1050e8u},
    {"_PEM_Task_NotifyHWOfPowerSource", 0, 0x107bc8u},
    {"__ZN20AtiPowerPlayServices12ppInitializeEP22PPDisplayConfiguration", 0, 0x2253a0u},
    {"_PHM_Initialize", 0, 0xbcdddu},
    {"_PP_Tables_Initialize", 0, 0xd8b75u},
    {"_PSM_Initialize", 0, 0xf8d76u},
    {"_PECI_Initialize", 0, 0xc9a21u},
    {"_PEM_Initialize", 0, 0x100211u},
    {"_Phw_SwSmu_UpdatePCIEParameters", 0, 0x20da3fu},
    {"_Phw_SwSmu_InitMaxSustainableClocks", 0, 0x20c86du},
    {"_Phw_SwSmu_GetAllClockRanges", 0, 0x20c3e7u},
    {"_execute_register_golden_settings", 0, 0x22ad83u},
    {"_ipi_smu_hw_init", 0, 0xa4548u},
    {"_sdma_5_2_3_get_fw_constants", 0, 0x58fe1u},
    {"_dmcub_dcn31_get_fw_constrant", 0, 0x65e6u},
    {"_gc_get_config", 0, 0x8804u},
    {"_sdma_get_config", 0, 0x59730u},
    {"_dmcub_load_fw", 0, 0x6ad6u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer4openEv", 1, 0x14d4cu},
    {"__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_", 1, 0x4f1e2u},
    {"__ZNK34AMDRadeonX6000_AmdRadeonController20doesAcceleratorExistEv", 1, 0x4ec80u},
    {"__ZN39AMDRadeonX6000_AmdRadeonControllerNavi219setupBootWatermarksEv", 1, 0x4496u},
    {"__ZNK34AMDRadeonX6000_AmdRadeonController7getLinkEh", 1, 0x4fbc0u},
    {"__ZN22AMDRadeonX6000_AmdLink20resetConnectedStatusEv", 1, 0x4581cu},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer9getMyLinkEh", 1, 0xe33cu},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter10gMetaClassE", 1, 0x30e2b0u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer21setPipeInterruptStateEb", 1, 0xadc2u},
    {"__ZN27AMDRadeonX6000_AmdDalHelper21dalAtomReadRegister32EPvj", 1, 0x4a22eu},
    {"__ZN27AMDRadeonX6000_AmdDalHelper22dalAtomWriteRegister32EPvjj", 1, 0x4a244u},
    {"__ZN27AMDRadeonX6000_AmdDalHelper19dalAtomExecCmdTableEPvjS0_", 1, 0x4a202u},
    {"__ZN24AMDRadeonX6000_AmdLogger15initWithPciInfoEP11IOPCIDevice", 1, 0x259au},
    {"__ZN22AMDRadeonX6000_AmdLink10detectLinkE17AmdFbDetectReasonP18DetectedLinkStatus", 1, 0x45830u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController17detectAllDisplaysEv", 1, 0x4dd64u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController14detectDisplaysEj", 1, 0x4e454u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController19detectDisplayOnLinkEh", 1, 0x4e51eu},
    {"__ZN34AMDRadeonX6000_AmdRadeonController20establishBootDisplayEv", 1, 0x4de20u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer29initializeFixedDisplayBindingEv", 1, 0x151e6u},
    {"__ZN32AMDRadeonX6000_AmdRegisterAccess11hwReadReg32Ej", 1, 0x1cc4eu},
    {"__ZN32AMDRadeonX6000_AmdRegisterAccess17readValidateReg32Ej", 1, 0x1ccc8u},
    {"__ZN32AMDRadeonX6000_AmdRegisterAccess10writeReg32Ejj", 1, 0x1cbdau},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25getAttributeForConnectionEijPm", 1, 0x36ceau},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25setAttributeForConnectionEijm", 1, 0x35e6au},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer16connectionChangeEv", 1, 0x16224u},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter24transactionVBlankHandlerEP8OSObjectPv", 1, 0x2ae92u},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter25transactionVUpdateHandlerEP8OSObjectPv", 1, 0x2aff4u},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter19enqueueTransactionsEy", 1, 0x2b90cu},
    // curdiag counters (FB LC_UUID C3FA8B38-...): prologues push rbp / mov rsp,rbp / push rbx|r15|r14|rbx.
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorImageEPv", 1, 0x164b8u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorStateEiib", 1, 0x165f8u},
    {"__ZN24AMDRadeonX6000_AmdCursor19commitCursorUpdatesEv", 1, 0x3247au},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter18dequeueTransationsEy", 1, 0x2c930u},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter24setVUpdateVBlankIntervalEv", 1, 0x2c148u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12setAttributeEjm", 1, 0x3757cu},
    {"__ZN29AMDRadeonX6000_AmdFbPresenter18enableTransactionsEv", 1, 0x2ad68u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer17setInterruptStateEPvj", 1, 0xac74u},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12connectFlagsEiiPj", 1, 0x4829eu},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer28getInformationForDisplayModeEiP24IODisplayModeInformation", 1, 0x47f4cu},
    {"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateDeviceInfoEv", 1, 0x3b428u},
    {"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateXGmiConfigEv", 1, 0x3b5b6u},
    {"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateMemorySizeEv", 1, 0x3b4a0u},
    // FB LC_UUID C3FA8B38-0702-3FCD-9878-C8FD05A7A724; prologue push rbp/mov rsp,rbp/push r14/push rbx/sub $0x60
    {"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi2328createControllerCoreServicesEv", 1, 0x3584cu},
    // Trace-only (FB LC_UUID C3FA8B38-...): prologue push rbp / mov rsp,rbp / push r15 / push r14.
    {"__ZN34AMDRadeonX6000_AmdRadeonController13createFbStubsEh", 1, 0x4bef0u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController15setupHwServicesEv", 1, 0x4c138u},
    // coreServices sub-step trace (LC_UUID C3FA8B38-0702-3FCD-9878-C8FD05A7A724, llvm-nm, 1 definition each;
    // first 5 bytes = push rbp; mov rsp,rbp; push r?? -> position-independent for Lilu's short-jump trampoline).
    {"__ZN34AMDRadeonX6000_AmdBiosParserHelper16createBiosParserER16AtomBiosInitData", 1, 0x16a4cu},
    {"__ZN34AMDRadeonX6000_AmdBiosParserHelper23initializeAtomBiosImageEPhm", 1, 0x16ca2u},
    {"__ZN34AMDRadeonX6000_AmdBiosParserHelper16readFirmwareInfoEv", 1, 0x16ef8u},
    {"__ZN32AMDRadeonX6000_AmdAsicInfoNavi2314createAsicInfoEP35AMDRadeonX6000_IAmdRadeonController", 1, 0x52a24u},
    {"__ZN26AMDRadeonX6000_AmdAsicInfo7refreshEv", 1, 0x3ba82u},
    {"__ZN26AMDRadeonX6000_AmdAsicInfo20populateMemoryConfigEv", 1, 0x3bf12u},
    {"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateFbLocationEv", 1, 0x3b504u},
    {"__ZN26AMDRadeonX6000_AmdAsicInfo17populateModelNameEv", 1, 0x3bd0eu},
    {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer5startEP9IOService", 1, 0x14b70u},
    {"__ZN30AMDRadeonX6000_AmdAgdcServices24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments", 1, 0x1f668u},
    {"__ZN30AMDRadeonX6000_AmdAgdcServices26validateConfigurationEntryEPK32AGDCResourceConfigurationEntry_t", 1, 0x223ccu},
    {"__ZN30AMDRadeonX6000_AmdAgdcServices26commitConfigurationForLinkEjPK18AGDCDPLinkConfig_t", 1, 0x23ae8u},
    {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5probeEP9IOServicePi", 2, 0xb88u},
    {"__ZN26AMDRadeonX6000_AMDHWMemory12initVRAMInfoEv", 2, 0x51a78u},
    {"__ZN26AMDRadeonX6000_AMDHWMemory17adjustVRAMAddressEy", 2, 0x52e44u},
    {"__ZN26AMDRadeonX6000_AMDHardware16powerUpHWEnginesEv", 2, 0x6ed10u},
    {"__ZN26AMDRadeonX6000_AMDHardware14startHWEnginesEv", 2, 0x6ee2cu},
    {"__ZN26AMDRadeonX6000_AMDHardware18updateHWInfoClocksEv", 2, 0x700a0u},
    {"__ZNK36AMDRadeonX6000_AMDAccelResourceAddr221shouldAllocScanoutDccEjjjj", 2, 0x2022eu},
    {"__ZN26AMDRadeonX6000_AMDHardware13resetHardwareEjPj", 2, 0x6fb8cu},
    {"__ZN30AMDRadeonX6000_AMDKIQHWChannel14submitKIQFrameEv", 2, 0x5b96au},
    {"__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel8startKIQEyyPK18AMD_PM4_QUEUE_SPECPj", 2, 0x8d4e6u},
    {"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine14initComputeMQDE18_eAMD_HW_RING_TYPE", 2, 0x6821cu},
    {"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine15initGraphicsMQDEv", 2, 0x683e4u},
    {"__ZN28AMDRadeonX6000_AMDRTHardware12getHWChannelE18_eAMD_CHANNEL_TYPE11SS_PRIORITYj", 2, 0x5d5a0u},
    {"__ZN26AMDRadeonX6000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", 2, 0x6f7ceu},
    {"__ZN11IOPCIDevice20extendedConfigRead16Ey", 5, 0x193c2u},
    {"__ZN11IOPCIDevice20extendedConfigRead32Ey", 5, 0x1b67au},
    {"__ZN11IOPCIDevice12configRead16Eh", 5, 0x1d420u},
    {"__ZN11IOPCIDevice12configRead16Em", 5, 0x1d420u},
    {"__ZN11IOPCIDevice12configRead32Eh", 5, 0x1d358u},
    {"__ZN11IOPCIDevice12configRead32Em", 5, 0x1d358u},
    {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", 2, 0xdc2u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController5startEP9IOService", 1, 0x4b6d8u},
    {"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235startEP9IOService", 1, 0x4b6d8u},
    {"__ZN19AMDRadeonController5startEP9IOService", 1, 0x4b6d8u},
    {"__ZN39AMDRadeonX6000_AmdRadeonControllerNavi25startEP9IOService", 1, 0x3e30u},
    {"__ZN30AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", 1, 0x445aeu},
    {"__ZN29AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", 1, 0x445aeu},
    {"__ZN14AmdGpuWrangler5probeEP9IOServicePi", 1, 0x445aeu},
    {"__ZN30AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", 1, 0x447c2u},
    {"__ZN29AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", 1, 0x447c2u},
    {"__ZN14AmdGpuWrangler5startEP9IOService", 1, 0x447c2u},
    {"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235probeEP9IOServicePi", 1, 0x35690u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController5probeEP9IOServicePi", 1, 0x4b390u},
    {"__ZN19AMDRadeonController5probeEP9IOServicePi", 1, 0x4b390u},
    {"__ZN14AmdTtlServices20createPciDeviceCacheEv", 0, 0xae940u},
    {"__ZN14AmdTtlServices20cosReadPciConfigDataEPvP32cos_access_pci_config_data_inputP33cos_access_pci_config_data_output", 0, 0xacdd0u},
    {"__ZN15AmdTtlInterface16createTtlServiceEP11IOPCIDevice", 0, 0xadf70u},
    {"__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", 3, 0x1650u},
    {"__ZN38AMDRadeonX6000_AMDRadeonHWServicesNavi5probeEP9IOServicePi", 3, 0x1650u},
    {"__ZN38AMDRadeonX6000_AMDRadeonHWServicesSWIP5startEP9IOService", 3, 0xff0u},
    {"__ZN38AMDRadeonX6000_AMDRadeonHWServicesNavi5startEP9IOService", 3, 0xff0u},
    {"__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract5startEP9IOService", 3, 0x18c0u},
    {"__ZN34AMDRadeonX6000_AmdRadeonController18populateDeviceInfoEPNS_10DeviceInfoE", 1, 0x4bdd2u},
    {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5probeEP9IOServicePi", 2, 0xb88u},
    {"__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5probeEP9IOServicePi", 2, 0xb88u},
    {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", 2, 0xdc2u},
    {"__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5startEP9IOService", 2, 0xdc2u},
    // vcn.cpp (opt-in hardware video)
    {"__ZN14AmdTtlServices11getFirmwareEPKcP10_TtlFwInfo", 0, 0xabfb0u},
    {"_mmhub_2_1_0_initialize_registers", 0, 0x32b58u},
    {"_mmhub_2_3_0_initialize_registers", 0, 0x32620u},
    {"_Phw_SwSmu_PowerGateVcn", 0, 0x20abacu},
    {"_Phw_SwSmu_PowerGateJpeg", 0, 0x20ac20u},
    {"_vcn_hw_init", 0, 0x7fe0fu},
    {"_vcn_start_queue", 0, 0x81cccu},
    {"__ZN29AMDRadeonX6000_AMDMMHub_2_1_115fillVMRegistersEP25AMD_GFX10_VM_REGS_PER_HUB", 2, 0x9db58u},
};

inline uint32_t lookupTahoeSymbolRVA_25G229(size_t groupIndex, const char *symbol) {
    if (!symbol) return 0;
    // 1. First search matching groupIndex
    for (size_t i = 0; i < sizeof(kTahoeSymbols) / sizeof(kTahoeSymbols[0]); ++i) {
        if (kTahoeSymbols[i].groupIndex == groupIndex && matchSym(kTahoeSymbols[i].name, symbol)) {
            return kTahoeSymbols[i].rva;
        }
    }
    // 2. Fallback search across all groups
    for (size_t i = 0; i < sizeof(kTahoeSymbols) / sizeof(kTahoeSymbols[0]); ++i) {
        if (matchSym(kTahoeSymbols[i].name, symbol)) {
            return kTahoeSymbols[i].rva;
        }
    }
    return 0;
}

inline mach_vm_address_t resolveSymbol(size_t groupIndex, const char *symbol, mach_vm_address_t base) {
    if (!base || !symbol) return 0;
    uint32_t rva = lookupTahoeSymbolRVA_25G229(groupIndex, symbol);
    if (rva) return base + rva;
    return 0;
}

} // namespace tahoe25G229
} // namespace rem660
