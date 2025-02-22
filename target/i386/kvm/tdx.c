/*
 * QEMU TDX support
 *
 * Copyright Intel
 *
 * Author:
 *      Xiaoyao Li <xiaoyao.li@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory
 *
 */

#include "qemu/osdep.h"
#include "qemu/mmap-alloc.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "standard-headers/asm-x86/kvm_para.h"
#include "sysemu/kvm.h"
#include "sysemu/sysemu.h"
#include "sysemu/tdx.h"
#include "sysemu/runstate.h"
#include "migration/vmstate.h"

#include "exec/address-spaces.h"
#include "exec/ramblock.h"
#include "hw/i386/apic_internal.h"
#include "hw/i386/e820_memory_layout.h"
#include "hw/i386/x86.h"
#include "hw/i386/tdvf.h"
#include "hw/i386/tdvf-hob.h"
#include "kvm_i386.h"
#include "tdx.h"
#include "tdx-vmcall-service.h"
#include "../cpu-internal.h"

#include "trace.h"

#define TDX_SUPPORTED_KVM_FEATURES  ((1U << KVM_FEATURE_NOP_IO_DELAY) | \
                                     (1U << KVM_FEATURE_PV_UNHALT) | \
                                     (1U << KVM_FEATURE_PV_TLB_FLUSH) | \
                                     (1U << KVM_FEATURE_PV_SEND_IPI) | \
                                     (1U << KVM_FEATURE_POLL_CONTROL) | \
                                     (1U << KVM_FEATURE_PV_SCHED_YIELD) | \
                                     (1U << KVM_FEATURE_MSI_EXT_DEST_ID))

#define TDX_MIN_TSC_FREQUENCY_KHZ   (100 * 1000)
#define TDX_MAX_TSC_FREQUENCY_KHZ   (10 * 1000 * 1000)

#define TDX_TD_ATTRIBUTES_DEBUG             BIT_ULL(0)
#define TDX_TD_ATTRIBUTES_SEPT_VE_DISABLE   BIT_ULL(28)
#define TDX_TD_ATTRIBUTES_MIG               BIT_ULL(29)
#define TDX_TD_ATTRIBUTES_PKS               BIT_ULL(30)
#define TDX_TD_ATTRIBUTES_PERFMON           BIT_ULL(63)

#define TDX_ATTRIBUTES_MAX_BITS      64

/*
 * Instance binding and ignore all the related TD fields when calculating
 * SERVTD_INFO_HASH. See TDX module ABI spec, Table 4.53 for details.
 */
#define TDX_MIGTD_ATTR_DEFAULT      0x000007ff00000001

static FeatureMask tdx_attrs_ctrl_fields[TDX_ATTRIBUTES_MAX_BITS] = {
    [30] = { .index = FEAT_7_0_ECX, .mask = CPUID_7_0_ECX_PKS },
    [31] = { .index = FEAT_7_0_ECX, .mask = CPUID_7_0_ECX_KeyLocker},
};

static FeatureDep xfam_dependencies[] = {
    /* XFAM[7:5] may be set to 111 only when XFAM[2] is set to 1 */
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_YMM_MASK },
        .to = { FEAT_XSAVE_XCR0_LO, XSTATE_AVX_512_MASK },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_YMM_MASK },
        .to = { FEAT_1_ECX,
                CPUID_EXT_FMA | CPUID_EXT_AVX | CPUID_EXT_F16C },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_YMM_MASK },
        .to = { FEAT_7_0_EBX, CPUID_7_0_EBX_AVX2 },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_YMM_MASK },
        .to = { FEAT_7_0_ECX, CPUID_7_0_ECX_VAES | CPUID_7_0_ECX_VPCLMULQDQ},
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_AVX_512_MASK },
        .to = { FEAT_7_0_EBX,
                CPUID_7_0_EBX_AVX512F | CPUID_7_0_EBX_AVX512DQ |
                CPUID_7_0_EBX_AVX512IFMA | CPUID_7_0_EBX_AVX512PF |
                CPUID_7_0_EBX_AVX512ER | CPUID_7_0_EBX_AVX512CD |
                CPUID_7_0_EBX_AVX512BW | CPUID_7_0_EBX_AVX512VL },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_AVX_512_MASK },
        .to = { FEAT_7_0_ECX,
                CPUID_7_0_ECX_AVX512_VBMI | CPUID_7_0_ECX_AVX512_VBMI2 |
                CPUID_7_0_ECX_AVX512VNNI | CPUID_7_0_ECX_AVX512BITALG |
                CPUID_7_0_ECX_AVX512_VPOPCNTDQ },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_AVX_512_MASK },
        .to = { FEAT_7_0_EDX,
                CPUID_7_0_EDX_AVX512_4VNNIW | CPUID_7_0_EDX_AVX512_4FMAPS |
                CPUID_7_0_EDX_AVX512_VP2INTERSECT | CPUID_7_0_EDX_AVX512_FP16 },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_AVX_512_MASK },
        .to = { FEAT_7_1_EAX, CPUID_7_1_EAX_AVX512_BF16 | CPUID_7_1_EAX_AVX_VNNI },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_PKRU_MASK },
        .to = { FEAT_7_0_ECX, CPUID_7_0_ECX_PKU },
    },
    {
        .from = { FEAT_XSAVE_XCR0_LO, XSTATE_AMX_MASK },
        .to = { FEAT_7_0_EDX,
                CPUID_7_0_EDX_AMX_BF16 | CPUID_7_0_EDX_AMX_TILE |
                CPUID_7_0_EDX_AMX_INT8}
    },
    /* XSS features */
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_RTIT_MASK },
        .to = { FEAT_7_0_EBX, CPUID_7_0_EBX_INTEL_PT },
    },
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_RTIT_MASK },
        .to = { FEAT_14_0_ECX, ~0ull },
    },
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_CET_MASK },
        .to = { FEAT_7_0_ECX, CPUID_7_0_ECX_CET_SHSTK },
    },
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_CET_MASK },
        .to = { FEAT_7_0_EDX, CPUID_7_0_EDX_CET_IBT },
    },
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_UINTR_MASK },
        .to = { FEAT_7_0_EDX, CPUID_7_0_EDX_UNIT },
    },
    {
        .from = { FEAT_XSAVE_XSS_LO, XSTATE_ARCH_LBR_MASK },
        .to = { FEAT_7_0_EDX, CPUID_7_0_EDX_ARCH_LBR },
    },
};

/*
 * Select a representative feature for each XFAM-controlled features.
 * e.g avx for all XFAM[2]. Only this typcial CPUID is allowed to be
 * configured. This can help prevent unintentional operation by the user.
 */
FeatureMask tdx_xfam_representative[] = {
    [XSTATE_YMM_BIT] = { .index = FEAT_1_ECX, .mask = CPUID_EXT_AVX },
    [XSTATE_OPMASK_BIT] = { .index = FEAT_7_0_EBX, .mask = CPUID_7_0_EBX_AVX512F },
    [XSTATE_ZMM_Hi256_BIT] = { .index = FEAT_7_0_EBX, .mask = CPUID_7_0_EBX_AVX512F },
    [XSTATE_Hi16_ZMM_BIT] = { .index = FEAT_7_0_EBX, .mask = CPUID_7_0_EBX_AVX512F },
    [XSTATE_RTIT_BIT] = { .index = FEAT_7_0_EBX, .mask = CPUID_7_0_EBX_INTEL_PT },
    [XSTATE_PKRU_BIT] = { .index = FEAT_7_0_ECX, .mask = CPUID_7_0_ECX_PKU },
    [XSTATE_CET_U_BIT] = { .index = FEAT_7_0_ECX, .mask = CPUID_7_0_ECX_CET_SHSTK },
    [XSTATE_CET_S_BIT] = { .index = FEAT_7_0_ECX, .mask = CPUID_7_0_ECX_CET_SHSTK },
    [XSTATE_ARCH_LBR_BIT] = { .index = FEAT_7_0_EDX, .mask = CPUID_7_0_EDX_ARCH_LBR },
    [XSTATE_XTILE_CFG_BIT] = { .index = FEAT_7_0_EDX, .mask = CPUID_7_0_EDX_AMX_TILE },
    [XSTATE_XTILE_DATA_BIT] = { .index = FEAT_7_0_EDX, .mask = CPUID_7_0_EDX_AMX_TILE },
};

typedef struct KvmTdxCpuidLookup {
    uint32_t tdx_fixed0;
    uint32_t tdx_fixed1;

    /*
     * The CPUID bits that are configurable from the view of TDX module
     * but require VMM emulation if configured to enabled by VMM.
     *
     * For those bits, they cannot be enabled actually if VMM (KVM/QEMU) cannot
     * virtualize them.
     */
    uint32_t vmm_fixup;

    bool inducing_ve;
    /*
     * The maximum supported feature set for given inducing-#VE leaf.
     * It's valid only when .inducing_ve is true.
     */
    uint32_t supported_on_ve;
} KvmTdxCpuidLookup;

 /*
  * QEMU maintained TDX CPUID lookup tables, which reflects how CPUIDs are
  * virtualized for guest TDs based on "CPUID virtualization" of TDX spec.
  *
  * Note:
  *
  * This table will be updated runtime by tdx_caps reported by platform.
  *
  */
static KvmTdxCpuidLookup tdx_cpuid_lookup[FEATURE_WORDS] = {
    [FEAT_1_EDX] = {
        .tdx_fixed0 =
            BIT(10) | BIT(20) | CPUID_IA64,
        .tdx_fixed1 =
            CPUID_MSR | CPUID_PAE | CPUID_MCE | CPUID_APIC |
            CPUID_MTRR | CPUID_MCA | CPUID_CLFLUSH | CPUID_DTS,
        .vmm_fixup =
            CPUID_ACPI | CPUID_PBE,
    },
    [FEAT_1_ECX] = {
        .tdx_fixed0 =
            CPUID_EXT_VMX | CPUID_EXT_SMX |
            BIT(16),
        .tdx_fixed1 =
            CPUID_EXT_DTES64 | CPUID_EXT_DSCPL |
            CPUID_EXT_CX16 | CPUID_EXT_PDCM | CPUID_EXT_X2APIC |
            CPUID_EXT_AES | CPUID_EXT_XSAVE | CPUID_EXT_RDRAND |
            CPUID_EXT_HYPERVISOR | CPUID_EXT_MONITOR,
        .vmm_fixup =
            CPUID_EXT_EST | CPUID_EXT_TM2 | CPUID_EXT_XTPR | CPUID_EXT_DCA,
    },
    [FEAT_8000_0001_EDX] = {
        .tdx_fixed1 =
            CPUID_EXT2_NX | CPUID_EXT2_PDPE1GB | CPUID_EXT2_RDTSCP |
            CPUID_EXT2_LM,
    },
    [FEAT_7_0_EBX] = {
        .tdx_fixed0 =
            CPUID_7_0_EBX_TSC_ADJUST | CPUID_7_0_EBX_SGX | CPUID_7_0_EBX_MPX,
        .tdx_fixed1 =
            CPUID_7_0_EBX_FSGSBASE | CPUID_7_0_EBX_RTM |
            CPUID_7_0_EBX_RDSEED | CPUID_7_0_EBX_SMAP |
            CPUID_7_0_EBX_CLFLUSHOPT | CPUID_7_0_EBX_CLWB |
            CPUID_7_0_EBX_SHA_NI | CPUID_7_0_EBX_HLE,
        .vmm_fixup =
            CPUID_7_0_EBX_PQM | CPUID_7_0_EBX_RDT_A,
    },
    [FEAT_7_0_ECX] = {
        .tdx_fixed0 =
            CPUID_7_0_ECX_FZM | CPUID_7_0_ECX_MAWAU |
            CPUID_7_0_ECX_ENQCMD | CPUID_7_0_ECX_SGX_LC,
        .tdx_fixed1 =
            CPUID_7_0_ECX_MOVDIR64B | CPUID_7_0_ECX_BUS_LOCK_DETECT,
        .vmm_fixup =
            CPUID_7_0_ECX_TME,
    },
    [FEAT_7_0_EDX] = {
        .tdx_fixed0 = CPUID_7_0_EDX_SGX_KEYS,
        .tdx_fixed1 =
            CPUID_7_0_EDX_SPEC_CTRL |
            CPUID_7_0_EDX_L1D_FLUSH | CPUID_7_0_EDX_ARCH_CAPABILITIES |
            CPUID_7_0_EDX_CORE_CAPABILITY | CPUID_7_0_EDX_SPEC_CTRL_SSBD,
        .vmm_fixup =
            CPUID_7_0_EDX_PCONFIG,
    },
    [FEAT_8000_0008_EBX] = {
        .tdx_fixed0 =
            ~CPUID_8000_0008_EBX_WBNOINVD,
        .tdx_fixed1 =
            CPUID_8000_0008_EBX_WBNOINVD,
    },
    [FEAT_XSAVE] = {
        .tdx_fixed1 =
            CPUID_XSAVE_XSAVEOPT | CPUID_XSAVE_XSAVEC |
            CPUID_XSAVE_XSAVES,
    },
    [FEAT_6_EAX] = {
        .inducing_ve = true,
        .supported_on_ve = -1U,
    },
    [FEAT_8000_0007_EDX] = {
        .inducing_ve = true,
        .supported_on_ve = -1U,
    },
    [FEAT_KVM] = {
        .inducing_ve = true,
        .supported_on_ve = TDX_SUPPORTED_KVM_FEATURES,
    },
    [FEAT_SGX_12_0_EAX] = {
        .tdx_fixed0 = -1U,
    },
    [FEAT_SGX_12_0_EBX] = {
        .tdx_fixed0 = -1U,
    },
    [FEAT_SGX_12_1_EAX] = {
        .tdx_fixed0 = -1U,
    },
};

static TdxGuest *tdx_guest;

static struct kvm_tdx_capabilities *tdx_caps;

/* It's valid after kvm_confidential_guest_init()->kvm_tdx_init() */
bool is_tdx_vm(void)
{
    return !!tdx_guest;
}

static inline uint32_t host_cpuid_reg(uint32_t function,
                                      uint32_t index, int reg)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t ret = 0;

    host_cpuid(function, index, &eax, &ebx, &ecx, &edx);

    switch (reg) {
    case R_EAX:
        ret |= eax;
        break;
    case R_EBX:
        ret |= ebx;
        break;
    case R_ECX:
        ret |= ecx;
        break;
    case R_EDX:
        ret |= edx;
        break;
    }
    return ret;
}

static inline uint32_t tdx_cap_cpuid_config(uint32_t function,
                                            uint32_t index, int reg)
{
    struct kvm_tdx_cpuid_config *cpuid_c;
    int ret = 0;
    int i;

    if (tdx_caps->nr_cpuid_configs <= 0) {
        return ret;
    }

    for (i = 0; i < tdx_caps->nr_cpuid_configs; i++) {
        cpuid_c = &tdx_caps->cpuid_configs[i];
        /* 0xffffffff in sub_leaf means the leaf doesn't require a sublesf */
        if (cpuid_c->leaf == function &&
            (cpuid_c->sub_leaf == 0xffffffff || cpuid_c->sub_leaf == index)) {
            switch (reg) {
            case R_EAX:
                ret = cpuid_c->eax;
                break;
            case R_EBX:
                ret = cpuid_c->ebx;
                break;
            case R_ECX:
                ret = cpuid_c->ecx;
                break;
            case R_EDX:
                ret = cpuid_c->edx;
                break;
            default:
                return 0;
            }
        }
    }
    return ret;
}

static FeatureWord get_cpuid_featureword_index(uint32_t function,
                                               uint32_t index, int reg)
{
    FeatureWord w;

    for (w = 0; w < FEATURE_WORDS; w++) {
        FeatureWordInfo *f = &feature_word_info[w];

        if (f->type == MSR_FEATURE_WORD || f->cpuid.eax != function ||
            f->cpuid.reg != reg ||
            (f->cpuid.needs_ecx && f->cpuid.ecx != index)) {
            continue;
        }

        return w;
    }

    return w;
}

#define KVM_TSX_CPUID   (CPUID_7_0_EBX_RTM | CPUID_7_0_EBX_HLE)
/*
 * TDX supported CPUID varies from what KVM reports. Adjust the result by
 * applying the TDX restrictions.
 */
void tdx_get_supported_cpuid(uint32_t function, uint32_t index, int reg,
                             uint32_t *ret)
{
    uint32_t vmm_cap = *ret;
    FeatureWord w;

    /* Only handle features leaves that recognized by feature_word_info[] */
    w = get_cpuid_featureword_index(function, index, reg);
    if (w == FEATURE_WORDS) {
        return;
    }

    if (tdx_cpuid_lookup[w].inducing_ve) {
        *ret &= tdx_cpuid_lookup[w].supported_on_ve;
        return;
    }

    /*
     * Include all the native bits as first step. It covers types
     * - As configured (if native)
     * - Native
     * - XFAM related and Attributes realted
     *
     * It also has side effect to enable unsupported bits, e.g., the
     * bits of "fixed0" type while present natively. It's safe because
     * the unsupported bits will be masked off by .fixed0 later.
     */
    *ret |= host_cpuid_reg(function, index, reg);

    /* Adjust according to "fixed" type in tdx_cpuid_lookup. */
    *ret |= tdx_cpuid_lookup[w].tdx_fixed1;
    *ret &= ~tdx_cpuid_lookup[w].tdx_fixed0;

    /*
     * Configurable cpuids are supported unconditionally. It's mainly to
     * include those configurable regardless of native existence.
     */
    *ret |= tdx_cap_cpuid_config(function, index, reg);

    /*
     * clear the configurable bits that require VMM emulation and VMM doesn't
     * report the support.
     */
    *ret &= ~(~vmm_cap & tdx_cpuid_lookup[w].vmm_fixup);

    if (function == 7 && index == 0 && reg == R_EBX && host_tsx_broken())
        *ret &= ~KVM_TSX_CPUID;

    if (function == 1 && reg == R_ECX && !enable_cpu_pm)
        *ret &= ~CPUID_EXT_MONITOR;

    /*
     * CPUID_HT is caculated in cpu_x86_cpuid() only relies on cpu toplogy,
     * so clear the bit here.
     */
    if (function == 1 && reg == R_EDX)
        *ret &= ~CPUID_HT;
}

void tdx_apply_xfam_dependencies(CPUState *cpu)
{
    X86CPU *x86_cpu = X86_CPU(cpu);
    CPUX86State *env = &x86_cpu->env;
    int i;

    for (i = 0; i < ARRAY_SIZE(xfam_dependencies); i++) {
        FeatureDep *d = &xfam_dependencies[i];
        if (!(env->features[d->from.index] & d->from.mask)) {
            uint64_t unavailable_features = env->features[d->to.index] & d->to.mask;

            /* Not an error unless the dependent feature was added explicitly */
            mark_unavailable_features(x86_cpu, d->to.index,
                                     unavailable_features & env->user_plus_features[d->to.index],
                                     "This feature cannot be enabled because its XFAM controlling bit is not enabled");
            env->features[d->to.index] &= ~unavailable_features;
        }
    }
}

static uint64_t tdx_get_xfam_bitmask(FeatureWord w, uint64_t bit_mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(xfam_dependencies); i++) {
        FeatureDep *d = &xfam_dependencies[i];
        if (w == d->to.index && bit_mask & d->to.mask) {
            return d->from.mask;
        }
    }
    return 0;
}

/* return bit field if xfam representative feature, otherwise -1 */
static int is_tdx_xfam_representative(FeatureWord w, uint64_t bit_mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(tdx_xfam_representative); i++) {
        FeatureMask *fm = &tdx_xfam_representative[i];
        if (w == fm->index && bit_mask & fm->mask) {
            return i;
        }
    }
    return -1;
}

static const char *tdx_xfam_representative_name(uint64_t xfam_mask)
{
    uint32_t delegate_index, delegate_feature;
    int bitnr, delegate_bitnr;
    const char *name;

    bitnr = ctz32(xfam_mask);
    delegate_index = tdx_xfam_representative[bitnr].index;
    delegate_feature = tdx_xfam_representative[bitnr].mask;
    delegate_bitnr = ctz32(delegate_feature);
    /* get XFAM feature delegate feature name */
    name = feature_word_info[delegate_index].feat_names[delegate_bitnr];
    assert(delegate_bitnr < 32 ||
           !(name &&
             feature_word_info[delegate_index].type == CPUID_FEATURE_WORD));
    return name;
}

static uint64_t tdx_disallow_minus_bits(FeatureWord w)
{
    FeatureWordInfo *wi = &feature_word_info[w];
    uint64_t ret = 0;
    int i;

    /*
     * TODO:
     * enable MSR feature configuration for TDX, disallow MSR feature
     * manipulation for TDX for now
     */
    if (wi->type == MSR_FEATURE_WORD) {
        return ~0ull;
    }

    /*
     * inducing_ve type is fully configured by VMM, i.e., all are allowed
     * to be removed
     */
    if (tdx_cpuid_lookup[w].inducing_ve) {
        return 0;
    }

    ret = tdx_cpuid_lookup[w].tdx_fixed1;

    for (i = 0; i < ARRAY_SIZE(xfam_dependencies); i++) {
        FeatureDep *d = &xfam_dependencies[i];
        if (w == d->to.index) {
            ret |= d->to.mask;
        }
    }

    for (i = 0; i < ARRAY_SIZE(tdx_xfam_representative); i++) {
        FeatureMask *fm = &tdx_xfam_representative[i];
        if (w == fm->index) {
            ret &= ~fm->mask;
        }
    }

    return ret;
}

void tdx_check_minus_features(CPUState *cpu)
{
    X86CPU *x86_cpu = X86_CPU(cpu);
    CPUX86State *env = &x86_cpu->env;
    FeatureWordInfo *wi;
    FeatureWord w;
    uint64_t disallow_minus_bits;
    uint64_t bitmask, xfam_controlling_mask;
    int i;

    char *reason;
    char xfam_dependency_str[100];
    char usual[]="TDX limitation";

    for (w = 0; w < FEATURE_WORDS; w++) {
        wi = &feature_word_info[w];

        if (wi->type == MSR_FEATURE_WORD) {
            continue;
        }

        disallow_minus_bits = env->user_minus_features[w] & tdx_disallow_minus_bits(w);

        for (i = 0; i < 64; i++) {
            bitmask = 1ULL << i;
            if (!(bitmask & disallow_minus_bits)) {
                continue;
            }

            xfam_controlling_mask = tdx_get_xfam_bitmask(w, bitmask);
            if (xfam_controlling_mask && is_tdx_xfam_representative(w, bitmask) == -1) {
                /*
                 * cannot fix env->feature[w] here since whether the bit i is
                 * set or cleared depends on the setting of its XFAM
                 * representative feature bit
                 */
                snprintf(xfam_dependency_str, sizeof(xfam_dependency_str),
                         "it depends on XFAM representative feature (%s)",
                 g_strdup(tdx_xfam_representative_name(xfam_controlling_mask)));
                reason = xfam_dependency_str;
            } else {
                /* set bit i since this feature cannot be removed */
                env->features[w] |= bitmask;
                reason = usual;
            }

            g_autofree char *feature_word_str = feature_word_description(wi, i);
            warn_report("This feature cannot be removed becuase %s: %s%s%s [bit %d]",
                         reason, feature_word_str,
                         wi->feat_names[i] ? "." : "",
                         wi->feat_names[i] ?: "", i);
        }
    }
}

enum tdx_ioctl_level{
    TDX_PLATFORM_IOCTL,
    TDX_VM_IOCTL,
    TDX_VCPU_IOCTL,
};

static int __tdx_ioctl(void *state, enum tdx_ioctl_level level, int cmd_id,
                        __u32 flags, void *data)
{
    struct kvm_tdx_cmd tdx_cmd;
    int r;

    memset(&tdx_cmd, 0x0, sizeof(tdx_cmd));

    tdx_cmd.id = cmd_id;
    tdx_cmd.flags = flags;
    tdx_cmd.data = (__u64)(unsigned long)data;

    switch (level) {
    case TDX_PLATFORM_IOCTL:
        r = kvm_ioctl(kvm_state, KVM_MEMORY_ENCRYPT_OP, &tdx_cmd);
        break;
    case TDX_VM_IOCTL:
        r = kvm_vm_ioctl(kvm_state, KVM_MEMORY_ENCRYPT_OP, &tdx_cmd);
        break;
    case TDX_VCPU_IOCTL:
        r = kvm_vcpu_ioctl(state, KVM_MEMORY_ENCRYPT_OP, &tdx_cmd);
        break;
    default:
        error_report("Invalid tdx_ioctl_level %d", level);
        exit(1);
    }

    return r;
}

static inline int tdx_platform_ioctl(int cmd_id, __u32 flags, void *data)
{
    return __tdx_ioctl(NULL, TDX_PLATFORM_IOCTL, cmd_id, flags, data);
}

static inline int tdx_vm_ioctl(int cmd_id, __u32 flags, void *data)
{
    return __tdx_ioctl(NULL, TDX_VM_IOCTL, cmd_id, flags, data);
}

static inline int tdx_vcpu_ioctl(void *vcpu_fd, int cmd_id, __u32 flags,
                                 void *data)
{
    return  __tdx_ioctl(vcpu_fd, TDX_VCPU_IOCTL, cmd_id, flags, data);
}

static void get_tdx_capabilities(void)
{
    struct kvm_tdx_capabilities *caps;
    /* 1st generation of TDX reports 6 cpuid configs */
    int nr_cpuid_configs = 6;
    int r, size;

    do {
        size = sizeof(struct kvm_tdx_capabilities) +
               nr_cpuid_configs * sizeof(struct kvm_tdx_cpuid_config);
        caps = g_malloc0(size);
        caps->nr_cpuid_configs = nr_cpuid_configs;

        r = tdx_platform_ioctl(KVM_TDX_CAPABILITIES, 0, caps);
        if (r == -EINVAL ) {
            g_free(caps);
            break;
        }

        if (r == -E2BIG) {
            g_free(caps);
            nr_cpuid_configs *= 2;
            if (nr_cpuid_configs > KVM_MAX_CPUID_ENTRIES) {
                error_report("KVM TDX seems broken that number of CPUID entries in kvm_tdx_capabilities exceeds limit");
                exit(1);
            }
        } else if (r < 0) {
            g_free(caps);
            error_report("KVM_TDX_CAPABILITIES failed: %s", strerror(-r));
            exit(1);
        }
    }
    while (r == -E2BIG);

    if (r == -EINVAL) {
        nr_cpuid_configs = 6;
        do {
            size = sizeof(struct kvm_tdx_capabilities) +
                nr_cpuid_configs * sizeof(struct kvm_tdx_cpuid_config);
            caps = g_malloc0(size);
            caps->nr_cpuid_configs = nr_cpuid_configs;

            r = tdx_vm_ioctl(KVM_TDX_CAPABILITIES, 0, caps);
            if (r == -E2BIG) {
                g_free(caps);
                if (nr_cpuid_configs > KVM_MAX_CPUID_ENTRIES) {
                    error_report("KVM TDX seems broken");
                    exit(1);
                }
                nr_cpuid_configs *= 2;
            } else if (r < 0) {
                g_free(caps);
                error_report("KVM_TDX_CAPABILITIES failed: %s\n", strerror(-r));
                exit(1);
            }
        }
        while (r == -E2BIG);
    }

    tdx_caps = caps;
}

static void update_tdx_cpuid_lookup_by_tdx_caps(void)
{
    KvmTdxCpuidLookup *entry;
    FeatureWordInfo *fi;
    uint32_t config;
    FeatureWord w;
    FeatureMask *fm;
    int i;

    /*
     * Patch tdx_fixed0/1 by tdx_caps that what TDX module reports as
     * configurable is not fixed.
     */
    for (w = 0; w < FEATURE_WORDS; w++) {
        fi = &feature_word_info[w];
        entry = &tdx_cpuid_lookup[w];

        if (fi->type != CPUID_FEATURE_WORD) {
            continue;
        }

        config = tdx_cap_cpuid_config(fi->cpuid.eax,
                                      fi->cpuid.needs_ecx ? fi->cpuid.ecx : ~0u,
                                      fi->cpuid.reg);

        entry->tdx_fixed0 &= ~config;
        entry->tdx_fixed1 &= ~config;
    }

    for (i = 0; i < ARRAY_SIZE(tdx_attrs_ctrl_fields); i++) {
        fm = &tdx_attrs_ctrl_fields[i];

        if (tdx_caps->attrs_fixed0 & (1ULL << i)) {
            tdx_cpuid_lookup[fm->index].tdx_fixed0 |= fm->mask;
        }

        if (tdx_caps->attrs_fixed1 & (1ULL << i)) {
            tdx_cpuid_lookup[fm->index].tdx_fixed1 |= fm->mask;
        }
    }

    /*
     * Because KVM gets XFAM settings via CPUID leaves 0xD,  map
     * tdx_caps->xfam_fixed{0, 1} into tdx_cpuid_lookup[].tdx_fixed{0, 1}.
     *
     * Then the enforment applies in tdx_get_configurable_cpuid() naturally.
     */
    tdx_cpuid_lookup[FEAT_XSAVE_XCR0_LO].tdx_fixed0 =
            (uint32_t)~tdx_caps->xfam_fixed0 & CPUID_XSTATE_XCR0_MASK;
    tdx_cpuid_lookup[FEAT_XSAVE_XCR0_LO].tdx_fixed1 =
            (uint32_t)tdx_caps->xfam_fixed1 & CPUID_XSTATE_XCR0_MASK;
    tdx_cpuid_lookup[FEAT_XSAVE_XCR0_HI].tdx_fixed0 =
            (~tdx_caps->xfam_fixed0 & CPUID_XSTATE_XCR0_MASK) >> 32;
    tdx_cpuid_lookup[FEAT_XSAVE_XCR0_HI].tdx_fixed1 =
            (tdx_caps->xfam_fixed1 & CPUID_XSTATE_XCR0_MASK) >> 32;

    tdx_cpuid_lookup[FEAT_XSAVE_XSS_LO].tdx_fixed0 =
            (uint32_t)~tdx_caps->xfam_fixed0 & CPUID_XSTATE_XSS_MASK;
    tdx_cpuid_lookup[FEAT_XSAVE_XSS_LO].tdx_fixed1 =
            (uint32_t)tdx_caps->xfam_fixed1 & CPUID_XSTATE_XSS_MASK;
    tdx_cpuid_lookup[FEAT_XSAVE_XSS_HI].tdx_fixed0 =
            (~tdx_caps->xfam_fixed0 & CPUID_XSTATE_XSS_MASK) >> 32;
    tdx_cpuid_lookup[FEAT_XSAVE_XSS_HI].tdx_fixed1 =
            (tdx_caps->xfam_fixed1 & CPUID_XSTATE_XSS_MASK) >> 32;
}

void tdx_set_tdvf_region(MemoryRegion *tdvf_region)
{
    assert(!tdx_guest->tdvf_region);
    tdx_guest->tdvf_region = tdvf_region;
}

static TdxFirmwareEntry *tdx_get_hob_entry(TdxGuest *tdx)
{
    TdxFirmwareEntry *entry;

    for_each_tdx_fw_entry(&tdx->tdvf, entry) {
        if (entry->type == TDVF_SECTION_TYPE_TD_HOB) {
            return entry;
        }
    }

    return NULL;
}

static void tdx_add_ram_entry(uint64_t address, uint64_t length, uint32_t type)
{
    uint32_t nr_entries = tdx_guest->nr_ram_entries;
    tdx_guest->ram_entries = g_renew(TdxRamEntry, tdx_guest->ram_entries,
                                     nr_entries + 1);

    tdx_guest->ram_entries[nr_entries].address = address;
    tdx_guest->ram_entries[nr_entries].length = length;
    tdx_guest->ram_entries[nr_entries].type = type;
    tdx_guest->nr_ram_entries++;
}

static TdxRamEntry *tdx_find_ram_range(uint64_t address, uint64_t length)
{
    TdxRamEntry *e;
    int i;

    for (i = 0; i < tdx_guest->nr_ram_entries; i++) {
        e = &tdx_guest->ram_entries[i];

        if (address + length <= e->address ||
            e->address + e->length <= address) {
                continue;
        }

        /*
         * The to-be-accepted ram range must be fully contained by one
         * RAM entry.
         */
        if (e->address > address ||
            e->address + e->length < address + length) {
            return NULL;
        }

        if (e->type == TDX_RAM_ADDED) {
            return NULL;
        }

        break;
    }

    if (i == tdx_guest->nr_ram_entries) {
        return NULL;
    }

    return e;
}

static int tdx_accept_ram_range(uint64_t address, uint64_t length)
{
    uint64_t head_start, tail_start, head_length, tail_length;
    uint64_t tmp_address, tmp_length;
    TdxRamEntry *e;

    e = tdx_find_ram_range(address, length);
    if (!e) {
        return -EINVAL;
    }

    tmp_address = e->address;
    tmp_length = e->length;

    e->address = address;
    e->length = length;
    e->type = TDX_RAM_ADDED;

    head_length = address - tmp_address;
    if (head_length > 0) {
        head_start = tmp_address;
        tdx_add_ram_entry(head_start, head_length, TDX_RAM_UNACCEPTED);
    }

    tail_start = address + length;
    if (tail_start < tmp_address + tmp_length) {
        tail_length = tmp_address + tmp_length - tail_start;
        tdx_add_ram_entry(tail_start, tail_length, TDX_RAM_UNACCEPTED);
    }

    return 0;
}

static int tdx_ram_entry_compare(const void *lhs_, const void* rhs_)
{
    const TdxRamEntry *lhs = lhs_;
    const TdxRamEntry *rhs = rhs_;

    if (lhs->address == rhs->address) {
        return 0;
    }
    if (le64_to_cpu(lhs->address) > le64_to_cpu(rhs->address)) {
        return 1;
    }
    return -1;
}

static void tdx_init_ram_entries(void)
{
    unsigned i, j, nr_e820_entries;

    nr_e820_entries = e820_get_num_entries();
    tdx_guest->ram_entries = g_new(TdxRamEntry, nr_e820_entries);

    for (i = 0, j = 0; i < nr_e820_entries; i++) {
        uint64_t addr, len;

        if (e820_get_entry(i, E820_RAM, &addr, &len)) {
            tdx_guest->ram_entries[j].address = addr;
            tdx_guest->ram_entries[j].length = len;
            tdx_guest->ram_entries[j].type = TDX_RAM_UNACCEPTED;
            j++;
        }
    }
    tdx_guest->nr_ram_entries = j;
}

static void tdx_post_init_vcpus(void)
{
    TdxFirmwareEntry *hob;
    void *hob_addr = NULL;
    CPUState *cpu;
    int r;

    hob = tdx_get_hob_entry(tdx_guest);
    if (hob) {
        hob_addr = (void *)hob->address;
    }

    CPU_FOREACH(cpu) {
        apic_force_x2apic(X86_CPU(cpu)->apic_state);

        r = tdx_vcpu_ioctl(cpu, KVM_TDX_INIT_VCPU, 0, hob_addr);
        if (r < 0) {
            error_report("KVM_TDX_INIT_VCPU failed %s", strerror(-r));
            exit(1);
        }
    }
}

static bool tdx_guest_need_prebinding(void)
{
    int i;
    uint64_t *qword = (uint64_t *)tdx_guest->migtd_hash;

    /*
     * migtd_hash by default is 0 which is deemed as invalid.
     * Pre-binding happens when user provided a non-0 hash value.
     */
    for (i = 0; i < KVM_TDX_SERVTD_HASH_SIZE / sizeof(uint64_t); i++) {
        if (qword[i] != 0) {
            return true;
        }
    }

    return false;
}

static bool tdx_guest_need_binding(void)
{
    /* User input the non-0 PID of a MigTD */
    return !!tdx_guest->migtd_pid;
}

static void tdx_binding_with_migtd_pid(void)
{
    struct kvm_tdx_servtd servtd;
    int r;

    servtd.version = KVM_TDX_SERVTD_VERSION;
    servtd.type = KVM_TDX_SERVTD_TYPE_MIGTD;
    servtd.attr = tdx_guest->migtd_attr;
    servtd.pid = tdx_guest->migtd_pid;

    r = tdx_vm_ioctl(KVM_TDX_SERVTD_BIND, 0, &servtd);
    if (r) {
        error_report("failed to bind migtd: %d", r);
    }
}

static void tdx_binding_with_migtd_hash(void)
{
    struct kvm_tdx_servtd servtd;
    int r;

    servtd.version = KVM_TDX_SERVTD_VERSION;
    servtd.type = KVM_TDX_SERVTD_TYPE_MIGTD;
    servtd.attr = tdx_guest->migtd_attr;
    memcpy(servtd.hash, tdx_guest->migtd_hash, KVM_TDX_SERVTD_HASH_SIZE);

    r = tdx_vm_ioctl(KVM_TDX_SERVTD_PREBIND, 0, &servtd);
    if (r) {
        error_report("failed to prebind migtd: %d", r);
    }
}

static void tdx_guest_init_vmcall_service_vtpm(TdxGuest *tdx);
static void tdx_finalize_vm(Notifier *notifier, void *unused)
{
    TdxFirmware *tdvf = &tdx_guest->tdvf;
    TdxFirmwareEntry *entry;
    RAMBlock *ram_block;
    int r;

    tdx_init_ram_entries();

    for_each_tdx_fw_entry(tdvf, entry) {
        switch (entry->type) {
        case TDVF_SECTION_TYPE_BFV:
        case TDVF_SECTION_TYPE_CFV:
        case TDVF_SECTION_TYPE_PAYLOAD:
            entry->mem_ptr = tdvf->mem_ptr + entry->data_offset;
            break;
        case TDVF_SECTION_TYPE_TD_HOB:
        case TDVF_SECTION_TYPE_TEMP_MEM:
            entry->mem_ptr = qemu_ram_mmap(-1, entry->size,
                                           qemu_real_host_page_size(), 0, 0);
            tdx_accept_ram_range(entry->address, entry->size);
            break;
        /* PERM_MEM is allocated and added later via PAGE.AUG */
        case TDVF_SECTION_TYPE_PERM_MEM:
            if (!tdx_find_ram_range(entry->address, entry->size)) {
                error_report("Failed to reserve ram for TDVF section %d",
                             entry->type);
                exit(1);
            }
            break;
        default:
            error_report("Unsupported TDVF section %d", entry->type);
            exit(1);
        }
    }

    qsort(tdx_guest->ram_entries, tdx_guest->nr_ram_entries,
          sizeof(TdxRamEntry), &tdx_ram_entry_compare);

    tdvf_hob_create(tdx_guest, tdx_get_hob_entry(tdx_guest));

    tdx_post_init_vcpus();

    /* Initial binding needs to be done before TD finalized */
    if (tdx_guest_need_binding()) {
        tdx_binding_with_migtd_pid();
    } else if (tdx_guest_need_prebinding()) {
        tdx_binding_with_migtd_hash();
    }

    /*
     * Don't finalize for the migration destination TD.
     * It will be finalzed after all the TD states successfully imported.
     */
    if (runstate_check(RUN_STATE_INMIGRATE)) {
        return;
    }

    for_each_tdx_fw_entry(tdvf, entry) {
        struct kvm_tdx_init_mem_region mem_region = {
            .source_addr = (__u64)entry->mem_ptr,
            .gpa = entry->address,
            .nr_pages = entry->size / 4096,
        };

        r = kvm_encrypt_reg_region(entry->address, entry->size, true);
        if (r < 0) {
             error_report("Reserve initial private memory failed %s", strerror(-r));
             exit(1);
        }

        if (entry->type == TDVF_SECTION_TYPE_PERM_MEM) {
            continue;
        }

        __u32 flags = entry->attributes & TDVF_SECTION_ATTRIBUTES_MR_EXTEND ?
                      KVM_TDX_MEASURE_MEMORY_REGION : 0;

        trace_kvm_tdx_init_mem_region(entry->type, entry->attributes, mem_region.source_addr, mem_region.gpa, mem_region.nr_pages);
        r = tdx_vm_ioctl(KVM_TDX_INIT_MEM_REGION, flags, &mem_region);
        if (r < 0) {
             error_report("KVM_TDX_INIT_MEM_REGION failed %s", strerror(-r));
             exit(1);
        }

        if (entry->type == TDVF_SECTION_TYPE_TD_HOB ||
            entry->type == TDVF_SECTION_TYPE_TEMP_MEM) {
            qemu_ram_munmap(-1, entry->mem_ptr, entry->size);
            entry->mem_ptr = NULL;
        }
    }

    /* Tdvf image was copied into private region above. It becomes unnecessary. */
    ram_block = tdx_guest->tdvf_region->ram_block;
    ram_block_discard_range(ram_block, 0, ram_block->max_length);

    r = tdx_vm_ioctl(KVM_TDX_FINALIZE_VM, 0, NULL);
    if (r < 0) {
        error_report("KVM_TDX_FINALIZE_VM failed %s", strerror(-r));
        exit(0);
    }

    tdx_guest_init_service_query(tdx_guest);
    tdx_guest_init_vmcall_service_vtpm(tdx_guest);
    tdx_guest->parent_obj.ready = true;
}

static Notifier tdx_machine_done_notify = {
    .notify = tdx_finalize_vm,
};

int tdx_kvm_init(MachineState *ms, Error **errp)
{
    X86MachineState *x86ms = X86_MACHINE(ms);
    TdxGuest *tdx = (TdxGuest *)object_dynamic_cast(OBJECT(ms->cgs),
                                                    TYPE_TDX_GUEST);

    if (x86ms->smm == ON_OFF_AUTO_AUTO) {
        x86ms->smm = ON_OFF_AUTO_OFF;
    } else if (x86ms->smm == ON_OFF_AUTO_ON) {
        error_setg(errp, "TDX VM doesn't support SMM");
        return -EINVAL;
    }

    if (x86ms->pic == ON_OFF_AUTO_AUTO) {
        x86ms->pic = ON_OFF_AUTO_OFF;
    } else if (x86ms->pic == ON_OFF_AUTO_ON) {
        error_setg(errp, "TDX VM doesn't support PIC");
        return -EINVAL;
    }

    x86ms->eoi_intercept_unsupported = true;

    if (!tdx_caps) {
        get_tdx_capabilities();
    }

    update_tdx_cpuid_lookup_by_tdx_caps();

    /*
     * Set kvm_readonly_mem_allowed to false, because TDX only supports readonly
     * memory for shared memory but not for private memory. Besides, whether a
     * memslot is private or shared is not determined by QEMU.
     *
     * Thus, just mark readonly memory not supported for simplicity.
     */
    kvm_readonly_mem_allowed = false;

    qemu_add_machine_init_done_notifier(&tdx_machine_done_notify);

    tdx_guest = tdx;

    if ((tdx->attributes & TDX_TD_ATTRIBUTES_DEBUG) &&
        kvm_vm_check_extension(kvm_state, KVM_CAP_ENCRYPT_MEMORY_DEBUG)) {
        kvm_setup_set_memory_region_debug_ops(kvm_state,
                                              kvm_encrypted_guest_set_memory_region_debug_ops);
        set_encrypted_memory_debug_ops();
    }

    return 0;
}

static int tdx_validate_attributes(TdxGuest *tdx)
{
    if (((tdx->attributes & tdx_caps->attrs_fixed0) | tdx_caps->attrs_fixed1) !=
        tdx->attributes) {
            error_report("Invalid attributes 0x%lx for TDX VM (fixed0 0x%llx, fixed1 0x%llx)",
                          tdx->attributes, tdx_caps->attrs_fixed0, tdx_caps->attrs_fixed1);
            return -EINVAL;
    }

    /*
    if (tdx->attributes & TDX_TD_ATTRIBUTES_DEBUG) {
        error_report("Current QEMU doesn't support attributes.debug[bit 0] for TDX VM");
        return -EINVAL;
    }
    */

    return 0;
}

static int setup_td_guest_attributes(X86CPU *x86cpu)
{
    CPUX86State *env = &x86cpu->env;

    tdx_guest->attributes |= (env->features[FEAT_7_0_ECX] & CPUID_7_0_ECX_PKS) ?
                             TDX_TD_ATTRIBUTES_PKS : 0;
    tdx_guest->attributes |= x86cpu->enable_pmu ? TDX_TD_ATTRIBUTES_PERFMON : 0;

    if (tdx_guest_need_prebinding() || tdx_guest_need_binding()) {
        tdx_guest->attributes |= TDX_TD_ATTRIBUTES_MIG;
        kvm_ram_default_shared = true;
    }

    return tdx_validate_attributes(tdx_guest);
}

int tdx_pre_create_vcpu(CPUState *cpu)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    X86CPU *x86cpu = X86_CPU(cpu);
    CPUX86State *env = &x86cpu->env;
    struct kvm_tdx_init_vm init_vm;
    uint32_t flags = 0;
    int r = 0;

    qemu_mutex_lock(&tdx_guest->lock);
    if (tdx_guest->initialized) {
        goto out;
    }

    r = kvm_vm_enable_cap(kvm_state, KVM_CAP_MAX_VCPUS, 0, ms->smp.cpus);
    if (r < 0) {
        error_report("Unable to set MAX VCPUS to %d", ms->smp.cpus);
        goto out;
    }

    r = -EINVAL;
    if (env->tsc_khz && (env->tsc_khz < TDX_MIN_TSC_FREQUENCY_KHZ ||
                         env->tsc_khz > TDX_MAX_TSC_FREQUENCY_KHZ)) {
        error_report("Invalid TSC %ld KHz, must specify cpu_frequency between [%d, %d] kHz",
                      env->tsc_khz, TDX_MIN_TSC_FREQUENCY_KHZ,
                      TDX_MAX_TSC_FREQUENCY_KHZ);
        goto out;
    }

    if (env->tsc_khz % (25 * 1000)) {
        error_report("Invalid TSC %ld KHz, it must be multiple of 25MHz", env->tsc_khz);
        goto out;
    }

    /* it's safe even env->tsc_khz is 0. KVM uses host's tsc_khz in this case */
    r = kvm_vm_ioctl(kvm_state, KVM_SET_TSC_KHZ, env->tsc_khz);
    if (r < 0) {
        error_report("Unable to set TSC frequency to %" PRId64 " kHz", env->tsc_khz);
        goto out;
    }

    r = setup_td_guest_attributes(x86cpu);
    if (r) {
        goto out;
    }

    memset(&init_vm, 0, sizeof(init_vm));
    init_vm.cpuid.nent = kvm_x86_arch_cpuid(env, init_vm.entries, 0);

    init_vm.attributes = tdx_guest->attributes;

    QEMU_BUILD_BUG_ON(sizeof(init_vm.mrconfigid) != sizeof(tdx_guest->mrconfigid));
    QEMU_BUILD_BUG_ON(sizeof(init_vm.mrowner) != sizeof(tdx_guest->mrowner));
    QEMU_BUILD_BUG_ON(sizeof(init_vm.mrownerconfig) != sizeof(tdx_guest->mrownerconfig));
    memcpy(init_vm.mrconfigid, tdx_guest->mrconfigid, sizeof(init_vm.mrconfigid));
    memcpy(init_vm.mrowner, tdx_guest->mrowner, sizeof(init_vm.mrowner));
    memcpy(init_vm.mrownerconfig, tdx_guest->mrownerconfig, sizeof(init_vm.mrownerconfig));

    if (runstate_check(RUN_STATE_INMIGRATE)) {
        flags = KVM_TDX_INIT_VM_F_POST_INIT;
    }

    r = tdx_vm_ioctl(KVM_TDX_INIT_VM, flags, &init_vm);
    if (r < 0) {
        error_report("KVM_TDX_INIT_VM failed %s", strerror(-r));
        goto out;
    }

    tdx_guest->initialized = true;

out:
    qemu_mutex_unlock(&tdx_guest->lock);
    return r;
}

int tdx_parse_tdvf(void *flash_ptr, int size)
{
    return tdvf_parse_metadata(&tdx_guest->tdvf, flash_ptr, size);
}

static bool tdx_guest_get_sept_ve_disable(Object *obj, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    return !!(tdx->attributes & TDX_TD_ATTRIBUTES_SEPT_VE_DISABLE);
}

static void tdx_guest_set_sept_ve_disable(Object *obj, bool value, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    if (value) {
        tdx->attributes |= TDX_TD_ATTRIBUTES_SEPT_VE_DISABLE;
    } else {
        tdx->attributes &= ~TDX_TD_ATTRIBUTES_SEPT_VE_DISABLE;
    }
}

static bool tdx_guest_get_debug(Object *obj, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    return !!(tdx->attributes & TDX_TD_ATTRIBUTES_DEBUG);
}

static void tdx_guest_set_debug(Object *obj, bool value, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    if (value) {
        tdx->attributes |= TDX_TD_ATTRIBUTES_DEBUG;
    } else {
        tdx->attributes &= ~TDX_TD_ATTRIBUTES_DEBUG;
    }
}

static char *tdx_guest_get_quote_generation(
    Object *obj, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    return g_strdup(tdx->quote_generation_str);
}

static void tdx_guest_set_quote_generation(
    Object *obj, const char *value, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    tdx->quote_generation = socket_parse(value, errp);
    if (!tdx->quote_generation)
        return;

    g_free(tdx->quote_generation_str);
    tdx->quote_generation_str = g_strdup(value);
}

#define UNASSIGNED_INTERRUPT_VECTOR     0

/* At destination, (re)-send all inflight requests to quoting server */
struct tdx_get_quote_state {
    uint64_t gpa;
    uint64_t buf_len;
    uint32_t apic_id;
    uint8_t event_notify_interrupt;
};

static const VMStateDescription tdx_get_quote_vmstate = {
    .name = "TdxGetQuote",
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(gpa, TdxGetQuoteState),
        VMSTATE_UINT64(buf_len, TdxGetQuoteState),
        VMSTATE_UINT32(apic_id, TdxGetQuoteState),
        VMSTATE_UINT8(event_notify_interrupt, TdxGetQuoteState),
        VMSTATE_END_OF_LIST()
    },
};

static int tdx_guest_pre_save(void *opaque);
static int tdx_guest_post_save(void *opaque);
static int tdx_guest_post_load(void *opaque, int version_id);

static const VMStateDescription tdx_guest_vmstate = {
    .name = TYPE_TDX_GUEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = &tdx_guest_pre_save,
    .post_save = &tdx_guest_post_save,
    .post_load = &tdx_guest_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(apic_id, TdxGuest),
        VMSTATE_UINT8(event_notify_interrupt, TdxGuest),

        VMSTATE_INT32(quote_generation_num, TdxGuest),
        VMSTATE_STRUCT_VARRAY_ALLOC(get_quote_state, TdxGuest,
                                    quote_generation_num, 0,
                                    tdx_get_quote_vmstate, TdxGetQuoteState),

        /*
         * quote_generation_str and quote_generation is local to the physical
         * machine. It must be specified on the destination.
         */
        VMSTATE_END_OF_LIST()
    }
};

static void tdx_migtd_get_pid(Object *obj, Visitor *v,
                              const char *name, void *opaque, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    visit_type_uint32(v, name, &tdx->migtd_pid, errp);
}

static void tdx_migtd_set_pid(Object *obj, Visitor *v,
                              const char *name, void *opaque, Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }

    tdx->migtd_pid = val;

    /* Binding on TD launch is performed after TD is initialized */
    if (!tdx_guest) {
        return;
    }

    /* Late binding is requested from qom-set when TD has been running */
    tdx_binding_with_migtd_pid();
}

/* tdx guest */
OBJECT_DEFINE_TYPE_WITH_INTERFACES(TdxGuest,
                                   tdx_guest,
                                   TDX_GUEST,
                                   CONFIDENTIAL_GUEST_SUPPORT,
                                   { TYPE_USER_CREATABLE },
                                   { NULL })

bool tdx_premig_is_done(void)
{
    struct kvm_tdx_get_migration_info info;

    memset(&info, 0, sizeof(struct kvm_tdx_get_migration_info));
    info.version = KVM_TDX_GET_MIGRATION_INFO_VERSION;
    tdx_vm_ioctl(KVM_TDX_GET_MIGRATION_INFO, 0, &info);

    return !!info.premig_done;
}

static void tdx_migtd_get_vsockport(Object *obj,
                                     Visitor *v,
                                     const char *name,
                                     void *opaque,
                                     Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    visit_type_uint32(v, name, &tdx->vsockport, errp);
}

static void tdx_migtd_set_vsockport(Object *obj,
                                     Visitor *v,
                                     const char *name,
                                     void *opaque,
                                     Error **errp)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    struct kvm_tdx_set_migration_info info;
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }

    tdx->vsockport = val;

    memset(&info, 0, sizeof(struct kvm_tdx_set_migration_info));
    info.version = KVM_TDX_SET_MIGRATION_INFO_VERSION;
    info.is_src = !runstate_check(RUN_STATE_INMIGRATE);
    info.vsock_port = tdx->vsockport;
    tdx_vm_ioctl(KVM_TDX_SET_MIGRATION_INFO, 0, &info);
}

static void tdx_guest_init_vmcall_service_vtpm(TdxGuest *tdx)
{
    TdxVmcallService *vms = &tdx->vmcall_service;

    if (!vms->vtpm_type)
        return;

    if (!vms->vtpm_path)
        return;

    if (!g_strcmp0(vms->vtpm_type, "client") &&
        !vms->vtpm_userid) {
        return;
    }

    if (!tdx_guest_init_vtpm(tdx))
    {
        bool vtpm_enabled = true;
        tdx_vm_ioctl(KVM_TDX_SET_VTPM_ENABLED, 0, &vtpm_enabled);
    }
}

static void tdx_guest_set_vtpm_type(Object *obj, const char *val, Error **err)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    TdxVmcallService *vms = &tdx->vmcall_service;

    if (vms->vtpm_type) {
        error_setg(err, "Invalid vtpm type: Duplicated value is not allowed");
        return;
    }

    if (g_strcmp0(val, "server") && g_strcmp0(val, "client")) {
        error_setg(err, "Invalid vtpm type: server or client");
        return;
    }

    g_free(vms->vtpm_type);
    vms->vtpm_type = g_strdup(val);
}

static void tdx_guest_set_vtpm_path(Object *obj, const char *val, Error **err)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    TdxVmcallService *vms = &tdx->vmcall_service;

    if (vms->vtpm_path) {
        error_setg(err, "Invalid vtpm path: Duplicated value is not allowed");
        return;
    }

    g_free(vms->vtpm_path);
    vms->vtpm_path = g_strdup(val);
}

static void tdx_guest_set_vtpm_userid(Object *obj, const char *val, Error **err)
{
    TdxGuest *tdx = TDX_GUEST(obj);
    TdxVmcallService *vms = &tdx->vmcall_service;

    if (vms->vtpm_userid) {
        error_setg(err, "Invalid vtpm userid: Duplicated value is not allowed");
        return;
    }

    g_free(vms->vtpm_userid);
    vms->vtpm_userid = g_strdup(val);
}

static void tdx_guest_init(Object *obj)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    qemu_mutex_init(&tdx->lock);

    tdx->attributes = TDX_TD_ATTRIBUTES_SEPT_VE_DISABLE;

    object_property_add_bool(obj, "sept-ve-disable",
                             tdx_guest_get_sept_ve_disable,
                             tdx_guest_set_sept_ve_disable);
    object_property_add_bool(obj, "debug",
                             tdx_guest_get_debug,
                             tdx_guest_set_debug);
    object_property_add_sha384(obj, "mrconfigid", tdx->mrconfigid,
                               OBJ_PROP_FLAG_READWRITE);
    object_property_add_sha384(obj, "mrowner", tdx->mrowner,
                               OBJ_PROP_FLAG_READWRITE);
    object_property_add_sha384(obj, "mrownerconfig", tdx->mrownerconfig,
                               OBJ_PROP_FLAG_READWRITE);
    object_property_add_sha384(obj, "migtd-hash", tdx->migtd_hash,
                               OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "migtd-attr",
                                   &tdx->migtd_attr, OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "migtd-pid", "uint32", tdx_migtd_get_pid,
                        tdx_migtd_set_pid, NULL, NULL);
    object_property_add(obj, "vsockport", "uint32", tdx_migtd_get_vsockport,
                        tdx_migtd_set_vsockport, NULL, NULL);

    tdx->quote_generation_str = NULL;
    tdx->quote_generation = NULL;
    object_property_add_str(obj, "quote-generation-service",
                            tdx_guest_get_quote_generation,
                            tdx_guest_set_quote_generation);

    object_property_set_bool(obj, CONFIDENTIAL_GUEST_SUPPORT_DISABLE_PV_CLOCK,
                             true, NULL);

    tdx->event_notify_interrupt = UNASSIGNED_INTERRUPT_VECTOR;
    tdx->apic_id = UNASSIGNED_APIC_ID;
    QLIST_INIT(&tdx->get_quote_task_list);

    object_property_add_str(obj, "vtpm-type",
                            NULL, tdx_guest_set_vtpm_type);
    object_property_add_str(obj, "vtpm-path",
                            NULL, tdx_guest_set_vtpm_path);
    object_property_add_str(obj, "vtpm-userid",
                            NULL, tdx_guest_set_vtpm_userid);

    tdx->migtd_attr = TDX_MIGTD_ATTR_DEFAULT;

    vmstate_register(NULL, 0, &tdx_guest_vmstate, tdx);
}

static void tdx_guest_finalize(Object *obj)
{
    TdxGuest *tdx = TDX_GUEST(obj);

    qemu_mutex_destroy(&tdx->lock);
    vmstate_unregister(NULL, &tdx_guest_vmstate, tdx);
}

static void tdx_guest_class_init(ObjectClass *oc, void *data)
{
}

#define TDG_VP_VMCALL_MAP_GPA                           0x10001ULL
#define TDG_VP_VMCALL_GET_QUOTE                         0x10002ULL
#define TDG_VP_VMCALL_SETUP_EVENT_NOTIFY_INTERRUPT      0x10004ULL
#define TDG_VP_VMCALL_SERVICE                           0x10005ULL

#define TDG_VP_VMCALL_SUCCESS           0x0000000000000000ULL
#define TDG_VP_VMCALL_RETRY             0x0000000000000001ULL
#define TDG_VP_VMCALL_INVALID_OPERAND   0x8000000000000000ULL
#define TDG_VP_VMCALL_ALIGN_ERROR       0x8000000000000002ULL

#define TDX_GET_QUOTE_STRUCTURE_VERSION 1ULL

#define TDX_VP_GET_QUOTE_SUCCESS                0ULL
#define TDX_VP_GET_QUOTE_IN_FLIGHT              (-1ULL)
#define TDX_VP_GET_QUOTE_ERROR                  0x8000000000000000ULL
#define TDX_VP_GET_QUOTE_QGS_UNAVAILABLE        0x8000000000000001ULL

/* Limit to avoid resource starvation. */
#define TDX_GET_QUOTE_MAX_BUF_LEN       (128 * 1024)
#define TDX_MAX_GET_QUOTE_REQUEST       16

/* Format of pages shared with guest. */
struct tdx_get_quote_header {
    /* Format version: must be 1 in little endian. */
    uint64_t structure_version;

    /*
     * GetQuote status code in little endian:
     *   Guest must set error_code to 0 to avoid information leak.
     *   Qemu sets this before interrupting guest.
     */
    uint64_t error_code;

    /*
     * in-message size in little endian: The message will follow this header.
     * The in-message will be send to QGS.
     */
    uint32_t in_len;

    /*
     * out-message size in little endian:
     * On request, out_len must be zero to avoid information leak.
     * On return, message size from QGS. Qemu overwrites this field.
     * The message will follows this header.  The in-message is overwritten.
     */
    uint32_t out_len;

    /*
     * Message buffer follows.
     * Guest sets message that will be send to QGS.  If out_len > in_len, guest
     * should zero remaining buffer to avoid information leak.
     * Qemu overwrites this buffer with a message returned from QGS.
     */
};

static hwaddr tdx_shared_bit(X86CPU *cpu)
{
    return (cpu->phys_bits > 48) ? BIT_ULL(51) : BIT_ULL(47);
}

static void tdx_handle_map_gpa(X86CPU *cpu, struct kvm_tdx_vmcall *vmcall)
{
    hwaddr addr_mask = (1ULL << cpu->phys_bits) - 1;
    hwaddr shared_bit = tdx_shared_bit(cpu);
    hwaddr gpa = vmcall->in_r12 & ~shared_bit;
    bool private = !(vmcall->in_r12 & shared_bit);
    hwaddr size = vmcall->in_r13;
    int ret = 0;

    trace_tdx_handle_map_gpa(gpa, size, private ? "private" : "shared");
    vmcall->status_code = TDG_VP_VMCALL_INVALID_OPERAND;

    if (gpa & ~addr_mask) {
        return;
    }
    if (!QEMU_IS_ALIGNED(gpa, 4096) || !QEMU_IS_ALIGNED(size, 4096)) {
        vmcall->status_code = TDG_VP_VMCALL_ALIGN_ERROR;
        return;
    }

    if (size > 0) {
        ret = kvm_convert_memory(gpa, size, private,
                                 cpu->parent_obj.cpu_index);
    }

    if (!ret) {
        vmcall->status_code = TDG_VP_VMCALL_SUCCESS;
    }
}

struct tdx_get_quote_task {
    QLIST_ENTRY(tdx_get_quote_task) list;

    hwaddr gpa;
    uint64_t buf_len;
    uint32_t apic_id;
    uint8_t event_notify_interrupt;

    char *out_data;
    uint64_t out_len;
    struct tdx_get_quote_header hdr;
    QIOChannelSocket *ioc;
    QEMUTimer timer;
    bool timer_armed;
};

struct x86_msi {
    union {
        struct {
            uint32_t    reserved_0              : 2,
                        dest_mode_logical       : 1,
                        redirect_hint           : 1,
                        reserved_1              : 1,
                        virt_destid_8_14        : 7,
                        destid_0_7              : 8,
                        base_address            : 12;
        } QEMU_PACKED x86_address_lo;
        uint32_t address_lo;
    };
    union {
        struct {
            uint32_t    reserved        : 8,
                        destid_8_31     : 24;
        } QEMU_PACKED x86_address_hi;
        uint32_t address_hi;
    };
    union {
        struct {
            uint32_t    vector                  : 8,
                        delivery_mode           : 3,
                        dest_mode_logical       : 1,
                        reserved                : 2,
                        active_low              : 1,
                        is_level                : 1;
        } QEMU_PACKED x86_data;
        uint32_t data;
    };
};

static int tdx_guest_pre_save(void *opaque)
{
    TdxGuest *_tdx_guest = opaque;
    struct tdx_get_quote_task *task;
    int32_t i = 0;

    qemu_mutex_lock(&_tdx_guest->lock);
    _tdx_guest->get_quote_state = g_malloc_n(_tdx_guest->quote_generation_num,
                                             sizeof(*_tdx_guest->get_quote_state));
    QLIST_FOREACH(task, &_tdx_guest->get_quote_task_list, list) {
        _tdx_guest->get_quote_state[i] = (struct tdx_get_quote_state) {
            .gpa = task->gpa,
            .buf_len = task->buf_len,
            .apic_id = task->apic_id,
            .event_notify_interrupt = task->event_notify_interrupt,
        };

        assert(i < _tdx_guest->quote_generation_num);
        i++;
    }
    qemu_mutex_unlock(&_tdx_guest->lock);
    return 0;
}

static int tdx_guest_post_save(void *opaque)
{
    TdxGuest *_tdx_guest = opaque;

    g_free(_tdx_guest->get_quote_state);
    _tdx_guest->get_quote_state = NULL;
    return 0;
}

static void __tdx_handle_get_quote(MachineState *ms, TdxGuest *tdx,
                                   hwaddr gpa, uint64_t buf_len,
                                   uint32_t apic_id,
                                   uint8_t event_notify_interrupt,
                                   struct kvm_tdx_vmcall *vmcall);

static int tdx_guest_post_load(void *opaque, int version_id)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    TdxGuest *_tdx_guest = opaque;
    uint32_t inflight_quote_num = _tdx_guest->quote_generation_num;
    int i;

    /* reset the quote num and re-trigger the inflight quote in dst-TD */
    _tdx_guest->quote_generation_num = 0;
    for (i = 0; i < inflight_quote_num; i++) {
        TdxGetQuoteState *state = &_tdx_guest->get_quote_state[i];

        __tdx_handle_get_quote(ms, _tdx_guest, state->gpa, state->buf_len,
                               state->apic_id, state->event_notify_interrupt,
                               NULL);
    }

    g_free(_tdx_guest->get_quote_state);
    _tdx_guest->get_quote_state = NULL;
    return 0;
}

static void tdx_handle_get_quote(X86CPU *cpu, struct kvm_tdx_vmcall *vmcall)
{
    hwaddr gpa = vmcall->in_r12;
    uint64_t buf_len = vmcall->in_r13;
    MachineState *ms;
    TdxGuest *tdx;

    trace_tdx_handle_get_quote(gpa, buf_len);
    vmcall->status_code = TDG_VP_VMCALL_INVALID_OPERAND;

    /* GPA must be shared. */
    if (!(gpa & tdx_shared_bit(cpu))) {
        return;
    }
    gpa &= ~tdx_shared_bit(cpu);

    if (!QEMU_IS_ALIGNED(gpa, 4096) || !QEMU_IS_ALIGNED(buf_len, 4096)) {
        vmcall->status_code = TDG_VP_VMCALL_ALIGN_ERROR;
        return;
    }
    if (buf_len == 0) {
        /*
         * REVERTME: Accept old GHCI GetQuote with R13 buf_len = 0.
         * buf size is 8KB. also hdr.out_len includes the header size.
         */
#define GHCI_GET_QUOTE_BUFSIZE_OLD      (8 * 1024)
        warn_report("Guest attestation driver uses old GetQuote ABI.(R13 == 0) "
                    "Please upgrade guest kernel.\n");
        buf_len = GHCI_GET_QUOTE_BUFSIZE_OLD;
    }

    ms = MACHINE(qdev_get_machine());
    tdx = TDX_GUEST(ms->cgs);
    __tdx_handle_get_quote(ms, tdx, gpa, buf_len, UNASSIGNED_APIC_ID,
                           UNASSIGNED_INTERRUPT_VECTOR, vmcall);
}

static int tdx_td_notify(uint32_t apic_id, int vector)
{
    struct x86_msi x86_msi;
    struct kvm_msi msi;

    /* It is optional for host VMM to interrupt TD. */
    if(!(32 <= vector && vector <= 255))
        return 0;

    x86_msi = (struct x86_msi) {
        .x86_address_lo  = {
            .reserved_0 = 0,
            .dest_mode_logical = 0,
            .redirect_hint = 0,
            .reserved_1 = 0,
            .virt_destid_8_14 = 0,
            .destid_0_7 = apic_id & 0xff,
        },
        .x86_address_hi = {
            .reserved = 0,
            .destid_8_31 = apic_id >> 8,
        },
        .x86_data = {
            .vector = vector,
            .delivery_mode = APIC_DM_FIXED,
            .dest_mode_logical = 0,
            .reserved = 0,
            .active_low = 0,
            .is_level = 0,
        },
    };
    msi = (struct kvm_msi) {
        .address_lo = x86_msi.address_lo,
        .address_hi = x86_msi.address_hi,
        .data = x86_msi.data,
        .flags = 0,
        .devid = 0,
    };

    return  kvm_vm_ioctl(kvm_state, KVM_SIGNAL_MSI, &msi);
}

static void tdx_getquote_task_cleanup(struct tdx_get_quote_task *t, bool outlen_overflow)
{
    MachineState *ms;
    TdxGuest *tdx;
    int ret;

    if (t->hdr.error_code != cpu_to_le64(TDX_VP_GET_QUOTE_SUCCESS) && !outlen_overflow) {
        t->hdr.out_len = cpu_to_le32(0);
    }

    if (address_space_write(
            &address_space_memory, t->gpa,
            MEMTXATTRS_UNSPECIFIED, &t->hdr, sizeof(t->hdr)) != MEMTX_OK) {
        error_report("TDX: failed to update GetQuote header.");
    }

    ret = tdx_td_notify(t->apic_id, t->event_notify_interrupt);
    if (ret < 0) {
        /* In this case, no better way to tell it to guest.  Log it. */
        error_report("TDX: injection %d failed, interrupt lost (%s).\n",
                     t->event_notify_interrupt, strerror(-ret));
    }

    /* Maintain the number of in-flight requests. */
    ms = MACHINE(qdev_get_machine());
    tdx = TDX_GUEST(ms->cgs);
    qemu_mutex_lock(&tdx->lock);
    QLIST_REMOVE(t, list);
    tdx->quote_generation_num--;
    qemu_mutex_unlock(&tdx->lock);

    if (t->ioc->fd > 0) {
        qemu_set_fd_handler(t->ioc->fd, NULL, NULL, NULL);
    }
    qio_channel_close(QIO_CHANNEL(t->ioc), NULL);
    object_unref(OBJECT(t->ioc));
    if (t->timer_armed)
        timer_del(&t->timer);
    g_free(t->out_data);
    g_free(t);
}

static void tdx_get_quote_read(void *opaque)
{
    struct tdx_get_quote_task *t = opaque;
    ssize_t size = 0;
    Error *err = NULL;
    bool outlen_overflow = false;

    while (true) {
        char *buf;
        size_t buf_size;

        if (t->out_len < t->buf_len) {
            buf = t->out_data + t->out_len;
            buf_size = t->buf_len - t->out_len;
        } else {
            /*
             * The received data is too large to fit in the shared GPA.
             * Discard the received data and try to know the data size.
             */
            buf = t->out_data;
            buf_size = t->buf_len;
        }

        size = qio_channel_read(QIO_CHANNEL(t->ioc), buf, buf_size, &err);
        if (!size) {
            break;
        }

        if (size < 0) {
            if (size == QIO_CHANNEL_ERR_BLOCK) {
                return;
            } else {
                break;
            }
        }
        t->out_len += size;
    }
    /*
     * If partial read successfully but return error at last, also treat it
     * as failure.
     */
    if (size < 0) {
        t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_QGS_UNAVAILABLE);
        goto error;
    }
    if (t->out_len > 0 && t->out_len > t->buf_len) {
        /*
         * There is no specific error code defined for this case(E2BIG) at the
         * moment.
         * TODO: Once an error code for this case is defined in GHCI spec ,
         * update the error code and the tdx_getquote_task_cleanup() argument.
         */
        t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_ERROR);
        t->hdr.out_len = cpu_to_le32(t->out_len);
        outlen_overflow = true;
        goto error;
    }

    if (address_space_write(
            &address_space_memory, t->gpa + sizeof(t->hdr),
            MEMTXATTRS_UNSPECIFIED, t->out_data, t->out_len) != MEMTX_OK) {
        goto error;
    }
    /*
     * Even if out_len == 0, it's a success.  It's up to the QGS-client contract
     * how to interpret the zero-sized message as return message.
     */
    t->hdr.out_len = cpu_to_le32(t->out_len);
    t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_SUCCESS);

error:
    tdx_getquote_task_cleanup(t, outlen_overflow);
}

#define TRANSACTION_TIMEOUT 30000

static void getquote_timer_expired(void *opaque)
{
    struct tdx_get_quote_task *t = opaque;

    tdx_getquote_task_cleanup(t, false);
}

static void tdx_transaction_start(struct tdx_get_quote_task *t)
{
    int64_t time;

    time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    /*
     * Timeout callback and fd callback both run in main loop thread,
     * thus no need to worry about race condition.
     */
    qemu_set_fd_handler(t->ioc->fd, tdx_get_quote_read, NULL, t);
    timer_init_ms(&t->timer, QEMU_CLOCK_VIRTUAL, getquote_timer_expired, t);
    timer_mod(&t->timer, time + TRANSACTION_TIMEOUT);
    t->timer_armed = true;
}

static void tdx_handle_get_quote_connected(QIOTask *task, gpointer opaque)
{
    struct tdx_get_quote_task *t = opaque;
    Error *err = NULL;
    char *in_data = NULL;
    int ret = 0;

    t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_ERROR);
    ret = qio_task_propagate_error(task, NULL);
    if (ret) {
        t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_QGS_UNAVAILABLE);
        goto out;
    }

    in_data = g_malloc(le32_to_cpu(t->hdr.in_len));
    if (!in_data) {
        ret = -1;
        goto out;
    }

    ret = address_space_read(&address_space_memory, t->gpa + sizeof(t->hdr),
                             MEMTXATTRS_UNSPECIFIED, in_data,
                             le32_to_cpu(t->hdr.in_len));
    if (ret) {
        g_free(in_data);
        goto out;
    }

    qio_channel_set_blocking(QIO_CHANNEL(t->ioc), false, NULL);

    ret = qio_channel_write_all(QIO_CHANNEL(t->ioc), in_data,
                              le32_to_cpu(t->hdr.in_len), &err);
    if (ret) {
        t->hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_QGS_UNAVAILABLE);
        g_free(in_data);
        goto out;
    }

out:
    if (ret) {
        tdx_getquote_task_cleanup(t, false);
    } else {
        tdx_transaction_start(t);
    }
    return;
}

static void __tdx_handle_get_quote(MachineState *ms, TdxGuest *tdx,
                                   hwaddr gpa, uint64_t buf_len,
                                   uint32_t apic_id,
                                   uint8_t event_notify_interrupt,
                                   struct kvm_tdx_vmcall *vmcall)
{
    struct tdx_get_quote_header hdr;
    QIOChannelSocket *ioc;
    struct tdx_get_quote_task *t;

    if (address_space_read(&address_space_memory, gpa, MEMTXATTRS_UNSPECIFIED,
                           &hdr, sizeof(hdr)) != MEMTX_OK) {
        return;
    }
    if (le64_to_cpu(hdr.structure_version) != TDX_GET_QUOTE_STRUCTURE_VERSION) {
        return;
    }
    /*
     * Paranoid: Guest should clear error_code and out_len to avoid information
     * leak.  Enforce it.  The initial value of them doesn't matter for qemu to
     * process the request.
     */
    if (le64_to_cpu(hdr.error_code) != TDX_VP_GET_QUOTE_SUCCESS &&
        le64_to_cpu(hdr.error_code) != TDX_VP_GET_QUOTE_IN_FLIGHT
        /* || le32_to_cpu(hdr.out_len) != 0 */) {
        return;
    }
    if (le32_to_cpu(hdr.out_len) > 0) {
        /* REVERTME: old shared page format. */
        warn_report("Guest attestation driver or R3AAL uses old GetQuote format."
                    "(out_len > 0) Please upgrade driver or R3AAL library.\n");
        if (le32_to_cpu(hdr.out_len) + sizeof(hdr) > buf_len) {
            return;
        }
        hdr.out_len = cpu_to_le32(0);
    }

    /* Only safe-guard check to avoid too large buffer size. */
    if (buf_len > TDX_GET_QUOTE_MAX_BUF_LEN ||
        le32_to_cpu(hdr.in_len) > TDX_GET_QUOTE_MAX_BUF_LEN ||
        le32_to_cpu(hdr.in_len) > buf_len) {
        return;
    }

    /* Mark the buffer in-flight. */
    hdr.error_code = cpu_to_le64(TDX_VP_GET_QUOTE_IN_FLIGHT);
    if (address_space_write(&address_space_memory, gpa, MEMTXATTRS_UNSPECIFIED,
                            &hdr, sizeof(hdr)) != MEMTX_OK) {
        return;
    }

    ms = MACHINE(qdev_get_machine());
    tdx = TDX_GUEST(ms->cgs);
    ioc = qio_channel_socket_new();

    t = g_malloc(sizeof(*t));
    t->gpa = gpa;
    t->buf_len = buf_len;
    t->out_data = g_malloc(t->buf_len);
    t->out_len = 0;
    t->hdr = hdr;
    t->ioc = ioc;
    t->timer_armed = false;

    qemu_mutex_lock(&tdx->lock);
    if (!tdx->quote_generation ||
        /* Prevent too many in-flight get-quote request. */
        tdx->quote_generation_num >= TDX_MAX_GET_QUOTE_REQUEST) {
        qemu_mutex_unlock(&tdx->lock);
        if (vmcall) {
            vmcall->status_code = TDG_VP_VMCALL_RETRY;
        }
        object_unref(OBJECT(ioc));
        g_free(t->out_data);
        g_free(t);
        return;
    }
    QLIST_INSERT_HEAD(&tdx->get_quote_task_list, t, list);
    if (apic_id == UNASSIGNED_APIC_ID) {
        t->apic_id = tdx->apic_id;
    } else {
        t->apic_id = apic_id;
    }
    tdx->quote_generation_num++;
    if (event_notify_interrupt == UNASSIGNED_INTERRUPT_VECTOR) {
        t->event_notify_interrupt = tdx->event_notify_interrupt;
    } else {
        t->event_notify_interrupt = event_notify_interrupt;
    }
    qio_channel_socket_connect_async(
        ioc, tdx->quote_generation, tdx_handle_get_quote_connected, t, NULL,
        NULL);
    qemu_mutex_unlock(&tdx->lock);

    if (vmcall) {
        vmcall->status_code = TDG_VP_VMCALL_SUCCESS;
    }
}

static void tdx_handle_setup_event_notify_interrupt(
    X86CPU *cpu, struct kvm_tdx_vmcall *vmcall)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    TdxGuest *tdx = TDX_GUEST(ms->cgs);
    int event_notify_interrupt = vmcall->in_r12;

    trace_tdx_handle_setup_event_notify_interrupt(event_notify_interrupt);
    if (32 <= event_notify_interrupt && event_notify_interrupt <= 255) {
        qemu_mutex_lock(&tdx->lock);
        tdx->event_notify_interrupt = event_notify_interrupt;
        tdx->apic_id = cpu->apic_id;
        qemu_mutex_unlock(&tdx->lock);
        vmcall->status_code = TDG_VP_VMCALL_SUCCESS;
    }
}

static int tdx_vmcall_service_do_cache_data_head(hwaddr addr,
                                                 TdxVmServiceDataHead *head)
{
    MemTxResult ret;

    ret = address_space_read(&address_space_memory,
                             addr, MEMTXATTRS_UNSPECIFIED,
                             head, le32_to_cpu(sizeof(*head)));
    if (ret != MEMTX_OK) {
        return -1;
    }

    return 0;
}


static int tdx_vmcall_service_sanity_check(X86CPU *cpu,
                                           struct kvm_tdx_vmcall *vmcall)
{
    TdxVmServiceDataHead head[2];
    hwaddr addrs[] = { vmcall->in_r12, vmcall->in_r13 };
    uint64_t vector;
    QemuUUID guid[2];

    for (int i = 0; i < 2; ++i) {

        if (!(addrs[i] & tdx_shared_bit(cpu))) {
            VMCALL_DEBUG("gpa in r12/r13 should have shared bit\n");
            return -1;
        }

        if (!QEMU_IS_ALIGNED((uint64_t)addrs[i], 4096)) {
            VMCALL_DEBUG("gpa in r12/r13 should 4K aligned\n");
            return -1;
        }

        /* Can't cache means the GPA may not in GPA space */
        if (tdx_vmcall_service_do_cache_data_head(addrs[i] & ~tdx_shared_bit(cpu),
                                                  &head[i])) {
           VMCALL_DEBUG("gpa in r12/r13 should be Guest physical memory\n");
           return -1;
        }

        /*Length should be at least cover the head*/
        if (head[i].length < sizeof(TdxVmServiceDataHead)) {
            VMCALL_DEBUG("length should >= Common VMCALL Service head size: %d\n",
                         sizeof(TdxVmServiceDataHead));
            return -1;
        }
    }

    guid[0] = head[0].guid;
    guid[1] = head[1].guid;
    /* the GUID in command/respond buffer should be same  */
    if (!qemu_uuid_is_equal(&guid[0], &guid[1])) {
        VMCALL_DEBUG("GUID in r12/r13 should be same\n");
        return -1;
    }

    /* check the notify vector for input parameter ONLY*/
    vector = vmcall->in_r14;
    if (vector && (vector < 32 || vector > 255)) {
        VMCALL_DEBUG("Vector of Service Call should in [32, 255]\n");
        return -1;
    }

    return 0;
}

static void tdx_vmcall_service_cache_data_head(TdxVmcallServiceItem *vsi)
{
    int ret;
    TdxVmcallSerivceDataCache *cache[] = {&vsi->command, &vsi->response};

    for (int i = 0; i < 2; ++i) {
        ret = tdx_vmcall_service_do_cache_data_head(cache[i]->addr,
                                                    &cache[i]->head);
        if (ret) {
            error_report("Unexpected failure of reading GPA space");
        }
    }
}

static int tdx_vmcall_service_cache_data(TdxVmcallServiceItem *vsi)
{
    MemTxResult ret;
    int64_t data_size;
    hwaddr addr;
    TdxVmcallSerivceDataCache *cache[] = {&vsi->command, &vsi->response};

    for (int i = 0; i < 2; ++i) {
        data_size = cache[i]->head.length - sizeof(cache[i]->head);

        if (!data_size) {
            cache[i]->data_len = 0;
            continue;
        }

        if (cache[i]->data_buf_len < data_size) {
            g_free(cache[i]->data_buf);
            cache[i]->data_buf = g_try_malloc0(data_size);
            if (cache[i]->data_buf) {
                cache[i]->data_buf_len = data_size;
            }
        }

        if (!cache[i]->data_buf) {
            return -1;
        }

        addr = cache[i]->addr + sizeof(cache[i]->head);
        ret = address_space_read(&address_space_memory,
                                 addr,
                                 MEMTXATTRS_UNSPECIFIED,
                                 cache[i]->data_buf,
                                 le32_to_cpu(data_size));
        if (ret != MEMTX_OK) {
            return -2;
        }

        cache[i]->data_len = data_size;
    }

    return 0;
}


static void tdx_vmcall_service_prepare_response(TdxVmcallSerivceDataCache *data_cache,
                                                bool prepare_rsp_head,
                                                bool prepare_rsp_data)
{
    hwaddr data_addr;
    MemTxResult ret;

    if (!data_cache)
        return;

    if (prepare_rsp_head) {
        data_cache->head.length = sizeof(TdxVmServiceDataHead) + data_cache->data_len;
        ret = address_space_write(&address_space_memory,
                                  data_cache->addr,
                                  MEMTXATTRS_UNSPECIFIED,
                                  &data_cache->head,
                                  le32_to_cpu(sizeof(data_cache->head)));
        if (ret != MEMTX_OK) {
            error_report("TDX: failed to update VM Service response header.");
            return;
        }
    }

    if (!prepare_rsp_data)
        return;

    data_addr = data_cache->addr + sizeof(TdxVmServiceDataHead);
    ret = address_space_write(&address_space_memory,
                              data_addr,
                              MEMTXATTRS_UNSPECIFIED,
                              data_cache->data_buf,
                              le32_to_cpu(data_cache->data_len));
    if (ret != MEMTX_OK) {
        error_report("TDX: failed to update VM Service response data area.");
    }
}

static int __tdx_vmcall_service_notify_guest(int apic_id, uint64_t vector)
{
    if (!vector)
        return 0;

    return tdx_td_notify(apic_id, vector);
}

static void __tdx_vmcall_service_complete_request(TdxVmcallSerivceDataCache *data_cache,
                                                  bool prepare_rsp_head, bool prepare_rsp_data,
                                                  int apic_id,
                                                  uint64_t notify_vector)
{
    tdx_vmcall_service_prepare_response(data_cache,
                                        prepare_rsp_head, prepare_rsp_data);

    __tdx_vmcall_service_notify_guest(apic_id, notify_vector);
}

TdxVmcallServiceType* tdx_vmcall_service_find_handler(QemuUUID *guid,
                                                      TdxVmcallService *vmc)
{
    for (int  i = 0; i < vmc->dispatch_table_count; ++i) {
        if (!qemu_uuid_is_equal(guid, &vmc->dispatch_table[i].from)) {
            continue;
        }

        if (!vmc->dispatch_table[i].to) {
            continue;
        }

        return &vmc->dispatch_table[i];
    }

    return NULL;
}

static void tdx_vmcall_service_dispatch_service_item(TdxVmcallServiceType *handler,
                                                     TdxVmcallServiceItem *vsi)
{
    handler->to(vsi, handler->opaque);
}

void tdx_vmcall_service_item_ref(TdxVmcallServiceItem *item)
{
    uint32_t ref;

    g_assert(item);
    ref = qatomic_fetch_inc(&item->ref_count);
    g_assert(ref < INT_MAX);
}

void tdx_vmcall_service_item_unref(TdxVmcallServiceItem *item)
{
    g_assert(item);
    g_assert(item->ref_count > 0);
    if (qatomic_fetch_dec(&item->ref_count) == 1) {
            g_free(item->command.data_buf);
            g_free(item->response.data_buf);
            qemu_sem_destroy(&item->wait);

            g_free(item);
    }
}

static TdxVmcallServiceItem*
tdx_vmcall_service_create_service_item(int vsi_size,
                                       struct kvm_tdx_vmcall *vmcall)
{
     TdxVmcallServiceItem* new;

     new = g_try_malloc0(vsi_size);
     if (!new)
         return new;

     tdx_vmcall_service_item_ref(new);

     return new;
}

static int
tdx_vmcall_service_init_service_item(X86CPU *cpu, struct kvm_tdx_vmcall *vmcall,
                                     TdxVmcallServiceItem *vsi)
{
     uint64_t gpa_mask = ~tdx_shared_bit(cpu);

     qemu_sem_init(&vsi->wait, 0);

     vsi->command.addr = vmcall->in_r12 & gpa_mask;
     vsi->response.addr = vmcall->in_r13 & gpa_mask;
     vsi->notify_vector = vmcall->in_r14;
     vsi->timeout = vmcall->in_r15;

     tdx_vmcall_service_cache_data_head(vsi);
     if (tdx_vmcall_service_cache_data(vsi)) {
         return -1;
     }

     return 0;
}

static bool tdx_vmcall_service_is_block(TdxVmcallServiceItem *vsi)
{
    return !vsi->notify_vector;
}

static int tdx_vmcall_service_wait(TdxVmcallServiceItem *vsi)
{
    return qemu_sem_timedwait(&vsi->wait, 100);
}

static void tdx_vmcall_service_wake(TdxVmcallServiceItem *vsi)
{
    qemu_sem_post(&vsi->wait);
}

static void tdx_handle_vmcall_service(X86CPU *cpu, struct kvm_tdx_vmcall *vmcall)
{
    TdxGuest *tdx;
    MachineState *ms;
    TdxVmcallServiceItem *vsi;
    TdxVmcallServiceType *handler;
    TdxVmcallSerivceDataCache command;
    TdxVmcallSerivceDataCache response;
    uint64_t gpa_mask = ~tdx_shared_bit(cpu);
    QemuUUID guid;
    struct {
        hwaddr addr;
        TdxVmcallSerivceDataCache *cache;
    } helper[] = {
        {vmcall->in_r12 & gpa_mask, &command},
        {vmcall->in_r13 & gpa_mask, &response},
    };

    ms = MACHINE(qdev_get_machine());
    tdx = TDX_GUEST(ms->cgs);

    if (tdx_vmcall_service_sanity_check(cpu, vmcall)) {
        vmcall->status_code = TDG_VP_VMCALL_INVALID_OPERAND;
        return;
    }

    vmcall->status_code = TDG_VP_VMCALL_SUCCESS;

    for (int i = 0; i < sizeof(helper)/sizeof(helper[0]); ++i) {
        memset(helper[i].cache, 0, sizeof(*helper[i].cache));
        helper[i].cache->addr = helper[i].addr;
        tdx_vmcall_service_do_cache_data_head(helper[i].addr,
                                              &helper[i].cache->head);

    }

    guid = command.head.guid;
    handler = tdx_vmcall_service_find_handler(&guid,
                                              &tdx->vmcall_service);
    if (!handler) {
        response.head.u.status = TDG_VP_VMCALL_SERVICE_NOT_SUPPORT;
        VMCALL_DEBUG("Service not supported, please check GUID value\n");
        goto fail;
    }

    vsi = tdx_vmcall_service_create_service_item(handler->vsi_size, vmcall);
    if (!vsi) {
        response.head.u.status = TDG_VP_VMCALL_SERVICE_OUT_OF_RESOURCE;
        VMCALL_DEBUG("Failed to create vsi, out of memory or incorrect vis_size:%d\n",
                     handler->vsi_size);
        goto fail;
    }
    vsi->apic_id = tdx->apic_id;

    if (tdx_vmcall_service_init_service_item(cpu, vmcall, vsi)) {
        response.head.u.status = TDG_VP_VMCALL_SERVICE_OUT_OF_RESOURCE;
        VMCALL_DEBUG("Failed to init vsi, out of memory or incorrect total length:%d\n",
            vsi->command.head.length);
        goto fail_free;
    }

    tdx_vmcall_service_dispatch_service_item(handler, vsi);

    if (tdx_vmcall_service_is_block(vsi)) {
        /*Handle reset/shutdown, return BUSY for this */
        while (1) {
            if (runstate_is_running()) {
                if (!tdx_vmcall_service_wait(vsi)) {
                    break;
                }
                continue;
            }

            tdx_vmcall_service_set_response_state(vsi,
                                                  TDG_VP_VMCALL_SERVICE_BUSY);
            tdx_vmcall_service_complete_request(vsi);
            break;
        }
    }

    tdx_vmcall_service_item_unref(vsi);
    return;

 fail_free:
    tdx_vmcall_service_item_unref(vsi);
 fail:
    __tdx_vmcall_service_complete_request(&response,
                                          true, false,
                                          tdx->apic_id,
                                          vmcall->in_r14);
}

static void tdx_handle_vmcall(X86CPU *cpu, struct kvm_tdx_vmcall *vmcall)
{
    vmcall->status_code = TDG_VP_VMCALL_INVALID_OPERAND;

    /* For now handle only TDG.VP.VMCALL. */
    if (vmcall->type != 0) {
        warn_report("unknown tdg.vp.vmcall type 0x%llx subfunction 0x%llx",
                    vmcall->type, vmcall->subfunction);
        return;
    }

    switch (vmcall->subfunction) {
    case TDG_VP_VMCALL_MAP_GPA:
        tdx_handle_map_gpa(cpu, vmcall);
        break;
    case TDG_VP_VMCALL_GET_QUOTE:
        tdx_handle_get_quote(cpu, vmcall);
        break;
    case TDG_VP_VMCALL_SETUP_EVENT_NOTIFY_INTERRUPT:
        tdx_handle_setup_event_notify_interrupt(cpu, vmcall);
        break;
    case TDG_VP_VMCALL_SERVICE:
        tdx_handle_vmcall_service(cpu, vmcall);
        break;
    default:
        warn_report("unknown tdg.vp.vmcall type 0x%llx subfunction 0x%llx",
                    vmcall->type, vmcall->subfunction);
        break;
    }
}

static void tdx_vmcall_service_timeout_handler(void *opaque)
{
    TdxVmcallServiceItem *vsi = opaque;

    timer_del(&vsi->timer);
    if (vsi->timer_cb)
        vsi->timer_cb(vsi, vsi->timer_opaque);

    tdx_vmcall_service_set_response_state(vsi, TDG_VP_VMCALL_SERVICE_TIME_OUT);
    tdx_vmcall_service_complete_request(vsi);
}

void tdx_vmcall_service_set_response_state(TdxVmcallServiceItem *vsi,
                                           int state)
{
    vsi->response.head.u.status = state;
}

void* tdx_vmcall_service_rsp_buf(TdxVmcallServiceItem *vsi)
{
    return vsi->response.data_buf;
}

int tdx_vmcall_service_rsp_size(TdxVmcallServiceItem *vsi)
{
    return vsi->response.data_len;
}

void tdx_vmcall_service_set_rsp_size(TdxVmcallServiceItem *vsi,
                                     int size)
{
    vsi->response.data_len = size;
}

void* tdx_vmcall_service_cmd_buf(TdxVmcallServiceItem *vsi)
{
    return vsi->command.data_buf;
}

int tdx_vmcall_service_cmd_size(TdxVmcallServiceItem *vsi)
{
    return vsi->command.data_len;
}

void tdx_vmcall_service_set_timeout_handler(TdxVmcallServiceItem *vsi,
                                            TdxVmcallServiceTimerCB *cb,
                                            void *opaque)
{
    if (!vsi->timeout)
        return;

    vsi->timer_cb = cb;
    vsi->timer_opaque = opaque;

    if (cb) {
        if (!vsi->timer_enable) {
            tdx_vmcall_service_item_ref(vsi);
            vsi->timer_enable = true;
        } else {
            timer_del(&vsi->timer);
        }
        timer_init_ms(&vsi->timer, QEMU_CLOCK_VIRTUAL,
                      tdx_vmcall_service_timeout_handler, vsi);
        timer_mod(&vsi->timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + vsi->timeout);
    } else {
            if (vsi->timer_enable) {
                vsi->timer_enable = false;
                timer_del(&vsi->timer);
                tdx_vmcall_service_item_unref(vsi);
            }
    }
}

void tdx_vmcall_service_complete_request(TdxVmcallServiceItem *vsi)
{
    TdxVmcallSerivceDataCache *out = &vsi->response;
    bool prepare_data;

    prepare_data = (out->head.u.status != TDG_VP_VMCALL_SERVICE_RSP_BUF_TOO_SMALL);

    __tdx_vmcall_service_complete_request(out, true, prepare_data,
                                          vsi->apic_id, vsi->notify_vector);

    if (tdx_vmcall_service_is_block(vsi)) {
        tdx_vmcall_service_wake(vsi);
    }

    tdx_vmcall_service_set_timeout_handler(vsi, NULL, NULL);
}

void tdx_vmcall_service_register_type(TdxGuest *tdx,
                                      TdxVmcallServiceType* type)
{
    TdxVmcallService *vmc;

    if (!tdx || !type)
        return;

    vmc = &tdx->vmcall_service;
    vmc->dispatch_table = g_realloc_n(vmc->dispatch_table,
                                      vmc->dispatch_table_count + 1,
                                      sizeof(*vmc->dispatch_table));
    vmc->dispatch_table[vmc->dispatch_table_count] = *type;
    ++vmc->dispatch_table_count;
}

void tdx_handle_exit(X86CPU *cpu, struct kvm_tdx_exit *tdx_exit)
{
    switch (tdx_exit->type) {
    case KVM_EXIT_TDX_VMCALL:
        tdx_handle_vmcall(cpu, &tdx_exit->u.vmcall);
        break;
    default:
        warn_report("unknown tdx exit type 0x%x", tdx_exit->type);
        break;
    }
}

bool tdx_debug_enabled(void)
{
    if (!is_tdx_vm())
        return false;

    return tdx_guest->attributes & TDX_TD_ATTRIBUTES_DEBUG;
}

static hwaddr tdx_gpa_stolen_mask(void)
{
    X86CPU *x86_cpu = X86_CPU(first_cpu);

    if (!x86_cpu || !x86_cpu->phys_bits)
        return 0ULL;

    if (x86_cpu->phys_bits > 48)
            return 1ULL << 51;
        else
            return 1ULL << 47;
}

hwaddr tdx_remove_stolen_bit(hwaddr gpa)
{
    if (!is_tdx_vm())
        return gpa;
    return gpa & ~tdx_gpa_stolen_mask();
}
