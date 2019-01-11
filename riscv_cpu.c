//******************************************************************************
// Copyright (C) 2018,2019, Esperanto Technologies Inc.
// The copyright to the computer program(s) herein is the
// property of Esperanto Technologies, Inc. All Rights Reserved.
// The program(s) may be used and/or copied only with
// the written permission of Esperanto Technologies and
// in accordance with the terms and conditions stipulated in the
// agreement/contract under which the program(s) have been supplied.
//------------------------------------------------------------------------------

/*
 * RISCV CPU emulator
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
 * Copyright (c) 2018,2019 Esperanto Technologies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>

#include "cutils.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "validation_events.h"

#ifndef FLEN
#define FLEN 64
#endif /* !FLEN */

#define DUMP_INVALID_MEM_ACCESS
#define DUMP_MMU_EXCEPTIONS
//#define DUMP_INTERRUPTS
//#define DUMP_INVALID_CSR
#define DUMP_ILLEGAL_INSTRUCTION
//#define DUMP_EXCEPTIONS
//#define DUMP_CSR
#define CONFIG_LOGFILE
#define CONFIG_SW_MANAGED_A_AND_D 1
#define CONFIG_ALLOW_MISALIGNED_ACCESS 0

#if FLEN > 0
#include "softfp.h"
#endif

#define __exception __attribute__((warn_unused_result))

typedef uint64_t target_ulong;
typedef int64_t target_long;
#define PR_target_ulong "016" PRIx64

/* FLEN is the floating point register width */
#if FLEN > 0
#if FLEN == 32
typedef uint32_t fp_uint;
#define F32_HIGH 0
#elif FLEN == 64
typedef uint64_t fp_uint;
#define F32_HIGH ((fp_uint)-1 << 32)
#define F64_HIGH 0
#elif FLEN == 128
typedef uint128_t fp_uint;
#define F32_HIGH ((fp_uint)-1 << 32)
#define F64_HIGH ((fp_uint)-1 << 64)
#else
#error unsupported FLEN
#endif
#endif

/* MLEN is the maximum memory access width */
#if 64 <= 32 && FLEN <= 32
#define MLEN 32
#elif 64 <= 64 && FLEN <= 64
#define MLEN 64
#else
#define MLEN 128
#endif

#if MLEN == 32
typedef uint32_t mem_uint_t;
#elif MLEN == 64
typedef uint64_t mem_uint_t;
#elif MLEN == 128
typedef uint128_t mem_uint_t;
#else
#unsupported MLEN
#endif

#define TLB_SIZE 256

#define CAUSE_MISALIGNED_FETCH    0x0
#define CAUSE_FAULT_FETCH         0x1
#define CAUSE_ILLEGAL_INSTRUCTION 0x2
#define CAUSE_BREAKPOINT          0x3
#define CAUSE_MISALIGNED_LOAD     0x4
#define CAUSE_FAULT_LOAD          0x5
#define CAUSE_MISALIGNED_STORE    0x6
#define CAUSE_FAULT_STORE         0x7
#define CAUSE_USER_ECALL          0x8
#define CAUSE_SUPERVISOR_ECALL    0x9
#define CAUSE_HYPERVISOR_ECALL    0xa
#define CAUSE_MACHINE_ECALL       0xb
#define CAUSE_FETCH_PAGE_FAULT    0xc
#define CAUSE_LOAD_PAGE_FAULT     0xd
#define CAUSE_STORE_PAGE_FAULT    0xf

#define CAUSE_MASK 0x1f // not including the MSB for interrupt

/* Note: converted to correct bit position at runtime */
#define CAUSE_INTERRUPT  ((uint32_t)1 << 31)

#define PRV_U 0
#define PRV_S 1
#define PRV_H 2
#define PRV_M 3

/* misa CSR */
#define MCPUID_SUPER   (1 << ('S' - 'A'))
#define MCPUID_USER    (1 << ('U' - 'A'))
#define MCPUID_I       (1 << ('I' - 'A'))
#define MCPUID_M       (1 << ('M' - 'A'))
#define MCPUID_A       (1 << ('A' - 'A'))
#define MCPUID_F       (1 << ('F' - 'A'))
#define MCPUID_D       (1 << ('D' - 'A'))
#define MCPUID_Q       (1 << ('Q' - 'A'))
#define MCPUID_C       (1 << ('C' - 'A'))

/* mstatus CSR */

#define MSTATUS_SPIE_SHIFT 5
#define MSTATUS_MPIE_SHIFT 7
#define MSTATUS_SPP_SHIFT 8
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_FS_SHIFT 13
#define MSTATUS_UXL_SHIFT 32
#define MSTATUS_SXL_SHIFT 34

#define MSTATUS_UIE (1 << 0)
#define MSTATUS_SIE (1 << 1)
#define MSTATUS_HIE (1 << 2)
#define MSTATUS_MIE (1 << 3)
#define MSTATUS_UPIE (1 << 4)
#define MSTATUS_SPIE (1 << MSTATUS_SPIE_SHIFT)
#define MSTATUS_HPIE (1 << 6)
#define MSTATUS_MPIE (1 << MSTATUS_MPIE_SHIFT)
#define MSTATUS_SPP (1 << MSTATUS_SPP_SHIFT)
#define MSTATUS_HPP (3 << 9)
#define MSTATUS_MPP (3 << MSTATUS_MPP_SHIFT)
#define MSTATUS_FS (3 << MSTATUS_FS_SHIFT)
#define MSTATUS_XS (3 << 15)
#define MSTATUS_MPRV (1 << 17)
#define MSTATUS_SUM (1 << 18)
#define MSTATUS_MXR (1 << 19)
#define MSTATUS_TVM (1 << 20)
#define MSTATUS_TW (1 << 21)
#define MSTATUS_TSR (1 << 22)
#define MSTATUS_UXL_MASK ((uint64_t)3 << MSTATUS_UXL_SHIFT)
#define MSTATUS_SXL_MASK ((uint64_t)3 << MSTATUS_SXL_SHIFT)

#define PG_SHIFT 12
#define PG_MASK ((1 << PG_SHIFT) - 1)

#define ASID_BITS 0

#define SATP_MASK ((15ULL << 60) | (((1ULL << ASID_BITS) - 1) << 44) | ((1ULL << 44) - 1))

#ifndef MAX_TRIGGERS
#define MAX_TRIGGERS 1 // As of right now, Maxion implements one trigger register
#endif

// A few of Debug Trigger Match Control bits (there are many more)
#define MCONTROL_M         (1 << 6)
#define MCONTROL_S         (1 << 4)
#define MCONTROL_U         (1 << 3)
#define MCONTROL_EXECUTE   (1 << 2)
#define MCONTROL_STORE     (1 << 1)
#define MCONTROL_LOAD      (1 << 0)

typedef struct {
    target_ulong vaddr;
    uintptr_t mem_addend;
} TLBEntry;

struct RISCVCPUState {
    target_ulong pc;
    target_ulong reg[32];
    /* Co-simulation sometimes need to see the value of a register
     * prior to the just excuted instruction. */
    target_ulong reg_prior[32];
    /* reg_ts[x] is the timestamp (in executed instructions) of the most
     * recent definition of the register. */
    uint64_t reg_ts[32];
    int most_recently_written_reg;

#if FLEN > 0
    fp_uint fp_reg[32];
    uint64_t fp_reg_ts[32];
    int most_recently_written_fp_reg;
    uint32_t fflags;
    uint8_t frm;
#endif

    uint8_t cur_xlen;  /* current XLEN value, <= 64 */
    uint8_t priv; /* see PRV_x */
    uint8_t fs; /* MSTATUS_FS value */
    uint8_t mxl; /* MXL field in MISA register */

    uint64_t insn_counter; // Simulator internal
    uint64_t minstret; // RISCV CSR (updated when insn_counter increases)
    uint64_t mcycle;   // RISCV CSR (updated when insn_counter increases)
    BOOL     stop_the_counter; // Set in debug mode only (cleared after ending Debug)

    BOOL power_down_flag; /* True when the core is idle awaiting
                           * interrupts, does NOT mean terminate
                           * simulation */
    BOOL terminate_simulation;
    int pending_exception; /* used during MMU exception handling */
    target_ulong pending_tval;

    /* CSRs */
    target_ulong mstatus;
    target_ulong mtvec;
    target_ulong mscratch;
    target_ulong mepc;
    target_ulong mcause;
    target_ulong mtval;
    target_ulong mvendorid; /* ro */
    target_ulong marchid; /* ro */
    target_ulong mimpid; /* ro */
    target_ulong mhartid; /* ro */
    uint32_t misa;
    uint32_t mie;
    uint32_t mip;
    uint32_t medeleg;
    uint32_t mideleg;
    uint32_t mcounteren;
    uint32_t tselect;
    target_ulong tdata1[MAX_TRIGGERS];
    target_ulong tdata2[MAX_TRIGGERS];
    target_ulong tdata3[MAX_TRIGGERS];

    target_ulong mhpmevent[32];

    target_ulong stvec;
    target_ulong sscratch;
    target_ulong sepc;
    target_ulong scause;
    target_ulong stval;
    uint64_t satp; /* currently 64 bit physical addresses max */
    uint32_t scounteren;

    target_ulong dcsr; // Debug CSR 0x7b0 (debug spec only)
    target_ulong dpc;  // Debug DPC 0x7b1 (debug spec only)
    target_ulong dscratch;  // Debug dscratch 0x7b2 (debug spec only)

    target_ulong load_res; /* for atomic LR/SC */
    uint32_t  store_repair_val32; /* saving previous value of memory so it can be repaired */
    uint64_t  store_repair_val64;
    uint128_t store_repair_val128;
    target_ulong store_repair_addr; /* saving which address to repair */
    uint64_t last_addr; /* saving previous value of address so it can be repaired */

    PhysMemoryMap *mem_map;

    TLBEntry tlb_read[TLB_SIZE];
    TLBEntry tlb_write[TLB_SIZE];
    TLBEntry tlb_code[TLB_SIZE];

    // User specified, command line argument terminating event
    const char *terminating_event;

    /* Control Flow Info */
    RISCVCTFInfo info;
    target_ulong next_addr; /* the CFI target address-- only valid for CFIs. */
};

// NOTE: Use GET_INSN_COUNTER not mcycle because this is just to track advancement of simulation
#define write_reg(x, val) ({s->most_recently_written_reg = (x); \
                           s->reg_ts[x] = GET_INSN_COUNTER();   \
                           s->reg_prior[x] = s->reg[x]; \
                           s->reg[x] = (val);})
#define read_reg(x)       (s->reg[x])

#define write_fp_reg(x, val) ({s->most_recently_written_fp_reg = (x); \
                               s->fp_reg_ts[x] = GET_INSN_COUNTER(); \
                               s->fp_reg[x] = (val);})
#define read_fp_reg(x)       (s->fp_reg[x])

static no_inline int target_read_slow(RISCVCPUState *s, mem_uint_t *pval,
                                      target_ulong addr, int size_log2);
static no_inline int target_write_slow(RISCVCPUState *s, target_ulong addr,
                                       mem_uint_t val, int size_log2);

#ifdef USE_GLOBAL_STATE
static RISCVCPUState riscv_cpu_global_state;
#endif

#ifdef CONFIG_LOGFILE
static FILE *log_file;

void log_vprintf(const char *fmt, va_list ap)
{
    if (!log_file)
        log_file = fopen("/tmp/riscemu.log", "wb");
    vfprintf(log_file, fmt, ap);
}
#else
void log_vprintf(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
}
#endif

void __attribute__((format(printf, 1, 2))) log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

static void fprint_target_ulong(FILE *f, target_ulong a)
{
    fprintf(f, "%" PR_target_ulong, a);
}

static void print_target_ulong(target_ulong a)
{
    fprint_target_ulong(stderr, a);
}

static char *reg_name[32] = {
"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
"s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
"a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
"s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

static target_ulong get_mstatus(RISCVCPUState *s, target_ulong mask);
static void dump_regs(RISCVCPUState *s)
{
    int i, cols;
    const char priv_str[4] = "USHM";
    cols = 256 / 64;
    fprintf(stderr, "pc =");
    print_target_ulong(s->pc);
    fprintf(stderr, " ");
    for (i = 1; i < 32; i++) {
        fprintf(stderr, "%-3s=", reg_name[i]);
        print_target_ulong(s->reg[i]);
        if ((i & (cols - 1)) == (cols - 1))
            fprintf(stderr, "\n");
        else
            fprintf(stderr, " ");
    }
    fprintf(stderr, "priv=%c", priv_str[s->priv]);
    fprintf(stderr, " mstatus=");
    print_target_ulong(get_mstatus(s, (target_ulong)-1));
    fprintf(stderr, " insn_counter=%" PRId64, s->insn_counter);
    fprintf(stderr, " minstret=%" PRId64, s->minstret);
    fprintf(stderr, " mcycle=%" PRId64, s->mcycle);
    fprintf(stderr, "\n");
#if 1
    fprintf(stderr, " mideleg=");
    print_target_ulong(s->mideleg);
    fprintf(stderr, " mie=");
    print_target_ulong(s->mie);
    fprintf(stderr, " mip=");
    print_target_ulong(s->mip);
    fprintf(stderr, "\n");
#endif
}

uint64_t checker_last_addr = 0;
uint64_t checker_last_data = 0;
int      checker_last_size = 0;

#define TRACK_MEM(vaddr,size,val)  \
do {  \
    checker_last_addr = vaddr; \
    checker_last_size = size; \
    checker_last_data = val; \
} while(0)

/* addr must be aligned. Only RAM accesses are supported */
#define PHYS_MEM_READ_WRITE(size, uint_type) \
static __maybe_unused inline void phys_write_u ## size(RISCVCPUState *s, target_ulong addr,\
                                        uint_type val)                   \
{\
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);\
    if (!pr || !pr->is_ram)\
        return;\
    TRACK_MEM(addr,size,val);\
    *(uint_type *)(pr->phys_mem + \
                 (uintptr_t)(addr - pr->addr)) = val;\
}\
\
static __maybe_unused inline uint_type phys_read_u ## size(RISCVCPUState *s, target_ulong addr) \
{\
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);\
    if (!pr || !pr->is_ram)\
        return 0;\
    uint_type pval =  *(uint_type *)(pr->phys_mem + \
                          (uintptr_t)(addr - pr->addr));     \
    TRACK_MEM(addr,size,pval);\
    return pval;\
}

PHYS_MEM_READ_WRITE(8, uint8_t)
PHYS_MEM_READ_WRITE(32, uint32_t)
PHYS_MEM_READ_WRITE(64, uint64_t)

/* return 0 if OK, != 0 if exception */
#define TARGET_READ_WRITE(size, uint_type, size_log2)                   \
static inline __exception int target_read_u ## size(RISCVCPUState *s, uint_type *pval, target_ulong addr)                              \
{\
    uint32_t tlb_idx;\
    if (!CONFIG_ALLOW_MISALIGNED_ACCESS && (addr & (size/8 - 1)) != 0) { \
        s->pending_tval = addr;                                         \
        s->pending_exception = CAUSE_MISALIGNED_LOAD;                   \
        return -1;                                                      \
    }                                                                   \
    tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);\
    if (likely(s->tlb_read[tlb_idx].vaddr == (addr & ~(PG_MASK & ~((size / 8) - 1))))) { \
        *pval = *(uint_type *)(s->tlb_read[tlb_idx].mem_addend + (uintptr_t)addr);\
        TRACK_MEM(addr,size,0);\
    } else {\
        mem_uint_t val;\
        int ret;\
        ret = target_read_slow(s, &val, addr, size_log2);\
        if (ret)\
            return ret;\
        *pval = val;\
    }\
    return 0;\
}\
\
static inline __exception int target_write_u ## size(RISCVCPUState *s, target_ulong addr,\
                                          uint_type val)                \
{\
    uint32_t tlb_idx;\
    if (!CONFIG_ALLOW_MISALIGNED_ACCESS && (addr & (size/8 - 1)) != 0) { \
        s->pending_tval = addr;                                         \
        s->pending_exception = CAUSE_MISALIGNED_STORE;                  \
        return -1;                                                      \
    }                                                                   \
    tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);\
    if (likely(s->tlb_write[tlb_idx].vaddr == (addr & ~(PG_MASK & ~((size / 8) - 1))))) { \
        *(uint_type *)(s->tlb_write[tlb_idx].mem_addend + (uintptr_t)addr) = val;\
        TRACK_MEM(addr,size,val);\
        return 0;\
    } else {\
        int r = target_write_slow(s, addr, val, size_log2);\
        if (r) return r; \
        return 0; \
    }\
}

TARGET_READ_WRITE(8, uint8_t, 0)
TARGET_READ_WRITE(16, uint16_t, 1)
TARGET_READ_WRITE(32, uint32_t, 2)
#if MLEN >= 64
TARGET_READ_WRITE(64, uint64_t, 3)
#endif
#if MLEN >= 128
TARGET_READ_WRITE(128, uint128_t, 4)
#endif

#define PTE_V_MASK (1 << 0)
#define PTE_U_MASK (1 << 4)
#define PTE_A_MASK (1 << 6)
#define PTE_D_MASK (1 << 7)

#define ACCESS_READ  0
#define ACCESS_WRITE 1
#define ACCESS_CODE  2

/* access = 0: read, 1 = write, 2 = code. Set the exception_pending
   field if necessary. return 0 if OK, -1 if translation error, -2 if
   the physical address is illegal. */
static int get_phys_addr(RISCVCPUState *s,
                         target_ulong *ppaddr, target_ulong vaddr,
                         int access)
{
    int mode, levels, pte_bits, pte_idx, pte_mask, pte_size_log2, xwr, priv;
    int need_write, vaddr_shift, i, pte_addr_bits;
    target_ulong pte_addr, pte, vaddr_mask, paddr;

    if ((s->mstatus & MSTATUS_MPRV) && access != ACCESS_CODE) {
        /* use previous privilege */
        priv = (s->mstatus >> MSTATUS_MPP_SHIFT) & 3;
    } else {
        priv = s->priv;
    }

    if (priv == PRV_M) {
        /* rv64mi-p-access expects illegal physical addresses to fail.
           We arbitrarily sets PA to 56. */
        if (s->cur_xlen > 32 && (uint64_t)vaddr >> 56 != 0)
            return -2;
        if (s->cur_xlen < 64) {
            /* truncate virtual address */
            *ppaddr = vaddr & (((target_ulong)1 << s->cur_xlen) - 1);
        } else {
            *ppaddr = vaddr;
        }
        return 0;
    }
    mode = (s->satp >> 60) & 0xf;
    if (mode == 0) {
        /* bare: no translation */
        *ppaddr = vaddr;
        return 0;
    } else {
        /* sv39/sv48 */
        levels = mode - 8 + 3;
        pte_size_log2 = 3;
        vaddr_shift = 64 - (PG_SHIFT + levels * 9);
        if ((((target_long)vaddr << vaddr_shift) >> vaddr_shift) != vaddr)
            return -1;
        pte_addr_bits = 44;
    }
    pte_addr = (s->satp & (((target_ulong)1 << pte_addr_bits) - 1)) << PG_SHIFT;
    pte_bits = 12 - pte_size_log2;
    pte_mask = (1 << pte_bits) - 1;
    for (i = 0; i < levels; i++) {
        vaddr_shift = PG_SHIFT + pte_bits * (levels - 1 - i);
        pte_idx = (vaddr >> vaddr_shift) & pte_mask;
        pte_addr += pte_idx << pte_size_log2;
        if (pte_size_log2 == 2)
            pte = phys_read_u32(s, pte_addr);
        else
            pte = phys_read_u64(s, pte_addr);
        if (!(pte & PTE_V_MASK))
            return -1; /* invalid PTE */
        paddr = (pte >> 10) << PG_SHIFT;
        xwr = (pte >> 1) & 7;
        if (xwr != 0) {
            if (xwr == 2 || xwr == 6)
                return -1;
            /* priviledge check */
            if (priv == PRV_S) {
                if ((pte & PTE_U_MASK) && !(s->mstatus & MSTATUS_SUM))
                    return -1;
            } else {
                if (!(pte & PTE_U_MASK))
                    return -1;
            }
            /* protection check */
            /* MXR allows read access to execute-only pages */
            if (s->mstatus & MSTATUS_MXR)
                xwr |= (xwr >> 2);

            if (((xwr >> access) & 1) == 0)
                return -1;

            /* 6. Check for misaligned superpages */
            unsigned ppn = pte >> 10;
            int j = levels-1 - i;
            if (((1 << j) - 1) & ppn)
                return -1;

            /*
              RISC-V Priv. Spec 1.11 (draft) Section 4.3.1 offers two
              ways to handle the A and D TLB flags.  Spike uses the
              software managed approach whereas RISCVEMU used to manage
              them (causing far fewer exceptios).
            */
            if (CONFIG_SW_MANAGED_A_AND_D) {
                if (!(pte & PTE_A_MASK))
                    return -1; // Must have A on access

                if (access == ACCESS_WRITE && !(pte & PTE_D_MASK))
                    return -1; // Must have D on write
            } else {
                need_write = !(pte & PTE_A_MASK) ||
                    (!(pte & PTE_D_MASK) && access == ACCESS_WRITE);
                pte |= PTE_A_MASK;
                if (access == ACCESS_WRITE)
                    pte |= PTE_D_MASK;
                if (need_write) {
                    if (pte_size_log2 == 2)
                        phys_write_u32(s, pte_addr, pte);
                    else
                        phys_write_u64(s, pte_addr, pte);
                }
            }

            vaddr_mask = ((target_ulong)1 << vaddr_shift) - 1;
            *ppaddr = paddr & ~vaddr_mask | vaddr & vaddr_mask;
            return 0;
        } else {
            pte_addr = paddr;
        }
    }
    return -1;
}

/* return 0 if OK, != 0 if exception */
static no_inline int target_read_slow(RISCVCPUState *s, mem_uint_t *pval,
                                      target_ulong addr, int size_log2)
{
    int size, tlb_idx, err, al;
    target_ulong paddr, offset;
    uint8_t *ptr;
    PhysMemoryRange *pr;
    mem_uint_t ret;

    /* first handle unaligned accesses */
    size = 1 << size_log2;
    al = addr & (size - 1);
    if (!CONFIG_ALLOW_MISALIGNED_ACCESS && al != 0) {
        s->pending_tval = addr;
        s->pending_exception = CAUSE_MISALIGNED_LOAD;
        return -1;
    } else if (al != 0) {
        switch(size_log2) {
        case 1:
            {
                uint8_t v0, v1;
                err = target_read_u8(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u8(s, &v1, addr + 1);
                if (err)
                    return err;
                ret = v0 | (v1 << 8);
            }
            break;
        case 2:
            {
                uint32_t v0, v1;
                addr -= al;
                err = target_read_u32(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u32(s, &v1, addr + 4);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (32 - al * 8));
            }
            break;
#if MLEN >= 64
        case 3:
            {
                uint64_t v0, v1;
                addr -= al;
                err = target_read_u64(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u64(s, &v1, addr + 8);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (64 - al * 8));
            }
            break;
#endif
#if MLEN >= 128
        case 4:
            {
                uint128_t v0, v1;
                addr -= al;
                err = target_read_u128(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u128(s, &v1, addr + 16);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (128 - al * 8));
            }
            break;
#endif
        default:
            abort();
        }
    } else {
        int err = get_phys_addr(s, &paddr, addr, ACCESS_READ);
        if (err) {
            s->pending_tval = addr;
            s->pending_exception = err == -1
                ? CAUSE_LOAD_PAGE_FAULT : CAUSE_FAULT_LOAD;
            return -1;
        }
        pr = get_phys_mem_range(s->mem_map, paddr);
        if (!pr) {
#ifdef DUMP_INVALID_MEM_ACCESS
            fprintf(stderr, "target_read_slow: invalid physical address 0x");
            print_target_ulong(paddr);
            fprintf(stderr, "\n");
#endif
            return 0;
        } else if (pr->is_ram) {
            tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);
            ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
            s->tlb_read[tlb_idx].vaddr = addr & ~PG_MASK;
            s->tlb_read[tlb_idx].mem_addend = (uintptr_t)ptr - addr;
            switch(size_log2) {
            case 0:
                ret = *(uint8_t *)ptr;
                break;
            case 1:
                ret = *(uint16_t *)ptr;
                break;
            case 2:
                ret = *(uint32_t *)ptr;
                break;
#if MLEN >= 64
            case 3:
                ret = *(uint64_t *)ptr;
                break;
#endif
#if MLEN >= 128
            case 4:
                ret = *(uint128_t *)ptr;
                break;
#endif
            default:
                abort();
            }
        } else {
            offset = paddr - pr->addr;
            if (((pr->devio_flags >> size_log2) & 1) != 0) {
                ret = pr->read_func(pr->opaque, offset, size_log2);
            }
#if MLEN >= 64
            else if ((pr->devio_flags & DEVIO_SIZE32) && size_log2 == 3) {
                /* emulate 64 bit access */
                ret = pr->read_func(pr->opaque, offset, 2);
                ret |= (uint64_t)pr->read_func(pr->opaque, offset + 4, 2) << 32;
            }
#endif
            else {
#ifdef DUMP_INVALID_MEM_ACCESS
                fprintf(stderr, "unsupported device read access: addr=0x");
                print_target_ulong(paddr);
                fprintf(stderr, " width=%d bits\n", 1 << (3 + size_log2));
#endif
                ret = 0;
            }
        }
    }
    *pval = ret;
    TRACK_MEM(addr,size,*pval);
    return 0;
}

/* return 0 if OK, != 0 if exception */
static no_inline int target_write_slow(RISCVCPUState *s, target_ulong addr,
                                       mem_uint_t val, int size_log2)
{
    int size, i, tlb_idx, err;
    target_ulong paddr, offset;
    uint8_t *ptr;
    PhysMemoryRange *pr;

    /* first handle unaligned accesses */
    size = 1 << size_log2;
    if (!CONFIG_ALLOW_MISALIGNED_ACCESS && (addr & (size - 1)) != 0) {
        s->pending_tval = addr;
        s->pending_exception = CAUSE_MISALIGNED_STORE;
        return -1;
    } else if ((addr & (size - 1)) != 0) {
        /* XXX: should avoid modifying the memory in case of exception */
        for (i = 0; i < size; i++) {
            err = target_write_u8(s, addr + i, (val >> (8 * i)) & 0xff);
            if (err)
                return err;
        }
    } else {
        int err = get_phys_addr(s, &paddr, addr, ACCESS_WRITE);
        if (err) {
            s->pending_tval = addr;
            s->pending_exception = err == -1 ?
                CAUSE_STORE_PAGE_FAULT : CAUSE_FAULT_STORE;
            return -1;
        }
        pr = get_phys_mem_range(s->mem_map, paddr);
        if (!pr) {
#ifdef DUMP_INVALID_MEM_ACCESS
            fprintf(stderr, "target_write_slow: invalid physical address 0x");
            print_target_ulong(paddr);
            fprintf(stderr, "\n");
#endif
        } else if (pr->is_ram) {
            phys_mem_set_dirty_bit(pr, paddr - pr->addr);
            tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);
            ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
            s->tlb_write[tlb_idx].vaddr = addr & ~PG_MASK;
            s->tlb_write[tlb_idx].mem_addend = (uintptr_t)ptr - addr;
            switch(size_log2) {
            case 0:
                *(uint8_t *)ptr = val;
                break;
            case 1:
                *(uint16_t *)ptr = val;
                break;
            case 2:
                *(uint32_t *)ptr = val;
                break;
#if MLEN >= 64
            case 3:
                *(uint64_t *)ptr = val;
                break;
#endif
#if MLEN >= 128
            case 4:
                *(uint128_t *)ptr = val;
                break;
#endif
            default:
                abort();
            }
        } else {
            offset = paddr - pr->addr;
            if (((pr->devio_flags >> size_log2) & 1) != 0) {
                pr->write_func(pr->opaque, offset, val, size_log2);
            }
#if MLEN >= 64
            else if ((pr->devio_flags & DEVIO_SIZE32) && size_log2 == 3) {
                /* emulate 64 bit access */
                pr->write_func(pr->opaque, offset,
                               val & 0xffffffff, 2);
                pr->write_func(pr->opaque, offset + 4,
                               (val >> 32) & 0xffffffff, 2);
            }
#endif
            else {
#ifdef DUMP_INVALID_MEM_ACCESS
                fprintf(stderr, "unsupported device write access: addr=0x");
                print_target_ulong(paddr);
                fprintf(stderr, " width=%d bits\n", 1 << (3 + size_log2));
#endif
            }
        }
    }
    TRACK_MEM(addr,size,val);
    return 0;
}

struct __attribute__((packed)) unaligned_u32 {
    uint32_t u32;
};

/* unaligned access at an address known to be a multiple of 2 */
static uint32_t get_insn32(uint8_t *ptr)
{
    return ((struct unaligned_u32 *)ptr)->u32;
}

/* return 0 if OK, != 0 if exception */
static no_inline __exception int target_read_insn_slow(RISCVCPUState *s,
                                                       uint32_t *insn,
                                                       int size,
                                                       target_ulong addr)
{
    int tlb_idx;
    target_ulong paddr;
    uint8_t *ptr;
    PhysMemoryRange *pr;

    int err = get_phys_addr(s, &paddr, addr, ACCESS_CODE);
    if (err) {
        s->pending_tval = addr;
        s->pending_exception = err == -1 ?
            CAUSE_FETCH_PAGE_FAULT : CAUSE_FAULT_FETCH;
        return -1;
    }
    pr = get_phys_mem_range(s->mem_map, paddr);
    if (!pr || !pr->is_ram) {
        /* XXX: we only access to execute code from RAM */
        s->pending_tval = addr;
        s->pending_exception = CAUSE_FAULT_FETCH;
        return -1;
    }
    tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);
    ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
    s->tlb_code[tlb_idx].vaddr = addr & ~PG_MASK;
    s->tlb_code[tlb_idx].mem_addend = (uintptr_t)ptr - addr;

    /* check for page crossing */
    int tlb_idx_cross = ((addr+2) >> PG_SHIFT) & (TLB_SIZE - 1);
    if ((tlb_idx != tlb_idx_cross) && (size == 32)) {
        target_ulong paddr_cross;
        int err = get_phys_addr(s, &paddr_cross, addr+2, ACCESS_CODE);
        if (err) {
            s->pending_tval = addr;
            s->pending_exception = err == -1 ?
                CAUSE_FETCH_PAGE_FAULT : CAUSE_FAULT_FETCH;
            return -1;
        }

        PhysMemoryRange *pr_cross = get_phys_mem_range(s->mem_map, paddr_cross);
        if (!pr_cross || !pr_cross->is_ram) {
            /* XXX: we only access to execute code from RAM */
            s->pending_tval = addr;
            s->pending_exception = CAUSE_FAULT_FETCH;
            return -1;
        }
        uint8_t *ptr_cross = pr_cross->phys_mem + (uintptr_t)(paddr_cross - pr_cross->addr);

        *insn = (uint32_t)*((uint16_t*)ptr);
        *insn |= ((uint32_t)*((uint16_t*)ptr_cross) << 16);
        return 0;
    }

    if (size == 32) {
        *insn = (uint32_t)*((uint32_t*)ptr);
    } else if (size == 16) {
        *insn = (uint32_t)*((uint16_t*)ptr);
    } else {
        assert(0);
    }

    TRACK_MEM(addr,32,*(insn));

    return 0;
}

/* addr must be aligned */
static inline __exception int target_read_insn_u16(RISCVCPUState *s, uint16_t *pinsn,
                                                   target_ulong addr)
{
    uint32_t tlb_idx;
    uintptr_t mem_addend;

    tlb_idx = (addr >> PG_SHIFT) & (TLB_SIZE - 1);
    if (likely(s->tlb_code[tlb_idx].vaddr == (addr & ~PG_MASK))) {
        mem_addend = s->tlb_code[tlb_idx].mem_addend;
        TRACK_MEM(addr,16,*(uint16_t *)(mem_addend + (uintptr_t)addr));
        *pinsn = *(uint16_t *)(mem_addend + (uintptr_t)addr);
        return 0;
    } else {
        uint32_t pinsn_temp;
        if (target_read_insn_slow(s, &pinsn_temp, 16, addr))
            return -1;
        *pinsn = (pinsn_temp & 0xffff);
    }
    return 0;
}

static void tlb_init(RISCVCPUState *s)
{
    int i;

    for (i = 0; i < TLB_SIZE; i++) {
        s->tlb_read[i].vaddr = -1;
        s->tlb_write[i].vaddr = -1;
        s->tlb_code[i].vaddr = -1;
    }
}

static void tlb_flush_all(RISCVCPUState *s)
{
    tlb_init(s);
}

static void tlb_flush_vaddr(RISCVCPUState *s, target_ulong vaddr)
{
    tlb_flush_all(s);
}

/* XXX: inefficient but not critical as long as it is seldom used */
void riscv_cpu_flush_tlb_write_range_ram(RISCVCPUState *s,
                                         uint8_t *ram_ptr, size_t ram_size)
{
    uint8_t *ptr, *ram_end;
    int i;

    ram_end = ram_ptr + ram_size;
    for (i = 0; i < TLB_SIZE; i++) {
        if (s->tlb_write[i].vaddr != -1) {
            ptr = (uint8_t *)(s->tlb_write[i].mem_addend +
                              (uintptr_t)s->tlb_write[i].vaddr);
            if (ptr >= ram_ptr && ptr < ram_end) {
                s->tlb_write[i].vaddr = -1;
            }
        }
    }
}


#define SSTATUS_MASK (  MSTATUS_SIE   \
                      | MSTATUS_SPIE  \
                      | MSTATUS_SPP   \
                      | MSTATUS_FS    \
                      | MSTATUS_SUM   \
                      | MSTATUS_MXR   \
                      | MSTATUS_UXL_MASK )

#define MSTATUS_MASK (  MSTATUS_SIE  \
                      | MSTATUS_MIE  \
                      | MSTATUS_SPIE \
                      | MSTATUS_MPIE \
                      | MSTATUS_SPP  \
                      | MSTATUS_MPP  \
                      | MSTATUS_FS   \
                      | MSTATUS_MPRV \
                      | MSTATUS_SUM  \
                      | MSTATUS_MXR  \
                      | MSTATUS_TVM  \
                      | MSTATUS_TW   \
                      | MSTATUS_TSR  \
                      | MSTATUS_UXL_MASK \
                      | MSTATUS_SXL_MASK )

/* cycle and insn counters */
#define COUNTEREN_MASK ((1 << 0) | (1 << 2))

/* return the complete mstatus with the SD bit */
static target_ulong get_mstatus(RISCVCPUState *s, target_ulong mask)
{
    target_ulong val;
    BOOL sd;
    val = s->mstatus | (s->fs << MSTATUS_FS_SHIFT);
    val &= mask;
    sd = ((val & MSTATUS_FS) == MSTATUS_FS) |
        ((val & MSTATUS_XS) == MSTATUS_XS);
    if (sd)
        val |= (target_ulong)1 << (s->cur_xlen - 1);
    return val;
}

static int get_base_from_xlen(int xlen)
{
    if (xlen == 32)
        return 1;
    else if (xlen == 64)
        return 2;
    else
        return 3;
}

static void set_mstatus(RISCVCPUState *s, target_ulong val)
{
    /* flush the TLBs if change of MMU config */
    target_ulong mod = s->mstatus ^ val;
    if ((mod & (MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR)) != 0 ||
        ((s->mstatus & MSTATUS_MPRV) && (mod & MSTATUS_MPP) != 0)) {
        tlb_flush_all(s);
    }
    s->fs = (val >> MSTATUS_FS_SHIFT) & 3;

    target_ulong mask = MSTATUS_MASK & ~MSTATUS_FS;
    s->mstatus = s->mstatus & ~mask | val & mask;

    /* IMPORTANT NOTE: should never change the UXL and SXL bits */
    s->mstatus |= ((uint64_t)(2) << MSTATUS_UXL_SHIFT) | ((uint64_t)(2) << MSTATUS_SXL_SHIFT);
}

static BOOL counter_access_ok(RISCVCPUState *s, uint32_t csr)
{
    uint32_t counteren = 0;

    switch (s->priv) {
    case PRV_U: counteren = s->mcounteren & s->scounteren; break;
    case PRV_S: counteren = s->mcounteren; break;
    case PRV_M: counteren = ~0; break;
    default: ;
    }

    return (counteren >> (csr & 31)) & 1;
}

/* return -1 if invalid CSR. 0 if OK. 'will_write' indicate that the
   csr will be written after (used for CSR access check) */
static int csr_read(RISCVCPUState *s, target_ulong *pval, uint32_t csr,
                     BOOL will_write)
{
    target_ulong val;

    if (((csr & 0xc00) == 0xc00) && will_write)
        return -1; /* read-only CSR */
    if (s->priv < ((csr >> 8) & 3))
        return -1; /* not enough priviledge */

    switch(csr) {
#if FLEN > 0
    case 0x001: /* fflags */
        if (s->fs == 0)
            return -1;
        val = s->fflags;
        break;
    case 0x002: /* frm */
        if (s->fs == 0)
            return -1;
        val = s->frm;
        break;
    case 0x003:
        if (s->fs == 0)
            return -1;
        val = s->fflags | (s->frm << 5);
        break;
#endif
    case 0x100:
        val = get_mstatus(s, SSTATUS_MASK);
        break;
    case 0x104: /* sie */
        val = s->mie & s->mideleg;
        break;
    case 0x105:
        val = s->stvec;
        break;
    case 0x106:
        val = s->scounteren;
        break;
    case 0x140:
        val = s->sscratch;
        break;
    case 0x141:
        val = s->sepc;
        break;
    case 0x142:
        val = s->scause;
        break;
    case 0x143:
        val = s->stval;
        break;
    case 0x144: /* sip */
        val = s->mip & s->mideleg;
        break;
    case 0x180:
        if (s->priv == PRV_S && s->mstatus & MSTATUS_TVM)
            return -1;
        val = s->satp;
        break;
    case 0x300:
        val = get_mstatus(s, (target_ulong)-1);
        break;
    case 0x301:
        val = s->misa;
        val |= (target_ulong)s->mxl << (s->cur_xlen - 2);
        break;
    case 0x302:
        val = s->medeleg;
        break;
    case 0x303:
        val = s->mideleg;
        break;
    case 0x304:
        val = s->mie;
        break;
    case 0x305:
        val = s->mtvec;
        break;
    case 0x306:
        val = s->mcounteren;
        break;
    case 0x340:
        val = s->mscratch;
        break;
    case 0x341:
        val = s->mepc;
        break;
    case 0x342:
        val = s->mcause;
        break;
    case 0x343:
        val = s->mtval;
        break;
    case 0x344:
        val = s->mip;
        break;
    case 0x7a0: // tselect
        val = s->tselect;
        break;
    case 0x7a1: // tdata1
        val = s->tdata1[s->tselect];
        break;
    case 0x7a2: // tdata2
        val = s->tdata2[s->tselect];
        break;
    case 0x7a3: // tdata3
        val = s->tdata3[s->tselect];
        break;
    case 0x7b0:
        val = s->dcsr;
        break;
    case 0x7b1:
        val = s->dpc;
        break;
    case 0x7b2:
        val = s->dscratch;
        break;

    case 0xb00: /* mcycle */
    case 0xc00: /* cycle */
        if (!counter_access_ok(s, csr))
            goto invalid_csr;
        val = (int64_t)s->mcycle;
        break;

    case 0xb02: /* minstret */
    case 0xc02: /* uinstret */
        if (!counter_access_ok(s, csr))
            goto invalid_csr;
        val = (int64_t)s->minstret;
        break;
    case 0xb03:
    case 0xc03:
    case 0xb04:
    case 0xc04:
    case 0xb05:
    case 0xc05:
    case 0xb06:
    case 0xc06:
    case 0xb07:
    case 0xc07:
    case 0xb08:
    case 0xc08:
    case 0xb09:
    case 0xc09:
    case 0xb0a:
    case 0xc0a:
    case 0xb0b:
    case 0xc0b:
    case 0xb0c:
    case 0xc0c:
    case 0xb0d:
    case 0xc0d:
    case 0xb0e:
    case 0xc0e:
    case 0xb0f:
    case 0xc0f:
    case 0xb10:
    case 0xc10:
    case 0xb11:
    case 0xc11:
    case 0xb12:
    case 0xc12:
    case 0xb13:
    case 0xc13:
    case 0xb14:
    case 0xc14:
    case 0xb15:
    case 0xc15:
    case 0xb16:
    case 0xc16:
    case 0xb17:
    case 0xc17:
    case 0xb18:
    case 0xc18:
    case 0xb19:
    case 0xc19:
    case 0xb1a:
    case 0xc1a:
    case 0xb1b:
    case 0xc1b:
    case 0xb1c:
    case 0xc1c:
    case 0xb1d:
    case 0xc1d:
    case 0xb1e:
    case 0xc1e:
    case 0xb1f:
    case 0xc1f:
        if (!counter_access_ok(s, csr))
            goto invalid_csr;
        val = 0; // mhpmcounter3..31
        break;
    case 0xb80: /* mcycleh */
    case 0xc80: /* cycleh */
        if (s->cur_xlen != 32 || !counter_access_ok(s, csr))
            goto invalid_csr;
        val = s->mcycle >> 32;
        break;

    case 0xb82: /* minstreth */
    case 0xc82: /* instreth */
        if (s->cur_xlen != 32 || !counter_access_ok(s, csr))
            goto invalid_csr;
        val = s->minstret >> 32;
        break;

    case 0xf14:
        val = s->mhartid;
        break;
    case 0xf13:
        val = s->mimpid;
        break;
    case 0xf12:
        val = s->marchid;
        break;
    case 0xf11:
        val = s->mvendorid;
        break;
    case 0x323:
    case 0x324:
    case 0x325:
    case 0x326:
    case 0x327:
    case 0x328:
    case 0x329:
    case 0x32a:
    case 0x32b:
    case 0x32c:
    case 0x32d:
    case 0x32e:
    case 0x32f:
    case 0x330:
    case 0x331:
    case 0x332:
    case 0x333:
    case 0x334:
    case 0x335:
    case 0x336:
    case 0x337:
    case 0x338:
    case 0x339:
    case 0x33a:
    case 0x33b:
    case 0x33c:
    case 0x33d:
    case 0x33e:
    case 0x33f:
        val = s->mhpmevent[csr & 0x1F];
        break;

    case 0x81F: // Esperanto Flush All cachelines
    case 0x8D0:
    case 0x8D1:
        val = 0;
        break;

    default:
    invalid_csr:
#ifdef DUMP_INVALID_CSR
        /* the 'time' counter is usually emulated */
        if (csr != 0xc01 && csr != 0xc81) {
            fprintf(stderr, "csr_read: invalid CSR=0x%x\n", csr);
        }
#endif
        *pval = 0;
        return -1;
    }
    *pval = val;
    return 0;
}

#if FLEN > 0
static void set_frm(RISCVCPUState *s, unsigned int val)
{
    s->frm = val;
}

/* return -1 if invalid roundind mode */
static int get_insn_rm(RISCVCPUState *s, unsigned int rm)
{
    if (rm == 7)
        rm = s->frm;

    if (rm >= 5)
        return -1;
    else
        return rm;
}
#endif

static void handle_write_validation1(RISCVCPUState *s, target_ulong val)
{
    if (val < 256) {// upper bits zero is the expected
        putchar((char)val); // Console to stdout
        return;
    }

    target_ulong cmd_payload = val & PAYLOAD_MASK;
    switch (val >> CMD_OFFSET) {
    // Valid only for riscvemu64
    case VALIDATION_CMD_LINUX:
        if (cmd_payload == LINUX_CMD_VALUE_INVALID
            || cmd_payload >= LINUX_CMD_VALUE_NUM) {
            fprintf(stderr, "ET UNKNOWN linux command=%" PR_target_ulong "\n",
                    cmd_payload);
        }
        break;
    case VALIDATION_CMD_BENCH:
        if (cmd_payload == BENCH_CMD_VALUE_INVALID
            || cmd_payload >= BENCH_CMD_VALUE_NUM) {
            fprintf(stderr, "ET UNKNOWN benchmark command=%" PR_target_ulong "\n",
                    cmd_payload);
        }
        break;

    default:
        fprintf(stderr, "ET UNKNOWN validation1 command=%llx\n", (long long)val);
    }

    for (int i = 0; i < countof(validation_events); ++i) {
        if (val == validation_events[i].value
            && validation_events[i].terminate
            && s->terminating_event != NULL
            && strcmp(validation_events[i].name, s->terminating_event) == 0) {
            s->terminate_simulation = TRUE;
            fprintf(stderr, "ET terminating validation event: %s encountered.",
                    s->terminating_event);
            fprintf(stderr, " Instructions committed: %lli \n",
                    (long long)s->minstret);
            break;
        }
    }
}

/* return -1 if invalid CSR, 0 if OK, 1 if the interpreter loop must be
   exited (e.g. XLEN was modified), 2 if TLBs have been flushed. */
static int csr_write(RISCVCPUState *s, uint32_t csr, target_ulong val)
{
    target_ulong mask;

#if defined(DUMP_CSR)
    fprintf(stderr, "csr_write: csr=0x%03x val=0x", csr);
    print_target_ulong(val);
    fprintf(stderr, "\n");
#endif
    switch(csr) {
#if FLEN > 0
    case 0x001: /* fflags */
        s->fflags = val & 0x1f;
        s->fs = 3;
        break;
    case 0x002: /* frm */
        set_frm(s, val & 7);
        s->fs = 3;
        break;
    case 0x003: /* fcsr */
        set_frm(s, (val >> 5) & 7);
        s->fflags = val & 0x1f;
        s->fs = 3;
        break;
#endif
    case 0x100: /* sstatus */
        set_mstatus(s, s->mstatus & ~SSTATUS_MASK | val & SSTATUS_MASK);
        break;
    case 0x104: /* sie */
        mask = s->mideleg;
        s->mie = s->mie & ~mask | val & mask;
        break;
    case 0x105:
        // RTLMAX-178, Maxion enforces 64-byte alignment for vectored interrupts
        if (val & 1) val &= ~63 + 1;
        s->stvec = val & ~2;
        break;
    case 0x106:
        s->scounteren = val & COUNTEREN_MASK;
        break;
    case 0x140:
        s->sscratch = val;
        break;
    case 0x141:
        s->sepc = val & (s->misa & MCPUID_C ? ~1 : ~3);
        break;
    case 0x142:
        s->scause = val & (CAUSE_MASK | (target_ulong)1 << (s->cur_xlen - 1));
        break;
    case 0x143:
        s->stval = val;
        break;
    case 0x144: /* sip */
        mask = s->mideleg;
        s->mip = s->mip & ~mask | val & mask;
        break;
    case 0x180:
        if (s->priv == PRV_S && s->mstatus & MSTATUS_TVM)
            return -1;
        {
            uint64_t mode = (val >> 60) & 15;
            if (mode == 0 || mode == 8 || mode == 9)
                s->satp = val & SATP_MASK;
        }
        /* no ASID implemented [yet] */
        tlb_flush_all(s);
        return 2;

    case 0x300:
        set_mstatus(s, val);
        break;
    case 0x301: /* misa */
        {
            int new_mxl;
            new_mxl = (val >> (s->cur_xlen - 2)) & 3;
            if (new_mxl >= 1 && new_mxl <= get_base_from_xlen(64)) {
                /* Note: misa is only modified in M level, so cur_xlen
                   = 2^(mxl + 4) */
                if (s->mxl != new_mxl) {
                    s->mxl = new_mxl;
                    s->cur_xlen = 1 << (new_mxl + 4);
                    return 1;
                }
            }
        }
        /*
         * We don't support turning C on dynamically, but if we did we
         * would have to check for PC alignment here and potentially
         * suppress the C per 3.1.1 in the priv 1.11 (draft) spec.
         */
        break;
    case 0x302: {
        target_ulong mask = 0xB109; // matching Spike and Maxion
        s->medeleg = s->medeleg & ~mask | val & mask;
        break;
    }
    case 0x303:
        mask = MIP_SSIP | MIP_STIP | MIP_SEIP;
        s->mideleg = s->mideleg & ~mask | val & mask;
        break;
    case 0x304:
        mask = MIE_MEIE | MIE_SEIE /*| MIE_UEIE*/ | MIE_MTIE | MIE_STIE | /*MIE_UTIE | */ MIE_MSIE | MIE_SSIE /*| MIE_USIE */;
        s->mie = s->mie & ~mask | val & mask;
        break;
    case 0x305:
        // RTLMAX-178, Maxion enforces 64-byte alignment for vectored interrupts
        if (val & 1) val &= ~63 + 1;
        s->mtvec = val & ~2;
        break;
    case 0x306:
        s->mcounteren = val & COUNTEREN_MASK;
        break;
    case 0x340:
        s->mscratch = val;
        break;
    case 0x341:
        s->mepc = val & (s->misa & MCPUID_C ? ~1 : ~3);
        break;
    case 0x342:
        s->mcause = val & (CAUSE_MASK | (target_ulong)1 << (s->cur_xlen - 1));
        break;
    case 0x343:
        s->mtval = val;
        break;
    case 0x344:
        mask = /* MEIP | */ MIP_SEIP | /*MIP_UEIP | MTIP | */ MIP_STIP | /*MIP_UTIP | MSIP | */ MIP_SSIP /*| MIP_USIP*/;
        s->mip = s->mip & ~mask | val & mask;
        break;
    case 0x7a0: // tselect
        s->tselect = val % MAX_TRIGGERS;
        break;
    case 0x7a1: // tdata1
        // Only support No Trigger and MControl
        {
            int type = val >> (s->cur_xlen - 4);
            if (type != 0 && type != 2)
                break;
            // SW can write type and mcontrol bits M and EXECUTE
            mask = ((target_ulong)15 << (s->cur_xlen - 4)) | MCONTROL_M | MCONTROL_EXECUTE;
            s->tdata1[s->tselect] = s->tdata1[s->tselect] & ~mask | val & mask;
        }
        break;
    case 0x7a2: // tdata2
        s->tdata2[s->tselect] = val;
        break;
    case 0x7a3: // tdata3
        s->tdata3[s->tselect] = val;
    case 0x323:
    case 0x324:
    case 0x325:
    case 0x326:
    case 0x327:
    case 0x328:
    case 0x329:
    case 0x32a:
    case 0x32b:
    case 0x32c:
    case 0x32d:
    case 0x32e:
    case 0x32f:
    case 0x330:
    case 0x331:
    case 0x332:
    case 0x333:
    case 0x334:
    case 0x335:
    case 0x336:
    case 0x337:
    case 0x338:
    case 0x339:
    case 0x33a:
    case 0x33b:
    case 0x33c:
    case 0x33d:
    case 0x33e:
    case 0x33f:
        s->mhpmevent[csr & 0x1F] = val;
        break;
    case 0x7b0:
        /* XXX We have a very incomplete implementation of debug mode, only just enough
           to restore a snapshot and stop counters */
        mask = 0x603; // stopcount and stoptime && also the priv level to return
        s->dcsr = s->dcsr & ~mask | val & mask;
        s->stop_the_counter = s->dcsr & 0x600 != 0;
        break;

    case 0x7b1:
        s->dpc = val & (s->misa & MCPUID_C ? ~1 : ~3);
        break;

    case 0x7b2:
        s->dscratch = val;
        break;
    case 0x81F: // Esperanto Flush All cachelines
        // Ignore it
        break;
    case 0x8D0: // Esperanto validation0 register
        if ((val >> 12) == 0xDEAD0) // Begin
            fprintf(stderr, "ET validation begin code=%llx\n", (long long)val & 0xFFF);
        else if ((val >> 12) == 0x1FEED) {
            fprintf(stderr, "ET validation PASS code=%llx\n", (long long)val & 0xFFF);
            s->terminate_simulation = TRUE;
            break;
        } else if ((val >> 12) == 0x50BAD) {
            fprintf(stderr, "ET validation FAIL code=%llx\n", (long long)val & 0xFFF);
            s->terminate_simulation = TRUE;
            break;
        } else
            fprintf(stderr, "ET UNKNOWN command=%llx code=%llx\n", (long long)val >> 12,
                    (long long)(val & 0xFFF));
        break;
    case 0x8D1: // Esperanto validation1 register
        handle_write_validation1(s, val);
        break;
    case 0xb00: /* mcycle */
        s->mcycle = val;
        break;
    case 0xb02: /* minstret */
        s->minstret = val;
        break;
    case 0xb03:
    case 0xb04:
    case 0xb05:
    case 0xb06:
    case 0xb07:
    case 0xb08:
    case 0xb09:
    case 0xb0a:
    case 0xb0b:
    case 0xb0c:
    case 0xb0d:
    case 0xb0e:
    case 0xb0f:
    case 0xb10:
    case 0xb11:
    case 0xb12:
    case 0xb13:
    case 0xb14:
    case 0xb15:
    case 0xb16:
    case 0xb17:
    case 0xb18:
    case 0xb19:
    case 0xb1a:
    case 0xb1b:
    case 0xb1c:
    case 0xb1d:
    case 0xb1e:
    case 0xb1f:
        // Allow, but ignore to write to performance counters mhpmcounter
        break;

    case 0xb80: /* mcycleh */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        s->mcycle = (uint32_t) s->mcycle | ((uint64_t)val << 32);
        break;
    case 0xb82: /* minstreth */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        s->minstret = (uint32_t) s->minstret | ((uint64_t)val << 32);
        break;

    default:
    invalid_csr:
#ifdef DUMP_INVALID_CSR
        fprintf(stderr, "csr_write: invalid CSR=0x%x\n", csr);
#endif
        return -1;
    }
    return 0;
}

static void set_priv(RISCVCPUState *s, int priv)
{
    if (s->priv != priv) {
        tlb_flush_all(s);
        /* change the current xlen */
        {
            int mxl;
            if (priv == PRV_S)
                mxl = (s->mstatus >> MSTATUS_SXL_SHIFT) & 3;
            else if (priv == PRV_U)
                mxl = (s->mstatus >> MSTATUS_UXL_SHIFT) & 3;
            else
                mxl = s->mxl;
            s->cur_xlen = 1 << (4 + mxl);
        }
        s->priv = priv;
    }
}

static void raise_exception2(RISCVCPUState *s, uint32_t cause,
                             target_ulong tval)
{
    BOOL deleg;
    target_ulong causel;

#if defined(DUMP_EXCEPTIONS)
    const static char *cause_s[] = {
        "misaligned_fetch",
        "fault_fetch",
        "illegal_instruction",
        "breakpoint",
        "misaligned_load",
        "fault_load",
        "misaligned_store",
        "fault_store",
        "user_ecall",
        "<reserved (supervisor_ecall?)>",
        "<reserved (hypervisor_ecall?)>",
        "<reserved (machine_ecall?)>",
        "fetch_page_fault",
        "load_page_fault",
        "<reserved_14>",
        "store_page_fault",
    };

    if (cause & CAUSE_INTERRUPT)
        fprintf(stderr, "core   0: exception interrupt #%d, epc 0x%016jx\n",
               (cause & (64 - 1)), (uintmax_t)s->pc);
    else if (cause <= CAUSE_STORE_PAGE_FAULT) {
        fprintf(stderr, "priv: %d core   0: exception %s, epc 0x%016jx\n",
               s->priv, cause_s[cause], (uintmax_t)s->pc);
        fprintf(stderr, "core   0:           tval 0x%016jx\n", (uintmax_t)tval);
    } else {
        fprintf(stderr, "core   0: exception %d, epc 0x%016jx\n",
               cause, (uintmax_t)s->pc);
        fprintf(stderr, "core   0:           tval 0x%016jx\n", (uintmax_t)tval);
    }
#endif

    if (s->priv <= PRV_S) {
        /* delegate the exception to the supervisor priviledge */
        if (cause & CAUSE_INTERRUPT)
            deleg = (s->mideleg >> (cause & (64 - 1))) & 1;
        else
            deleg = (s->medeleg >> cause) & 1;
    } else {
        deleg = 0;
    }

    causel = cause & CAUSE_MASK;
    if (cause & CAUSE_INTERRUPT)
        causel |= (target_ulong)1 << (s->cur_xlen - 1);

    if (deleg) {
        s->scause = causel;
        s->sepc = s->pc;
        s->stval = tval;
        s->mstatus = (s->mstatus & ~MSTATUS_SPIE) |
            (!!(s->mstatus & MSTATUS_SIE) << MSTATUS_SPIE_SHIFT);
        s->mstatus = (s->mstatus & ~MSTATUS_SPP) |
            (s->priv << MSTATUS_SPP_SHIFT);
        s->mstatus &= ~MSTATUS_SIE;
        set_priv(s, PRV_S);
        if (s->stvec & 1 && cause & CAUSE_INTERRUPT)
            s->pc = s->stvec - 1 + 4 * s->scause;
        else
            s->pc = s->stvec;
    } else {
        s->mcause = causel;
        s->mepc = s->pc;
        s->mtval = tval;

        /* When a trap is taken from privilege mode y into privilege
           mode x, xPIE is set to the value of xIE; xIE is set to 0;
           and xPP is set to y.

           Here x = M, thus MPIE = MIE; MIE = 0; MPP = s->priv
        */

        s->mstatus = (s->mstatus & ~MSTATUS_MPIE) |
            (!!(s->mstatus & MSTATUS_MIE) << MSTATUS_MPIE_SHIFT);

        s->mstatus = (s->mstatus & ~MSTATUS_MPP) |
            (s->priv << MSTATUS_MPP_SHIFT);
        s->mstatus &= ~MSTATUS_MIE;
        set_priv(s, PRV_M);
        if (s->mtvec & 1 && cause & CAUSE_INTERRUPT)
            s->pc = s->mtvec - 1 + 4 * s->mcause;
        else
            s->pc = s->mtvec;
    }
}

static void raise_exception(RISCVCPUState *s, uint32_t cause)
{
    raise_exception2(s, cause, 0);
}

static void handle_sret(RISCVCPUState *s)
{

    /* Copy down SPIE to SIE and set SPIE */
    s->mstatus &= ~MSTATUS_SIE;
    s->mstatus |= (s->mstatus >> 4) & MSTATUS_SIE;
    s->mstatus |= MSTATUS_SPIE;

    int spp = (s->mstatus & MSTATUS_SPP) >> MSTATUS_SPP_SHIFT;
    s->mstatus &= ~MSTATUS_SPP;

    set_priv(s, spp);
    s->pc = s->sepc;
}

static void handle_mret(RISCVCPUState *s)
{
    /* Copy down MPIE to MIE and set MPIE */
    s->mstatus &= ~MSTATUS_MIE;
    s->mstatus |= (s->mstatus >> 4) & MSTATUS_MIE;
    s->mstatus |= MSTATUS_MPIE;

    int mpp = (s->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    s->mstatus &= ~MSTATUS_MPP;

    set_priv(s, mpp);
    s->pc = s->mepc;
}

static void handle_dret(RISCVCPUState *s)
{
    s->stop_the_counter = FALSE; // Enable counters again
    set_priv(s, s->dcsr & 3);
    s->pc = s->dpc;
}

static inline uint32_t get_pending_irq_mask(RISCVCPUState *s)
{
    uint32_t pending_ints, enabled_ints;

    pending_ints = s->mip & s->mie;
    if (pending_ints == 0)
        return 0;

    enabled_ints = 0;
    switch(s->priv) {
    case PRV_M:
        if (s->mstatus & MSTATUS_MIE)
            enabled_ints = ~s->mideleg;
        break;
    case PRV_S:
        enabled_ints = ~s->mideleg;
        if (s->mstatus & MSTATUS_SIE)
            enabled_ints |= s->mideleg;
        break;
    default:
    case PRV_U:
        enabled_ints = -1;
        break;
    }
    return pending_ints & enabled_ints;
}

static __exception int raise_interrupt(RISCVCPUState *s)
{
    uint32_t mask;
    int irq_num;

    mask = get_pending_irq_mask(s);
    if (mask == 0)
        return 0;
    irq_num = ctz32(mask);
#ifdef DUMP_INTERRUPTS
    fprintf(stderr, "raise_interrupt: irq=%d priv=%d pc=%llx\n", irq_num,s->priv,(unsigned long long)s->pc);
#endif

    raise_exception(s, irq_num | CAUSE_INTERRUPT);
    return -1;
}

static inline int32_t sext(int32_t val, int n)
{
    return (val << (32 - n)) >> (32 - n);
}

static inline uint32_t get_field1(uint32_t val, int src_pos,
                                  int dst_pos, int dst_pos_max)
{
    int mask;
    assert(dst_pos_max >= dst_pos);
    mask = ((1 << (dst_pos_max - dst_pos + 1)) - 1) << dst_pos;
    if (dst_pos >= src_pos)
        return (val << (dst_pos - src_pos)) & mask;
    else
        return (val >> (src_pos - dst_pos)) & mask;
}

static inline RISCVCTFInfo ctf_compute_hint(int rd, int rs1)
{
    int rd_link  = rd  == 1 || rd  == 5;
    int rs1_link = rs1 == 1 || rs1 == 5;
    RISCVCTFInfo k = rd_link*2 + rs1_link + ctf_taken_jalr;

    if (k == ctf_taken_jalr_pop_push && rs1 == rd)
        return ctf_taken_jalr_push;

    return k;
}

#define XLEN 32
#include "riscvemu_template.h"

#define XLEN 64
#include "riscvemu_template.h"

int riscv_cpu_interp(RISCVCPUState *s, int n_cycles)
{
    int executed = 0;

#ifdef USE_GLOBAL_STATE
    s = &riscv_cpu_global_state;
#endif
    uint64_t timeout;

    timeout = s->insn_counter + n_cycles;
    {
        n_cycles = timeout - s->insn_counter;
        switch (s->cur_xlen) {
        case 64:
            executed += riscv_cpu_interp64(s, n_cycles);
            break;
        default:
            abort();
        }
    }

    return executed;
}

/* Note: the value is not accurate when called in riscv_cpu_interp() */
uint64_t riscv_cpu_get_cycles(RISCVCPUState *s)
{
    return s->mcycle;
}

void riscv_cpu_set_mip(RISCVCPUState *s, uint32_t mask)
{
    s->mip |= mask;
    /* exit from power down if an interrupt is pending */
    if (s->power_down_flag && (s->mip & s->mie) != 0)
        s->power_down_flag = FALSE;
}

void riscv_cpu_reset_mip(RISCVCPUState *s, uint32_t mask)
{
    s->mip &= ~mask;
}

uint32_t riscv_cpu_get_mip(RISCVCPUState *s)
{
    return s->mip;
}

BOOL riscv_cpu_get_power_down(RISCVCPUState *s)
{
    return s->power_down_flag;
}

RISCVCPUState *riscv_cpu_init(PhysMemoryMap *mem_map,
                              const char *validation_terminate_event)
{
    RISCVCPUState *s;

#ifdef USE_GLOBAL_STATE
    s = &riscv_cpu_global_state;
#else
    s = mallocz(sizeof(*s));
#endif
    s->mem_map = mem_map;
    s->pc = BOOT_BASE_ADDR;
    s->priv = PRV_M;
    s->cur_xlen = 64;
    s->mxl = get_base_from_xlen(64);
    s->mstatus = ((uint64_t)s->mxl << MSTATUS_UXL_SHIFT) |
        ((uint64_t)s->mxl << MSTATUS_SXL_SHIFT) |
        (3 << MSTATUS_MPP_SHIFT);
    s->misa |= MCPUID_SUPER | MCPUID_USER | MCPUID_I | MCPUID_M | MCPUID_A;
    s->most_recently_written_reg = -1;
#if FLEN >= 32
    s->most_recently_written_fp_reg = -1;
    s->misa |= MCPUID_F;
#endif
#if FLEN >= 64
    s->misa |= MCPUID_D;
#endif
#if FLEN >= 128
    s->misa |= MCPUID_Q;
#endif
    s->misa |= MCPUID_C;

    /* Match Maxion */
    s->mvendorid = 11 * 128 + 101; // Esperanto JEDEC number 101 in bank 11
    s->marchid   = (1ULL << 63) | 2;
    s->mimpid    = 1;
    s->mhartid   = 0;

    s->store_repair_addr = ~0;
    s->tselect = 0;
    for (int i = 0; i < MAX_TRIGGERS; ++i) {
      s->tdata1[i] = ~(target_ulong)0;
      s->tdata2[i] = ~(target_ulong)0;
    }

    tlb_init(s);

    // Initialize valiation event info
    s->terminating_event = validation_terminate_event;

    return s;
}

void riscv_cpu_end(RISCVCPUState *s)
{
#ifdef USE_GLOBAL_STATE
    free(s);
#endif
}

void riscv_set_pc(RISCVCPUState *s, uint64_t val)
{
    s->pc = val & (s->misa & MCPUID_C ? ~1 : ~3);
}

uint64_t riscv_get_pc(RISCVCPUState *s)
{
    return s->pc;
}

uint64_t riscv_get_reg(RISCVCPUState *s, int rn)
{
    assert(0 <= rn && rn < 32);
    return s->reg[rn];
}

uint64_t riscv_get_reg_previous(RISCVCPUState *s, int rn)
{
    assert(0 <= rn && rn < 32);
    return s->reg_prior[rn];
}


void riscv_repair_csr(RISCVCPUState *s, uint32_t reg_num, uint64_t csr_num, uint64_t csr_val)
{
    switch (csr_num & 0xFFF) {
    case 0xb00:
    case 0xc00: // mcycle
        s->mcycle       = csr_val;
        s->reg[reg_num] = csr_val;
        break;
    case 0xb02:
    case 0xc02: // minstret
        s->minstret     = csr_val;
        s->reg[reg_num] = csr_val;
        break;

    default:
        fprintf(stderr, "riscv_repair_csr: This CSR is unsupported for repairing: %lx\n",
                (unsigned long)csr_num);
        break;
    }
}

int riscv_repair_load(RISCVCPUState *s, uint32_t reg_num, uint64_t reg_val,
                      uint64_t htif_tohost_addr,
                      uint64_t *htif_tohost,
                      uint64_t *htif_fromhost)
{
    BOOL repair_load = 0;
    if (s->last_addr == htif_tohost_addr) {
        *htif_tohost = reg_val;
        repair_load = 1;
    } else if (s->last_addr == htif_tohost_addr + 64) {
        *htif_fromhost = reg_val;
        repair_load = 1;
    } else if (*htif_tohost <= s->last_addr && s->last_addr < *htif_tohost + 32) {
        target_write_slow(s, s->last_addr, reg_val, 3);
        repair_load = 1;
    }

    if (repair_load) {
        s->reg[reg_num] = reg_val;
        return 1;
    } else
        return 0;
}

int riscv_repair_store(RISCVCPUState *s, uint32_t reg_num, uint32_t funct3)
{
    switch (funct3) {
    case 2:
        if (target_write_u32(s, s->store_repair_addr, s->store_repair_val32))
            return 1;
        else
            s->reg[reg_num] = 1;
        break;

    case 3:
        if (target_write_u64(s, s->store_repair_addr, s->store_repair_val64))
            return 1;
        else
            s->reg[reg_num] = 1;
        break;

    default:
        fprintf(stderr, "riscv_repair_store: Store repairing not successful.");
        return 2;
    }

    return 0;
}

/* Sync up the shadow register state if there are no errors */
void riscv_cpu_sync_regs(RISCVCPUState *s)
{
  for (int i = 1; i < 32; ++i) {
     s->reg_prior[i] = s->reg[i];
   }
}

uint64_t riscv_cpu_get_mstatus(RISCVCPUState* s){
  return get_mstatus(s, MSTATUS_MASK);
}

uint64_t riscv_cpu_get_medeleg(RISCVCPUState* s){
  return s->medeleg;
}

uint64_t riscv_get_fpreg(RISCVCPUState *s, int rn)
{
    assert(0 <= rn && rn < 32);
    return s->fp_reg[rn];
}

void riscv_set_reg(RISCVCPUState *s, int rn, uint64_t val)
{
    assert(0 < rn && rn < 32);
    s->reg[rn] = val;
}

void riscv_dump_regs(RISCVCPUState *s)
{
    dump_regs(s);
}

int riscv_read_insn(RISCVCPUState *s, uint32_t *insn, uint64_t addr)
{
    //uintptr_t mem_addend;

    int i = target_read_insn_slow(s, insn, 32, addr);
    if (i)
        return i;

    //*insn = *(uint32_t *)(mem_addend + addr);

    return 0;
}

int riscv_read_u64(RISCVCPUState *s, uint64_t *data, uint64_t addr)
{
    *data = phys_read_u64(s, addr);
    fprintf(stderr, "data:0x%" PRIx64 " addr:0x%08" PRIx64 "\n", *data, addr);
    int i = 0;
    if (i) {
        fprintf(stderr, "Illegal read addr:%"  PRIx64 "\n", addr);
        return i;
    }

    return 0;
}

uint32_t riscv_cpu_get_misa(RISCVCPUState *s)
{
    return s->misa;
}

int riscv_get_priv_level(RISCVCPUState *s)
{
    return s->priv;
}

int riscv_get_most_recently_written_reg(RISCVCPUState *s,
                                        uint64_t *instret_ts)
{
    int regno = s->most_recently_written_reg;
    if (instret_ts)
        *instret_ts = s->reg_ts[regno];

    return regno;
}

int riscv_get_most_recently_written_fp_reg(RISCVCPUState *s,
                                           uint64_t *instret_ts)
{
    int regno = s->most_recently_written_fp_reg;
    if (instret_ts)
        *instret_ts = s->fp_reg_ts[regno];

    return regno;
}

void riscv_get_ctf_info(RISCVCPUState *s, RISCVCTFInfo *info)
{
    *info = s->info;
}

void riscv_get_ctf_target(RISCVCPUState *s, uint64_t *target)
{
    *target = s->next_addr;
}

BOOL riscv_terminated(RISCVCPUState *s)
{
    return s->terminate_simulation;
}

static void serialize_memory(const void *base, size_t size, const char *file)
{
    int f_fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0777);

    if (f_fd < 0)
        err(-3, "trying to write %s", file);

    while (size) {
        ssize_t written = write(f_fd, base, size);
        if (written <= 0)
            err(-3, "while writing %s", file);
        size -= written;
    }

    close(f_fd);
}

static void deserialize_memory(void *base, size_t size, const char *file)
{
    int f_fd = open(file, O_RDONLY);

    if (f_fd < 0)
        err(-3, "trying to read %s", file);

    ssize_t sz = read(f_fd, base, size);

    if (sz != size)
        err(-3, "%s %zd size does not match memory size %zd", file, sz, size);

    close(f_fd);
}

static uint32_t create_csrrw(int rs, uint32_t csrn)
{
    return 0x1073 | ((csrn & 0xFFF) << 20) | ((rs & 0x1F) << 15);
}

static uint32_t create_csrrs(int rd, uint32_t csrn)
{
    return 0x2073 | ((csrn & 0xFFF) << 20) | ((rd & 0x1F) << 7);
}

static uint32_t create_auipc(int rd, uint32_t addr)
{
    if (addr & 0x800)
        addr += 0x800;

    return 0x17 | ((rd & 0x1F) << 7) | ((addr >> 12) << 12);
}

static uint32_t create_addi(int rd, uint32_t addr)
{
    uint32_t pos = addr & 0xFFF;

    return 0x13 | ((rd & 0x1F)<<7) | ((rd & 0x1F)<<15) | ((pos & 0xFFF)<<20);
}

static uint32_t create_seti(int rd, uint32_t data)
{
    return 0x13 | ((rd & 0x1F)<<7) | ((data & 0xFFF)<<20);
}

static uint32_t create_ld(int rd, int rs1)
{
    return 0x3 | ((rd & 0x1F)<<7) | (0x3<<12) | ((rs1 & 0x1F)<<15);
}

static uint32_t create_sd(int rs1, int rs2)
{
    return 0x23 | ((rs2 & 0x1F)<<20) | (0x3<<12) | ((rs1 & 0x1F)<<15);
}

static uint32_t create_fld(int rd, int rs1)
{
    return 0x7 | ((rd & 0x1F)<<7) | (0x3<<12) | ((rs1 & 0x1F)<<15);
}

static void create_csr12_recovery(uint32_t *rom, uint32_t *code_pos, uint32_t csrn, uint16_t val)
{
  rom[(*code_pos)++] = create_seti(1,  val & 0xFFF);
  rom[(*code_pos)++] = create_csrrw(1,  csrn);
}

static void create_csr64_recovery(uint32_t *rom, uint32_t *code_pos, uint32_t *data_pos, uint32_t csrn, uint64_t val)
{
    uint32_t data_off = sizeof(uint32_t) * (*data_pos - *code_pos);

    rom[(*code_pos)++] = create_auipc(1, data_off);
    rom[(*code_pos)++] = create_addi(1, data_off);
    rom[(*code_pos)++] = create_ld(1, 1);
    rom[(*code_pos)++] = create_csrrw(1, csrn);

    rom[(*data_pos)++] = val & 0xFFFFFFFF;
    rom[(*data_pos)++] = val >> 32;
}

static void create_reg_recovery(uint32_t *rom, uint32_t *code_pos, uint32_t *data_pos, int rn, uint64_t val)
{
    uint32_t data_off = sizeof(uint32_t) * (*data_pos - *code_pos);

    rom[(*code_pos)++] = create_auipc(rn, data_off);
    rom[(*code_pos)++] = create_addi(rn, data_off);
    rom[(*code_pos)++] = create_ld(rn, rn);

    rom[(*data_pos)++] = val & 0xFFFFFFFF;
    rom[(*data_pos)++] = val >> 32;
}

static void create_io64_recovery(uint32_t *rom, uint32_t *code_pos, uint32_t *data_pos, uint64_t addr, uint64_t val)
{
    uint32_t data_off = sizeof(uint32_t) * (*data_pos - *code_pos);

    rom[(*code_pos)++] = create_auipc(1, data_off);
    rom[(*code_pos)++] = create_addi(1, data_off);
    rom[(*code_pos)++] = create_ld(1, 1);

    rom[(*data_pos)++] = addr & 0xFFFFFFFF;
    rom[(*data_pos)++] = addr >> 32;

    uint32_t data_off2 = sizeof(uint32_t) * (*data_pos - *code_pos);
    rom[(*code_pos)++] = create_auipc(2, data_off2);
    rom[(*code_pos)++] = create_addi(2, data_off2);
    rom[(*code_pos)++] = create_ld(2, 2);

    rom[(*code_pos)++] = create_sd(1, 2);

    rom[(*data_pos)++] = val & 0xFFFFFFFF;
    rom[(*data_pos)++] = val >> 32;
}

static void create_boot_rom(RISCVCPUState *s, RISCVMachine *m, const char *file)
{
    uint32_t rom[ROM_SIZE/4];
    memset(rom, 0, ROM_SIZE);

    // ROM organization
    // 0..40 wasted
    // 40..0x800 boot code
    // 0x800..0x1000 boot data

    uint32_t code_pos = (BOOT_BASE_ADDR - ROM_BASE_ADDR) / sizeof(uint32_t);
    uint32_t data_pos = ROM_SIZE / 2 / sizeof(uint32_t);
    uint32_t data_pos_start = data_pos;

    create_csr64_recovery(rom, &code_pos, &data_pos, 0x7b1, s->pc); // Write to DPC (CSR, 0x7b1)

    // Write current priviliege level to prv in dcsr (0 user, 1 supervisor, 2 user)
    // dcsr is at 0x7b0 prv is bits 0 & 1
    // dcsr.stopcount = 1
    // dcsr.stoptime  = 1
    // dcsr = 0x600 | (PrivLevel & 0x3)
    int prv = 0;
    if (s->priv == PRV_U)
        prv = 0;
    else if (s->priv == PRV_S)
        prv = 1;
    else if (s->priv == PRV_M)
        prv = 3;
    else {
        fprintf(stderr, "UNSUPORTED Priv mode (no hyper)\n");
        exit(-4);
    }

    create_csr12_recovery(rom, &code_pos, 0x7b0, 0x600|prv);

    // NOTE: mstatus & misa should be one of the first because risvemu breaks down this
    // register for performance reasons. E.g: restoring the fflags also changes
    // parts of the mstats
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x300, get_mstatus(s, (target_ulong)-1)); // mstatus
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x301, s->misa | ((target_ulong)s->mxl << (s->cur_xlen - 2))); // misa

    // All the remaining CSRs
    if (s->fs) { // If the FPU is down, you can not recover flags
      create_csr12_recovery(rom, &code_pos, 0x001, s->fflags);
      // Only if fflags, otherwise it would raise an illegal instruction
      create_csr12_recovery(rom, &code_pos, 0x002, s->frm);
      create_csr12_recovery(rom, &code_pos, 0x003, s->fflags | (s->frm<<5));

      // do the FP registers, iff fs is set
      for (int i = 0; i < 32; i++) {
        uint32_t data_off = sizeof(uint32_t) * (data_pos - code_pos);
        rom[code_pos++] = create_auipc(1, data_off);
        rom[code_pos++] = create_addi(1, data_off);
        rom[code_pos++] = create_fld(i, 1);

        rom[data_pos++] = (uint32_t)s->fp_reg[i];
        rom[data_pos++] = (uint64_t)s->reg[i] >> 32;
      }
    }


  // Recover CPU CSRs

    // Cycle and instruction are alias across modes. Just write to m-mode counter
    // Already done before CLINT. create_csr64_recovery(rom, &code_pos, &data_pos, 0xb00, s->insn_counter); // mcycle
    //create_csr64_recovery(rom, &code_pos, &data_pos, 0xb02, s->insn_counter); // instret

    for(int i = 3; i < 32 ; i++ ) {
      create_csr12_recovery(rom, &code_pos, 0xb00 + i, 0); // reset mhpmcounter3..31
      create_csr64_recovery(rom, &code_pos, &data_pos, 0x320 + i, s->mhpmevent[i]); // mhpmevent3..31
    }
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x7a0, s->tselect); // tselect
    //FIXME: create_csr64_recovery(rom, &code_pos, &data_pos, 0x7a1, s->tdata1); // tdata1
    //FIXME: create_csr64_recovery(rom, &code_pos, &data_pos, 0x7a2, s->tdata2); // tdata2
    //FIXME: create_csr64_recovery(rom, &code_pos, &data_pos, 0x7a3, s->tdata3); // tdata3

    create_csr64_recovery(rom, &code_pos, &data_pos, 0x302, s->medeleg);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x303, s->mideleg);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x304, s->mie);  // mie & sie
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x305, s->mtvec);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x105, s->stvec);
    create_csr12_recovery(rom, &code_pos, 0x306, s->mcounteren);
    create_csr12_recovery(rom, &code_pos, 0x106, s->scounteren);

    // NOTE: no pmp (pmpcfg0). Not implemented in RTL

    create_csr64_recovery(rom, &code_pos, &data_pos, 0x340, s->mscratch);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x341, s->mepc);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x342, s->mcause);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x343, s->mtval);

    create_csr64_recovery(rom, &code_pos, &data_pos, 0x140, s->sscratch);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x141, s->sepc);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x142, s->scause);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x143, s->stval);

    create_csr64_recovery(rom, &code_pos, &data_pos, 0x344, s->mip); // mip & sip

    for (int i = 3; i < 32; i++) { // Not 1 and 2 which are used by create_...
      create_reg_recovery(rom, &code_pos, &data_pos, i, s->reg[i]);
    }

    // Recover CLINT (Close to the end of the recovery to avoid extra cycles)
    // TODO: One per hart (multicore/SMP)

    fprintf(stderr, "clint hart0 timecmp=%lld cycles (%lld)\n", (long long)m->timecmp, (long long)riscv_cpu_get_cycles(s)/RTC_FREQ_DIV);
    create_io64_recovery(rom, &code_pos, &data_pos, CLINT_BASE_ADDR + 0x4000, m->timecmp); // Assuming 16 ratio between CPU and CLINT and that CPU is reset to zero

    create_csr64_recovery(rom, &code_pos, &data_pos, 0xb02, s->minstret);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0xb00, s->mcycle);

    create_io64_recovery(rom, &code_pos, &data_pos, CLINT_BASE_ADDR + 0xbff8, s->mcycle/RTC_FREQ_DIV);

    for (int i = 1; i < 3; i++) { // recover 1 and 2 now
      create_reg_recovery(rom, &code_pos, &data_pos, i, s->reg[i]);
    }

    rom[code_pos++] = create_csrrw(1, 0x7b2);
    create_csr64_recovery(rom, &code_pos, &data_pos, 0x180, s->satp);
    // last Thing because it changes addresses. Use dscratch register to remember reg 1
    rom[code_pos++] = create_csrrs(1, 0x7b2);

    // dret 0x7b200073
    rom[code_pos++] = 0x7b200073;

    if (data_pos >= (ROM_SIZE / sizeof(uint32_t)) || code_pos >= data_pos_start) {
        fprintf(stderr, "ERROR: rom is too small. ROM_SIZE should increase. Current code_pos=%d data_pos=%d\n",
                code_pos,data_pos);
        exit(-6);
    }

    serialize_memory(rom, ROM_SIZE, file);
}

void riscv_cpu_serialize(RISCVCPUState *s, RISCVMachine *m, const char *dump_name)
{
    FILE *conf_fd = 0;
    size_t n = strlen(dump_name) + 64;
    char *conf_name = alloca(n);
    snprintf(conf_name, n, "%s.re_regs", dump_name);

    conf_fd = fopen(conf_name, "w");
    if (conf_fd == 0)
        err(-3, "opening %s for serialization", conf_name);

    fprintf(conf_fd, "# RISCVEMU serialization file\n");

    fprintf(conf_fd, "pc:0x%llx\n", (long long)s->pc);

    for (int i = 1; i < 32; i++) {
        fprintf(conf_fd, "reg_x%d:%llx\n", i, (long long)s->reg[i]);
    }

#if LEN > 0
    for (int i = 0; i < 32; i++) {
        fprintf(conf_fd, "reg_f%d:%llx\n", i, (long long)s->fp_reg[i]);
    }
    fprintf(conf_fd, "fflags:%c\n", s->fflags);
    fprintf(conf_fd, "frm:%c\n", s->frm);
#endif

    const char priv_str[4] = "USHM";
    fprintf(conf_fd, "priv:%c\n", priv_str[s->priv]);
    fprintf(conf_fd, "insn_counter:%"PRIu64"\n", s->insn_counter);

    fprintf(conf_fd, "pending_exception:%d\n", s->pending_exception);

    fprintf(conf_fd, "mstatus:%llx\n", (unsigned long long)s->mstatus);
    fprintf(conf_fd, "mtvec:%llx\n", (unsigned long long)s->mtvec);
    fprintf(conf_fd, "mscratch:%llx\n", (unsigned long long)s->mscratch);
    fprintf(conf_fd, "mepc:%llx\n", (unsigned long long)s->mepc);
    fprintf(conf_fd, "mcause:%llx\n", (unsigned long long)s->mcause);
    fprintf(conf_fd, "mtval:%llx\n", (unsigned long long)s->mtval);

    fprintf(conf_fd, "misa:%" PRIu32 "\n", s->misa);
    fprintf(conf_fd, "mie:%" PRIu32 "\n", s->mie);
    fprintf(conf_fd, "mip:%" PRIu32 "\n", s->mip);
    fprintf(conf_fd, "medeleg:%" PRIu32 "\n", s->medeleg);
    fprintf(conf_fd, "mideleg:%" PRIu32 "\n", s->mideleg);
    fprintf(conf_fd, "mcounteren:%" PRIu32 "\n", s->mcounteren);
    fprintf(conf_fd, "tselect:%" PRIu32 "\n", s->tselect);

    fprintf(conf_fd, "stvec:%llx\n", (unsigned long long)s->stvec);
    fprintf(conf_fd, "sscratch:%llx\n", (unsigned long long)s->sscratch);
    fprintf(conf_fd, "sepc:%llx\n", (unsigned long long)s->sepc);
    fprintf(conf_fd, "scause:%llx\n", (unsigned long long)s->scause);
    fprintf(conf_fd, "stval:%llx\n", (unsigned long long)s->stval);
    fprintf(conf_fd, "satp:%llx\n", (unsigned long long)s->satp);
    fprintf(conf_fd, "scounteren:%llx\n", (unsigned long long)s->scounteren);

    PhysMemoryRange *boot_ram = 0;
    int main_ram_found = 0;

    for (int i = s->mem_map->n_phys_mem_range-1; i >= 0; --i) {
        PhysMemoryRange *pr = &s->mem_map->phys_mem_range[i];
        fprintf(conf_fd, "mrange%d:0x%llx 0x%llx %s\n", i,
                (long long)pr->addr, (long long)pr->size,
                pr->is_ram ? "ram" : "io");

        if (pr->is_ram && pr->addr == ROM_BASE_ADDR) {

            assert(!boot_ram);
            boot_ram = pr;

        } else if (pr->is_ram && pr->addr == RAM_BASE_ADDR) {

            assert(!main_ram_found);
            main_ram_found = 1;

            char *f_name = alloca(strlen(dump_name)+64);
            sprintf(f_name, "%s.mainram", dump_name);

            serialize_memory(pr->phys_mem, pr->size, f_name);
        }
    }

    if (!boot_ram || !main_ram_found) {
        fprintf(stderr, "ERROR: could not find boot and main ram???\n");
        exit(-3);
    }

    n = strlen(dump_name) + 64;
    char *f_name = alloca(n);
    snprintf(f_name, n, "%s.bootram", dump_name);

    if (s->priv != 3 || ROM_BASE_ADDR + ROM_SIZE < s->pc) {
        fprintf(stderr, "NOTE: creating a new boot rom\n");
        create_boot_rom(s, m, f_name);
    } else if (BOOT_BASE_ADDR < s->pc) {
        fprintf(stderr, "ERROR: could not checkpoint when running inside the ROM\n");
        exit(-4);
    } else if (s->pc == BOOT_BASE_ADDR && boot_ram) {
        fprintf(stderr, "NOTE: using the default riscvemu room\n");
        serialize_memory(boot_ram->phys_mem, boot_ram->size, f_name);
    } else {
        fprintf(stderr, "ERROR: unexpected PC address 0x%llx\n", (long long)s->pc);
        exit(-4);
    }
}

void riscv_cpu_deserialize(RISCVCPUState *s, const char *dump_name)
{
    for (int i = s->mem_map->n_phys_mem_range - 1; i >= 0; --i) {
        PhysMemoryRange *pr = &s->mem_map->phys_mem_range[i];

        if (pr->is_ram && pr->addr == ROM_BASE_ADDR) {

            size_t n = strlen(dump_name) + 64;
            char *boot_name = alloca(n);
            snprintf(boot_name, n, "%s.bootram", dump_name);

            deserialize_memory(pr->phys_mem, pr->size, boot_name);

        } else if (pr->is_ram && pr->addr == RAM_BASE_ADDR) {

            size_t n = strlen(dump_name) + 64;
            char *main_name = alloca(n);
            snprintf(main_name, n, "%s.mainram", dump_name);

            deserialize_memory(pr->phys_mem, pr->size, main_name);
        }
    }
}
