/*
 * COREMU Parallel Emulator Framework
 *
 * Atomic support for COREMU system.
 * XXX: Now only support x86-64 architecture.
 *
 * Copyright (C) 2010 Parallel Processing Institute (PPI), Fudan Univ.
 *  <http://ppi.fudan.edu.cn/system_research_group>
 *
 * Authors:
 *  Zhaoguo Wang    <zgwang@fudan.edu.cn>
 *  Yufei Chen      <chenyufei@fudan.edu.cn>
 *  Ran Liu         <naruilone@gmail.com>
 *  Xi Wu           <wuxi@fudan.edu.cn>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _COREMU_ATOMIC_H
#define _COREMU_ATOMIC_H

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include "config-target.h"

/* Given the guest virtual address, get the corresponding host address.
 * This macro resembles ldxxx in softmmu_template.h
 * NOTE: This must be inlined since the use of GETPC needs to get the
 * return address. Using always inline also works, we use macro here to be more
 * explicit. */
#if defined(CONFIG_USER_ONLY)
#define CM_GET_QEMU_ADDR(q_addr, v_addr) \
do {					 \
    q_addr = v_addr + GUEST_BASE;	 \
} while (0)

#else
#define CM_GET_QEMU_ADDR(q_addr, v_addr) \
do {                                                                        \
    int __mmu_idx, __index;                                                 \
    CPUState *__env1 = cpu_single_env;                                      \
    void *__retaddr;                                                        \
    __index = (v_addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);            \
    /* get the CPL, hence determine the MMU mode */                         \
    __mmu_idx = cpu_mmu_index(__env1);                                      \
    /* We use this function in the implementation of atomic instructions */ \
    /* and we are going to modify these memory. So we use addr_write. */    \
    if (unlikely(__env1->tlb_table[__mmu_idx][__index].addr_write           \
                != (v_addr & TARGET_PAGE_MASK))) {                          \
        __retaddr = GETPC();                                                \
        tlb_fill(v_addr, 1, __mmu_idx, __retaddr);                          \
    }                                                                       \
    q_addr = v_addr + __env1->tlb_table[__mmu_idx][__index].addend;         \
} while(0)
#endif

/* XXX These are also used by atomic instruction handling.
 * Put these defines in some other files? */
#define DATA_b uint8_t
#define DATA_w uint16_t
#define DATA_l uint32_t
#define DATA_q uint64_t

#define __inline__ inline __attribute__((always_inline))

#if defined(__i386__) || defined(__x86_64__)
// Is this the correct way to detect 64 system?
#if defined(_LP64)
static __inline__ uint8_t
atomic_compare_exchange16b(uint64_t *memp,
                           uint64_t rax, uint64_t rdx,
                           uint64_t rbx, uint64_t rcx)
{
    uint8_t z;
    __asm __volatile__ ( "lock; cmpxchg16b %3\n\t"
                         "setz %2\n\t"
                         : "=a" (rax), "=d" (rdx), "=r" (z), "+m" (*memp)
                         : "a" (rax), "d" (rdx), "b" (rbx), "c" (rcx)
                         : "memory", "cc" );
    return z;
}
#else
static __inline__ uint8_t
atomic_compare_exchange16b(uint64_t *memp,
                           uint64_t rax, uint64_t rdx,
                           uint64_t rbx, uint64_t rcx)
{
    assert("atomic_compare_exchange16b: not supported.\n");
    exit(0);
}

static __inline__ uint8_t
atomic_compare_exchangeq(uint64_t *addr,
		uint64_t oldval, uint64_t newval)
{
    assert("atomic_compare_exchangeq: not supported.\n");
    exit(0);
}

#endif

/* Memory Barriers: x86-64 ONLY now */
#define mb()    asm volatile("mfence":::"memory")
#define rmb()   asm volatile("lfence":::"memory")
#define wmb()   asm volatile("sfence" ::: "memory")

#define LOCK_PREFIX "lock; "

#define coremu_xglue(a, b) a ## b
// If a/b is macro, it will expand first, then pass to coremu_xglue
#define coremu_glue(a, b) coremu_xglue(a, b)

#define coremu_xstr(s) # s
#define coremu_str(s) coremu_xstr(s)

#define DATA_BITS 8
#include "coremu-template.h"

#define DATA_BITS 16
#include "coremu-template.h"

#define DATA_BITS 32
#include "coremu-template.h"

#if defined(_LP64)
#define DATA_BITS 64
#include "coremu-template.h"
#else
static inline uint64_t atomic_exchangeq(uint64_t *p, uint64_t val)
{
    assert("atomic_exchangeq: not supported.\n");
    exit(0);
}

#endif

#elif defined(__arm__)

#if defined(__ARM_ARCH_7__)   || \
    defined(__ARM_ARCH_7A__)  || \
    defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_7M__)  || \
    defined(__ARM_ARCH_7R__)  || \
    defined(__ARM_ARCH_6J__)  || \
    defined(__ARM_ARCH_6K__)  || \
    defined(__ARM_ARCH_6T2__) || \
    defined(__ARM_ARCH_6Z__)  || \
    defined(__ARM_ARCH_6ZK__)
#define USE_ARMV6_INSTRUCTIONS
#endif

#ifdef USE_ARMV6_INSTRUCTIONS
#define mb()	__asm__ __volatile__("dmb" : : : "memory")
#define raw_local_irq_save(x)                                   \
        ({                                                      \
        __asm__ __volatile__(                                   \
        "mrs    %0, cpsr                @ local_irq_save\n"     \
        "cpsid  i"                                              \
        : "=r" (x) : : "memory", "cc");                         \
        })
#else
#define mb()    __asm__ __volatile__("":::"memory")
#define raw_local_irq_save(x)                                   \
        ({                                                      \
                unsigned long temp;                             \
                (void) (&temp == &x);                           \
        __asm__ __volatile__(                                   \
        "mrs    %0, cpsr                @ local_irq_save\n"     \
"       orr     %1, %0, #128\n"                                 \
"       msr     cpsr_c, %1"                                     \
        : "=r" (x), "=r" (temp)                                 \
        :                                                       \
        : "memory", "cc");                                      \
        })
#endif

#define raw_local_irq_restore(x)                                \
	__asm__ __volatile(                                     \
	"msr    cpsr_c, %0              @ local_irq_restore\n"  \
	:                                                       \
	: "r" (x)                                               \
	: "memory", "cc")

static __inline__ uint8_t atomic_compare_exchangeb(uint8_t *addr,
        uint8_t oldval, uint8_t newval)
{
    uint8_t ret;
#ifdef USE_ARMV6_INSTRUCTIONS
    unsigned long tmp;
    __asm__ __volatile__("@ atomic_cmpxchgl\n"
    "1:     ldrexb  %1, [%3]\n"
    "       mov    %0, #0\n"
    "       teq    %1, %4\n"
    "       strexbeq %0, %5, [%3]\n"
    "       teq    %0, #0\n"
    "       bne    1b\n"
            : "=&r" (tmp), "=&r" (ret), "+Qo" (*addr)
            : "r" (addr), "Ir" (oldval), "r" (newval)
            : "cc");
#else
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *addr;
    if (likely(ret == oldval))
        *addr = newval;
    raw_local_irq_restore(flags);
#endif
    return ret;
}

static __inline__ uint16_t atomic_compare_exchangew(uint16_t *addr,
        uint16_t oldval, uint16_t newval)
{
    uint16_t ret;
#ifdef USE_ARMV6_INSTRUCTIONS
    unsigned long tmp;
    __asm__ __volatile__("@ atomic_cmpxchgl\n"
    "1:     ldrexh  %1, [%3]\n"
    "       mov    %0, #0\n"
    "       teq    %1, %4\n"
    "       strexheq %0, %5, [%3]\n"
    "       teq    %0, #0\n"
    "       bne    1b\n"
            : "=&r" (tmp), "=&r" (ret), "+Qo" (*addr)
            : "r" (addr), "Ir" (oldval), "r" (newval)
            : "cc");
#else
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *addr;
    if (likely(ret == oldval))
        *addr = newval;
    raw_local_irq_restore(flags);
#endif
    return ret;
}

static __inline__ uint32_t atomic_compare_exchangel(uint32_t *addr,
        uint32_t oldval, uint32_t newval)
{
    uint32_t ret;
#ifdef USE_ARMV6_INSTRUCTIONS
    unsigned long tmp;
    __asm__ __volatile__("@ atomic_cmpxchgl\n"
    "1:     ldrex  %1, [%3]\n"
    "       mov    %0, #0\n"
    "       teq    %1, %4\n"
    "       strexeq %0, %5, [%3]\n"
    "       teq    %0, #0\n"
    "       bne    1b\n"
            : "=&r" (tmp), "=&r" (ret), "+Qo" (*addr)
            : "r" (addr), "Ir" (oldval), "r" (newval)
            : "cc");
#else
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *addr;
    if (likely(ret == oldval))
        *addr = newval;
    raw_local_irq_restore(flags);
#endif
    return ret;
}

static __inline__ uint64_t atomic_compare_exchangeq(uint64_t *addr,
        uint64_t oldval, uint64_t newval)
{
    uint64_t ret;
#ifdef USE_ARMV6_INSTRUCTIONS
    unsigned long tmp;
    __asm__ __volatile__("@ atomic_cmpxchgl\n"
    "1:     ldrexd  %1, %H1, [%3]\n"
    "       mov    %0, #0\n"
    "       teq    %1, %4\n"
    "       teqeq  %H1, %H4\n"
    "       strexdeq %0, %5, %H5, [%3]\n"
    "       teq    %0, #0\n"
    "       bne    1b\n"
            : "=&r" (tmp), "=&r" (ret), "+Qo" (*addr)
            : "r" (addr), "Ir" (oldval), "r" (newval)
            : "cc");
#else
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *addr;
    if (likely(ret == oldval))
        *addr = newval;
    raw_local_irq_restore(flags);
#endif
    return ret;
}

static __inline__ uint8_t
atomic_compare_exchange16b(uint64_t *memp,
                           uint64_t old_less, uint64_t old_most,
                           uint64_t new_less, uint64_t new_most)
{
    uint8_t ret = 0;
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *memp;
    if (likely(*memp == old_less && *(memp+1) == old_most))
    {
        *memp = new_less;
	*(memp+1) = new_most;
	ret = 1;
    }
    raw_local_irq_restore(flags);
    return ret;
}

static __inline__ unsigned long __xchg(unsigned long x, volatile void *ptr, int size)
{
    unsigned long ret;
#ifdef USE_ARMV6_INSTRUCTIONS
    unsigned int tmp;
#endif

    mb();

    switch (size) {
#ifdef USE_ARMV6_INSTRUCTIONS
        case 1:
		__asm __volatile("@	__xchg1\n"
		"1:	ldrexb	%0, [%3]\n"
		"	strexb	%1, %2, [%3]\n"
		"	teq	%1, #0\n"
		"	bne	1b"
			: "=&r" (ret), "=&r" (tmp)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
        case 2:
		__asm __volatile("@	__xchg1\n"
		"1:	ldrexh	%0, [%3]\n"
		"	strexh	%1, %2, [%3]\n"
		"	teq	%1, #0\n"
		"	bne	1b"
			: "=&r" (ret), "=&r" (tmp)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
	case 4:
		__asm __volatile("@	__xchg4\n"
		"1:	ldrex	%0, [%3]\n"
		"	strex	%1, %2, [%3]\n"
		"	teq	%1, #0\n"
		"	bne	1b"
			: "=&r" (ret), "=&r" (tmp)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
#else
	case 1:
		__asm __volatile("@	__xchg1\n"
		"	swpb	%0, %1, [%2]"
			: "=&r" (ret)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;

	case 4:
		__asm __volatile("@	__xchg4\n"
		"	swp	%0, %1, [%2]"
			: "=&r" (ret)
			: "r" (x), "r" (ptr)
			: "memory", "cc");
		break;
	case 2:
		{
    		unsigned long flags = 0;
		raw_local_irq_save(flags);
		ret = *(volatile uint16_t *)ptr;
		*(volatile uint16_t *)ptr = x;
		raw_local_irq_restore(flags);
		break;
		}

#endif
	default:
		exit(0);
    }
    mb();

    return ret;
}

#define xchg(ptr,x) ((__typeof__(*(ptr)))__xchg((unsigned long)(x),(ptr),sizeof(*(ptr))))
#define GEN_ATOMIC_XCHG_HELPER(TYPE) \
static __inline__ DATA_##TYPE atomic_exchange##TYPE(DATA_##TYPE *p, DATA_##TYPE val) { return xchg(p, val); }

GEN_ATOMIC_XCHG_HELPER(b);
GEN_ATOMIC_XCHG_HELPER(w);
GEN_ATOMIC_XCHG_HELPER(l);

#endif

#endif /* _COREMU_ATOMIC_H */

