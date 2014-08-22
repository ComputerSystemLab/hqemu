/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 *
 *   This file implements the basic optimization schemes including indirect
 *   branch target cache (IBTC), indirect branch chain (IB chain), and trace
 *   profiling and prediction routines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include "exec-all.h"
#include "disas.h"
#include "tcg-op.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

#include "cpu-all.h"
#include "optimization.h"
#include "llvm-wrapper.h"

#if defined(TCG_TARGET_I386)
#if defined(__i386__)
#define AREG0 "ebp"
#elif defined(__x86_64__)
#define AREG0 "r14"
#endif
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
#define AREG0 "r27"
#elif defined(TCG_TARGET_ARM)
#define AREG0 "r7"
#else
#error "unsupported processor type"
#endif

/*
 * `exec_env' is used to hold the base pointer to CPUState.
 * The pointer is stored in a global variable instead of pinning to a register
 * (as QEMU does) for llvm-2.8 and llvm-2.9 because the tool `llvm-link' of
 * these two LLVM version fails to combine the same global register defined in
 * multiple bitcode files. Client/Server mode also does not support global
 * register because the server-side compiler may not recognize the register
 * which belongs to the client architecture only. For other condition, the
 * CPUState pointer is pinned to a global register.
 */
#if defined(CONFIG_LLVM) && \
    (defined(CONFIG_NET) || defined(LLVM_V28) || defined(LLVM_V29))
#define USE_INLINEASM_ENV  0
__thread CPUState *exec_env;
#else
#define USE_INLINEASM_ENV  1
register CPUState *exec_env asm(AREG0);
#endif

const char *basereg = AREG0;    /* The base register name */
int llvm_flush;                 /* True is the optimized code cache is issued a
                                   flush */
pthread_mutex_t qemu_log_lock = PTHREAD_MUTEX_INITIALIZER;  /* Debug lock */
uint8_t *ibchain_gen_ptr;       /* Code cache pointer for emitting IB chain */
uint8_t *ibchain_gen_end;       /* End pointer of IB chain code cache */
llvm_lock_t ibchain_lock;       /* Lock to emit IB chain */
extern uint8_t *ibtc_ret_addr;  /* Epilog address of IBTC exit */ 
extern uint8_t *ibtc_helper_entry;  /* The address of the IBTC helper
                                       function's stub */

/*
 * ibtc_init()
 *  Initialize IBTC hash table structure.
 */
static inline void ibtc_init(CPUState *env)
{
    struct ibtc_table *ibtc = (struct ibtc_table *)
        llvm_malloc(sizeof(struct ibtc_table));
    memset(ibtc, 0, sizeof(struct ibtc_table));
    env->update_ibtc = 0;
    env->cached_eip = 0;

    env->ibtc = ibtc;
    
    if (ibchain_gen_ptr == NULL)
    {
        llvm_lock_init(&ibchain_lock);
        ibchain_gen_ptr = qemu_get_ibchain_address();
        ibchain_gen_end = ibchain_gen_ptr + qemu_get_cache_size() / 32;
    }
}

/*
 * ibtc_finalize()
 *  Destroy IBTC hash table.
 */
static inline void ibtc_finalize(CPUState *env)
{
    llvm_free(env->ibtc);
}

/*
 * ibtc_reset()
 *  Reset IBTC hash table.
 */
static inline void ibtc_reset(CPUState *env)
{
    memset(env->ibtc, 0, sizeof(struct ibtc_table));

    env->update_ibtc = 0;
    env->cached_eip = 0;
}

/*
 * update_ibtc_entry()
 *  Update one IBTC hash table entry.
 */
static inline void update_ibtc_table(CPUState *env, TranslationBlock *tb)
{
    struct jmp_pair *hash_entry;

    hash_entry = &env->ibtc->htable[env->cached_eip & IBTC_CACHE_MASK];
    hash_entry->eip = env->cached_eip;
    hash_entry->tb = tb;
}

/*
 * update_ibtc_entry()
 *  Update IB chain or IBTC hash table.
 */
void update_ibtc_entry(CPUState *env, TranslationBlock *tb)
{
    if (env->update_ibtc == 1) /* exit from IBTC hash table */
    {
        update_ibtc_table(env, tb);
        goto done;
    }

#if defined(ENABLE_IBCHAIN)
    unsigned long key = (unsigned long)env->cached_tb;

    llvm_spin_lock(&ibchain_lock);
    if ((key & 3) == 0x3) /* exit from trace */
    {
        llvm_update_ibtc_entry((int)(key >> 2), tb);
    }
    else    /* exit from block cache */
    {
        int size, num;
        uint8_t *chain;
        uint16_t patch_offset;
        TranslationBlock *last_tb = (TranslationBlock *)key;
        struct ibchain_info *ibchain = &last_tb->ibchain;

        num = ibchain->num_chain;
        if (num == 0)
        {
            ibchain->chain = (uint8_t **)
                llvm_malloc(IBCHAIN_MAX_SIZE * sizeof(uint8_t*));
            ibchain->patch_offset = (uint16_t *)
                llvm_malloc(IBCHAIN_MAX_SIZE * sizeof(uint16_t));
            ibchain->chained_pc = (target_ulong *)
                llvm_malloc(IBCHAIN_MAX_SIZE * sizeof(target_ulong));
        }
        else
        {
            int i;
            target_ulong *chained_pc = ibchain->chained_pc;
            target_ulong next_pc = tb->pc;
            for (i = 0; i < num; i++)
            {
                if (next_pc == chained_pc[i])
                {
                    llvm_spin_unlock(&ibchain_lock);
                    goto done;
                }
            }
        }

        if (num == IBCHAIN_MAX_SIZE)
        {
            fprintf(stderr, "%s: internal error on chain size.\n", __func__);
            exit(0);
        }
        if (ibchain_gen_end - ibchain_gen_ptr < 128)
        {
            fprintf(stderr, "%s: internal error on cache size.\n", __func__);
            exit(0);
        }

        chain = ibchain_gen_ptr;
        size = tcg_out_ibchain(ibchain_gen_ptr, tb, &patch_offset);
        ibchain_gen_ptr = (void *)(((uintptr_t)ibchain_gen_ptr + size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));

        flush_icache_range((unsigned long)chain, (unsigned long)chain + size);

        ibchain->chain[num] = chain;
        ibchain->patch_offset[num] = patch_offset;
        ibchain->chained_pc[num] = tb->pc;
        ibchain->num_chain = num + 1;

#ifdef IBCHAIN_INSERT_TAIL 
        uint8_t *patch_addr;
        if (num == IBCHAIN_MAX_SIZE - 1)
        {
            /* If this chain reaches its maximum length, force the IB target
             * translation to look up the global hash table. */
            patch_jmp((uintptr_t)chain + patch_offset,
                    (uintptr_t)ibtc_helper_entry);
        }

        if (num == 0)
            patch_addr = last_tb->tc_ptr + ibchain->jmp_offset;
        else
            patch_addr = ibchain->chain[num-1] + ibchain->patch_offset[num-1];

        patch_jmp((uintptr_t)patch_addr, (uintptr_t)chain);
#else
        if (num == IBCHAIN_MAX_SIZE - 1)
        {
            /* If this chain reaches its maximum length, force the IB target
             * translation to look up the global hash table. */
            patch_jmp((uintptr_t)ibchain->chain[0] + ibchain->patch_offset[0],
                    (uintptr_t)ibtc_helper_entry);
        }

        if (num != 0)
            patch_jmp((uintptr_t)chain + patch_offset,
                    (uintptr_t)ibchain->chain[num-1]);

        patch_jmp((uintptr_t)last_tb->tc_ptr + ibchain->jmp_offset,
                (uintptr_t)chain);
#endif
    }
    llvm_spin_unlock(&ibchain_lock);
#endif

done:
    env->update_ibtc = 0;

#if defined(CONFIG_HYBRID)
    if (tb->mode == 0)
        tb_start_trace_profile(tb);
#endif
}

/*
 * invalidate_ibtc_entry()
 *  Invalidate one IBTC hash table entry.
 */
void invalidate_ibtc_entry(CPUState *env, target_ulong eip)
{
    struct jmp_pair *hash_entry;
    
    hash_entry = &env->ibtc->htable[eip & IBTC_CACHE_MASK];
    hash_entry->eip = 0;
    hash_entry->tb = 0;

    env->update_ibtc = 0;
}

/*
 * helper_lookup_ibchain()
 *  Helper function to lookup the IB chain. This function should never be
 *  called.
 */
void helper_lookup_ibchain(target_ulong eip, void *key)
{
    fprintf(stderr, "Fatal error: function %s cannot be called.\n", __func__);
    exit(0);
}

/*
 * helper_lookup_ibtc()
 *  Helper function to lookup the IBTC hash table.
 */
#if defined(CONFIG_SOFTMMU)
void *helper_lookup_ibtc(target_ulong eip)
{
    return ibtc_ret_addr;
}
#else
void *helper_lookup_ibtc(target_ulong eip)
{
    struct jmp_pair *hash_entry;
    target_ulong cs_base, pc;
    int flags;

    /* A match of 'eip', 'cs_base' and 'flags' results in a IBTC hit. Since
     * cs_base is only meaningful with x86 guest and system mode (cs_base is
     * always 0 for user-mode emulation and non-x86 guest), we only compare
     * cs_base with system mode emulation of x86 guest. */
    hash_entry = &exec_env->ibtc->htable[eip & IBTC_CACHE_MASK];
    if (likely(eip == hash_entry->eip))
    {
        TranslationBlock *next_tb = hash_entry->tb;
        cpu_get_tb_cpu_state(exec_env, &pc, &cs_base, &flags);

#if defined(TARGET_I386)
        if (likely(next_tb->cs_base == cs_base))
#endif
        if (likely(next_tb->flags == flags))
            return next_tb->opt_ptr;
    }

    exec_env->cached_eip = eip;
    exec_env->update_ibtc = 1;

    return ibtc_ret_addr;
}
#endif

#if defined(TARGET_SPARC)
/*
 * helper_lookup_ibtc2()
 *  Helper function to lookup the IBTC hash table.
 */
void *helper_lookup_ibtc2(target_ulong eip, target_ulong eip2)
{
    struct jmp_pair *hash_entry;
    target_ulong cs_base, pc;
    int flags;

    hash_entry = &exec_env->ibtc->htable[eip & IBTC_CACHE_MASK];
    if (likely(eip == hash_entry->eip))
    {
        TranslationBlock *next_tb = hash_entry->tb;
        cpu_get_tb_cpu_state(exec_env, &pc, &cs_base, &flags);

        if (likely(next_tb->cs_base == eip2 && next_tb->flags == flags))
            return next_tb->opt_ptr;
    }

    exec_env->cached_eip = eip;
    exec_env->update_ibtc = 1;

    return ibtc_ret_addr;
}
#endif


#if defined(CONFIG_HYBRID)
static inline void trace_formation_init(CPUState *env)
{
    env->predicted_count = 0;
    env->start_trace_prediction = 0;
    env->predicted_tb = (void *)llvm_malloc(PREDICT_THRESHOLD * sizeof(TranslationBlock **));
    
    add_trace_profile(env, PROFILE_THRESHOLD, PREDICT_THRESHOLD);
}

static inline void trace_formation_finalize(CPUState *env)
{
    llvm_free(env->predicted_tb);
}
#else
#define trace_formation_init(a)
#define trace_formation_finalize(a)
#endif

/*
 * tb_get_jmp_entry()
 *  Return the block-to-trace patch point (address which is patched a jump from
 *  the block code cache to the optimized code cache).
 */
unsigned long tb_get_jmp_entry(TranslationBlock *tb)
{
#if defined(TCG_TARGET_I386)
    return (unsigned long)tb->tc_ptr + 3;
#elif defined(TCG_TARGET_ARM)
    return (unsigned long)tb->tc_ptr + 4;
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    return (unsigned long)tb->tc_ptr;
#else
#error "unsupported processor type"
#endif
}

/*
 * tb_get_jmp_next()
 *  Return the next address the patch point will jump to initially.
 */
inline unsigned long tb_get_jmp_next(TranslationBlock *tb)
{
    return (unsigned long)tb->tc_ptr + tb->patch_next;
}

#if defined(CONFIG_HYBRID)
#if defined(CONFIG_HPM)
void tb_gen_trace_stub(CPUState *env, TranslationBlock *tb)
{
    int l1;

    if (env->gen_trace != 0)
        return;

    l1 = gen_new_label();
    tcg_gen_patch(l1);
    gen_set_label(l1);
}

inline void tb_start_trace_profile(TranslationBlock *tb)
{
}

inline void tb_start_trace_predict(TranslationBlock *tb)
{
}

#else

/*
 * tb_gen_trace_stub()
 *  Generate trace profiling and prediction stubs at the beginning of a block.
 *  The code layout is 
 *
 *       JMP <L1 or trace>
 *       ProfilingStub
 *   L1: PredictionStub
 *       TranslatedCode
 */
void tb_gen_trace_stub(CPUState *env, TranslationBlock *tb)
{
    if (env->gen_trace != 0)
        return;

    int l1, l2;
    TCGv_i32 startp;
    TCGv_ptr tmp1, tmp2;

    l1 = gen_new_label();
    l2 = gen_new_label();

    /* Patch point for block-to-trace chaining. */
    tcg_gen_patch(l1);

    /* Trace profiling stub. */
    tmp1 = tcg_const_ptr((tcg_target_long)(long)tb);
    gen_helper_trace_profile(tmp1);

    /* Initially set the patch point jumping to here. */
    gen_set_label(l1);
    tcg_temp_free_ptr(tmp1);

    /* Trace prediction stub. */
    startp = tcg_temp_new_i32();
    tmp2 = tcg_const_ptr((long)&env->start_trace_prediction);
    tcg_gen_ld_i32(startp, tmp2, 0);
    tcg_gen_brcondi_i32(TCG_COND_EQ, startp, 0, l2);
    tmp1 = tcg_const_ptr((tcg_target_long)(long)tb);
    gen_helper_trace_predict(tmp1);
    gen_set_label(l2);

    tcg_temp_free_i32(startp);
    tcg_temp_free_ptr(tmp1);
    tcg_temp_free_ptr(tmp2);
}

/*
 * tb_start_trace_profile()
 *  Start trace profiling.
 */
inline void tb_start_trace_profile(TranslationBlock *tb)
{
    if (llvm_testandset(&tb->mode) == 1)
        return;

    /* Turn on trace profiling by jumping to the next instruction. */
    unsigned long jmp_addr = tb_get_jmp_entry(tb);
#if defined(TCG_TARGET_I386)
    patch_jmp(jmp_addr, jmp_addr + 5);
#elif defined(TCG_TARGET_ARM)
    patch_jmp(jmp_addr, jmp_addr + 4);
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    patch_jmp(jmp_addr, jmp_addr + 16);
#endif
}

/*
 * tb_start_trace_predict()
 *  Start trace prediction. Stop prediction if `tb' is NULL.
 */
inline void tb_start_trace_predict(TranslationBlock *tb)
{
    helper_trace_predict(tb);
}

#endif /* CONFIG_HPM */

/*
 * helper_trace_profile()
 *  Helper function to perform trace profiling.
 */
void helper_trace_profile(void *arg)
{
    TranslationBlock *tb = (TranslationBlock *)arg;
    int ret = atomic_inc_return(&tb->exec_count);
    if (ret != PROFILE_THRESHOLD)
        return;

    /* We reach a profile threshold, stop trace profiling and start trace tail
     * prediction. The profiling is disabled by setting the jump directly to 
     * trace prediction stub. */
    patch_jmp(tb_get_jmp_entry(tb), tb_get_jmp_next(tb));

    exec_env->start_trace_prediction = 1;
}

/*
 * helper_trace_predict()
 *  Helper function to perform trace prediction. The prediction will be stopped
 *  if the input argument is NULL.
 */
void helper_trace_predict(void *arg)
{
    TranslationBlock *tb = (TranslationBlock *)arg;
    TranslationBlock **predicted_tb = (TranslationBlock **)exec_env->predicted_tb;

    if (tb == NULL)
    {
        exec_env->start_trace_prediction = 0;
        exec_env->predicted_count = 0;
        return;
    }

    /* The trace prediction will terminate if a cyclic path is detected.
     * This is, the last tb has already existed in the history list
     * (either head or middle of the list). */
    int i, loop_index = -1;
    for (i = exec_env->predicted_count-1; i >= 0; i--)
    {
        if (tb == predicted_tb[i])
        {
            loop_index = i;
            goto trace_building;
        }
    }
    
    predicted_tb[exec_env->predicted_count++] = tb;

    /* The trace prediction also terminates if it reaches the maximum
     * prediction length. */
    if (exec_env->predicted_count == PREDICT_THRESHOLD)
        goto trace_building;

    return;
    
trace_building:
    if (exec_env->predicted_count == 0)
    {
        fprintf(stderr, "%s: trace length cannot be zero\n", __func__);
        exit(0);
    }
    
    if (loop_index == -1)     /* Fall-through path. */
        llvm_gen_trace(exec_env, predicted_tb, exec_env->predicted_count, -1);
    else if (loop_index == 0) /* Loopback at the head of the list */
        llvm_gen_trace(exec_env, predicted_tb, exec_env->predicted_count, 0);
    else
    {
        /* Loopback at the middle of the list. We forms two regions for this
         * condition. One is the whole region with loopback to the middle of
         * the region. The other one is the simple loop with the loopback block
         * as the loop head.
         *
         * NOTE: Do not exclude the simple-loop trace because it will degrade
         * the performance significantly. */
        llvm_gen_trace(exec_env, &predicted_tb[loop_index], exec_env->predicted_count-loop_index, 0);
        llvm_gen_trace(exec_env, predicted_tb, exec_env->predicted_count, loop_index);
    }

    exec_env->start_trace_prediction = 0;
    exec_env->predicted_count = 0;
}

#else
void tb_gen_trace_stub(CPUState *env, TranslationBlock *tb)
{
}
void helper_trace_profile(void *arg)
{
}
void helper_trace_predict(void *arg)
{
}
#endif

/*
 * optimization_init()
 *  Initialize the optimization schemes.
 */
int optimization_init(CPUState *env)
{
    if (env->cpu_index == -1)   /* llvm optimization thread */
        return 0 ;

    if (USE_INLINEASM_ENV == 0)
        exec_env = env;

    trace_formation_init(env);
    ibtc_init(env);
    dbo_register_thread(env->cpu_index);

    return 1;
}

/*
 * optimization_finalize()
 *  Finalize the optimization schemes.
 */
int optimization_finalize(CPUState *env)
{
    if (env->cpu_index == -1)   /* llvm optimization thread */
        return 0 ;

    trace_formation_finalize(env);
    ibtc_finalize(env);
    dbo_deregister_thread();

    return 1;
}

/*
 * optimization_reset()
 *  Reset to default values of the optimizatiion schemes.
 */
int optimization_reset(CPUState *env)
{
    ibtc_reset(env);

    return 1;
}

/*
 * helper_counter_inc() and helper_counter_inc_client() are used to profile
 * the branch count of a region. These two functions are parsed by llvm-gcc
 * and will be inserted in the LLVM module. They are used by the LLVM
 * transltion only.
 */
void helper_counter_inc(int type, void *counter_p, int level)
{
    uint64_t **counter = (uint64_t **)(long)counter_p;
    int cpuid = exec_env->cpu_index;
    if (type == 0)
        counter[cpuid][1]++;
    counter[cpuid][level]++;
}

void helper_counter_inc_client(int type, uint32_t id, int level)
{
    uint64_t **counter = net_get_counter(id);
    int cpuid = exec_env->cpu_index;
    if (type == 0)
        counter[cpuid][1]++;
    counter[cpuid][level]++;
}

/*
 * The following data structure and routine are used to save/restore the states
 * of CPUState. Only the states that could affect decoding the guest binary by
 * the TCG front-end are saved/restored. Such states are saved when translating
 * the block at the first time because the states could change later and are
 * restored to the saved values when the block is decoded again during the
 * trace formation.
 */
struct i386_env
{
    int singlestep_enabled;
    uint32_t cpuid_features;
    uint32_t cpuid_ext_features;
    uint32_t cpuid_ext2_features;
    uint32_t cpuid_ext3_features;
};

struct arm_env
{
    int singlestep_enabled;
    uint32_t CF;     /* 0 or 1 */
    uint32_t VF;     /* V is the bit 31. All other bits are undefined */
    uint32_t NF;     /* N is bit 31. All other bits are undefined. */
    uint32_t ZF;     /* Z set if zero.  */
    uint32_t QF;     /* 0 or 1 */
    uint32_t GE;     /* cpsr[19:16] */
    uint32_t thumb;  /* cpsr[5]. 0 = arm mode, 1 = thumb mode. */
    uint32_t condexec_bits; /* IT bits.  cpsr[15:10,26:25].  */
};

struct ppc_env
{
    int singlestep_enabled;
    int mmu_idx;
    uint32_t flags;
};

struct sh4_env
{
    int singlestep_enabled;
    uint32_t sr;	/* status register */
    uint32_t fpscr;	/* floating point status/control register */
    uint32_t features;
};

struct m68k_env
{
    int singlestep_enabled;
    uint32_t sr;	/* status register */
    uint32_t fpcr;	/* floating point status/control register */
};

struct mips_env
{
    int singlestep_enabled;
    target_ulong btarget;
};

void tcg_save_state(CPUState *env, TranslationBlock *tb)
{
#define COPY_STATE(_dst, _src, _e) do { _dst->_e = _src->_e; } while(0)

#if defined(TARGET_I386) || defined(TARGET_X86_64)
    struct i386_env *i386e = tb->env = (struct i386_env *)llvm_malloc(sizeof(struct i386_env));
    COPY_STATE(i386e, env, singlestep_enabled);
    COPY_STATE(i386e, env, cpuid_features);
    COPY_STATE(i386e, env, cpuid_ext_features);
    COPY_STATE(i386e, env, cpuid_ext2_features);
    COPY_STATE(i386e, env, cpuid_ext3_features);
#elif defined(TARGET_ARM)
    struct arm_env *arme = tb->env = (struct arm_env *)llvm_malloc(sizeof(struct arm_env));
    COPY_STATE(arme, env, singlestep_enabled);
    COPY_STATE(arme, env, thumb);
    COPY_STATE(arme, env, condexec_bits);
    COPY_STATE(arme, env, CF);
    COPY_STATE(arme, env, VF);
    COPY_STATE(arme, env, NF);
    COPY_STATE(arme, env, ZF);
    COPY_STATE(arme, env, QF);
    COPY_STATE(arme, env, GE);
#elif defined(TARGET_PPC) || defined(TARGET_PPC64)
    struct ppc_env *ppce = tb->env = (struct ppc_env *)llvm_malloc(sizeof(struct ppc_env));
    COPY_STATE(ppce, env, singlestep_enabled);
    COPY_STATE(ppce, env, mmu_idx);
    COPY_STATE(ppce, env, flags);
#elif defined(TARGET_SH4)
    struct sh4_env *sh4e = tb->env = (struct sh4_env *)llvm_malloc(sizeof(struct sh4_env));
    COPY_STATE(sh4e, env, singlestep_enabled);
    COPY_STATE(sh4e, env, sr);
    COPY_STATE(sh4e, env, fpscr);
    COPY_STATE(sh4e, env, features);
#elif defined(TARGET_M68K)
    struct m68k_env *m68ke = tb->env = (struct m68k_env *)llvm_malloc(sizeof(struct m68k_env));
    COPY_STATE(m68ke, env, singlestep_enabled);
    COPY_STATE(m68ke, env, sr);
    COPY_STATE(m68ke, env, fpcr);
#elif defined(TARGET_MIPS)
    struct mips_env *mipse = tb->env = (struct mips_env *)llvm_malloc(sizeof(struct mips_env));
    COPY_STATE(mipse, env, singlestep_enabled);
    COPY_STATE(mipse, env, btarget);
#endif

#undef COPY_STATE
}

int tcg_pack_state(char *p, TranslationBlock *tb)
{
#if defined(TARGET_I386) || defined(TARGET_X86_64)
    memcpy(p, tb->env, sizeof(struct i386_env));
    return sizeof(struct i386_env);
#elif defined(TARGET_ARM)
    memcpy(p, tb->env, sizeof(struct arm_env));
    return sizeof(struct arm_env);
#elif defined(TARGET_PPC) || defined(TARGET_PPC64)
    memcpy(p, tb->env, sizeof(struct ppc_env));
    return sizeof(struct ppc_env);
#elif defined(TARGET_SH4)
    memcpy(p, tb->env, sizeof(struct sh4_env));
    return sizeof(struct sh4_env);
#elif defined(TARGET_M68K)
    memcpy(p, tb->env, sizeof(struct m68k_env));
    return sizeof(struct m68k_env);
#elif defined(TARGET_MIPS)
    memcpy(p, tb->env, sizeof(struct mips_env));
    return sizeof(struct mips_env);
#endif
    return 0;
}

/*
 * tcg_restore_state()
 *  Reset states to those when the block is first translated.
 */
void *tcg_copy_state(CPUState *env, TranslationBlock *tb)
{
#define COPY_STATE(_dst, _src, _e)  \
    do {                            \
        backup->_e = _dst->_e;      \
        _dst->_e = _src->_e;        \
    } while (0)

#if defined(TARGET_I386) || defined(TARGET_X86_64)
    struct i386_env *i386e = (struct i386_env *)tb->env;
    struct i386_env *backup = (struct i386_env *)llvm_malloc(sizeof(struct i386_env));
    COPY_STATE(env, i386e, singlestep_enabled);
    COPY_STATE(env, i386e, cpuid_features);
    COPY_STATE(env, i386e, cpuid_ext_features);
    COPY_STATE(env, i386e, cpuid_ext2_features);
    COPY_STATE(env, i386e, cpuid_ext3_features);
#elif defined(TARGET_ARM)
    struct arm_env *arme = (struct arm_env *)tb->env;
    struct arm_env *backup = (struct arm_env *)llvm_malloc(sizeof(struct arm_env));
    COPY_STATE(env, arme, singlestep_enabled);
    COPY_STATE(env, arme, thumb);
    COPY_STATE(env, arme, condexec_bits);
    COPY_STATE(env, arme, CF);
    COPY_STATE(env, arme, VF);
    COPY_STATE(env, arme, NF);
    COPY_STATE(env, arme, ZF);
    COPY_STATE(env, arme, QF);
    COPY_STATE(env, arme, GE);
#elif defined(TARGET_PPC) || defined(TARGET_PPC64)
    struct ppc_env *ppce = (struct ppc_env *)tb->env;
    struct ppc_env *backup = (struct ppc_env *)llvm_malloc(sizeof(struct ppc_env));
    COPY_STATE(env, ppce, singlestep_enabled);
    COPY_STATE(env, ppce, mmu_idx);
    COPY_STATE(env, ppce, flags);
#elif defined(TARGET_SH4)
    struct sh4_env *sh4e = (struct sh4_env *)tb->env;
    struct sh4_env *backup = (struct sh4_env *)llvm_malloc(sizeof(struct sh4_env));
    COPY_STATE(env, sh4e, singlestep_enabled);
    COPY_STATE(env, sh4e, sr);
    COPY_STATE(env, sh4e, fpscr);
    COPY_STATE(env, sh4e, features);
#elif defined(TARGET_M68K)
    struct m68k_env *m68ke = (struct m68k_env *)tb->env;
    struct m68k_env *backup = (struct m68k_env *)llvm_malloc(sizeof(struct m68k_env));
    COPY_STATE(env, m68ke, singlestep_enabled);
    COPY_STATE(env, m68ke, sr);
    COPY_STATE(env, m68ke, fpcr);
#elif defined(TARGET_MIPS)
    struct mips_env *mipse = (struct mips_env *)tb->env;
    struct mips_env *backup = (struct mips_env *)llvm_malloc(sizeof(struct mips_env));
    COPY_STATE(env, mipse, singlestep_enabled);
    COPY_STATE(env, mipse, btarget);
#else
    void *backup = NULL;
#endif

#undef COPY_STATE

    return backup;
}

void tcg_restore_state(CPUState *env, void *backup, int clean)
{
#define COPY_STATE(_dst, _src, _e) do { _dst->_e = _src->_e; } while (0)

#if defined(TARGET_I386) || defined(TARGET_X86_64)
    struct i386_env *i386e = (struct i386_env *)backup;
    COPY_STATE(env, i386e, singlestep_enabled);
    COPY_STATE(env, i386e, cpuid_features);
    COPY_STATE(env, i386e, cpuid_ext_features);
    COPY_STATE(env, i386e, cpuid_ext2_features);
    COPY_STATE(env, i386e, cpuid_ext3_features);
#elif defined(TARGET_ARM)
    struct arm_env *arme = (struct arm_env *)backup;
    COPY_STATE(env, arme, singlestep_enabled);
    COPY_STATE(env, arme, thumb);
    COPY_STATE(env, arme, condexec_bits);
    COPY_STATE(env, arme, CF);
    COPY_STATE(env, arme, VF);
    COPY_STATE(env, arme, NF);
    COPY_STATE(env, arme, ZF);
    COPY_STATE(env, arme, QF);
    COPY_STATE(env, arme, GE);
#elif defined(TARGET_PPC) || defined(TARGET_PPC64)
    struct ppc_env *ppce = (struct ppc_env *)backup;
    COPY_STATE(env, ppce, singlestep_enabled);
    COPY_STATE(env, ppce, mmu_idx);
    COPY_STATE(env, ppce, flags);
#elif defined(TARGET_SH4)
    struct sh4_env *sh4e = (struct sh4_env *)backup;
    COPY_STATE(env, sh4e, singlestep_enabled);
    COPY_STATE(env, sh4e, sr);
    COPY_STATE(env, sh4e, fpscr);
    COPY_STATE(env, sh4e, features);
#elif defined(TARGET_M68K)
    struct m68k_env *m68ke = (struct m68k_env *)backup;
    COPY_STATE(env, m68ke, singlestep_enabled);
    COPY_STATE(env, m68ke, sr);
    COPY_STATE(env, m68ke, fpcr);
#elif defined(TARGET_MIPS)
    struct mips_env *mipse = (struct mips_env *)backup;
    COPY_STATE(env, mipse, singlestep_enabled);
    COPY_STATE(env, mipse, btarget);
#endif
    if (clean == 1 && backup)
        llvm_free(backup);

#undef COPY_STATE
}

/*
 * llvm_check_flush()
 *  Check if any other component has issued a flush request.
 */
inline int llvm_check_flush(void)
{
#if defined(CONFIG_LLVM)
#if defined(CONFIG_HYBRID)
    return llvm_flush == 1;
#else
    return llvm_tb_alloc() == 0;
#endif
#endif
    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

