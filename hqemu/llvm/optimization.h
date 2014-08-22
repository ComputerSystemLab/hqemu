/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __OPTIMIZATION_H
#define __OPTIMIZATION_H

#ifdef __cplusplus
extern "C" {
#endif
#include "exec-all.h"
#include "list.h"
#include "utils.h"

#if TCG_TARGET_REG_BITS == 32
#define tcg_gen_st_ptr          tcg_gen_st_i32
#define tcg_gen_brcond_ptr      tcg_gen_brcond_i32
#define tcg_temp_free_ptr       tcg_temp_free_i32
#define tcg_temp_local_new_ptr  tcg_temp_local_new_i32
#else
#define tcg_gen_st_ptr          tcg_gen_st_i64
#define tcg_gen_brcond_ptr      tcg_gen_brcond_i64
#define tcg_temp_free_ptr       tcg_temp_free_i64
#define tcg_temp_local_new_ptr  tcg_temp_local_new_i64
#endif

#if TARGET_LONG_BITS == 32
#define TCGv TCGv_i32
#else
#define TCGv TCGv_i64
#endif


/*
 * Indirect Branch Target Cache (IBTC)
 */
#define IBTC_CACHE_BITS     (16)
#define IBTC_CACHE_SIZE     (1U << IBTC_CACHE_BITS)
#define IBTC_CACHE_MASK     (IBTC_CACHE_SIZE - 1)
#define IBCHAIN_INSERT_TAIL
#define IBCHAIN_MAX_SIZE (16)

/* IBTC entry */
struct jmp_pair
{
    target_ulong eip;       /* Guest EIP (hash key) */
    TranslationBlock *tb;   /* Guest TB  (value) */
};

/* IBTC hash table */
struct ibtc_table
{
    struct jmp_pair htable[IBTC_CACHE_SIZE];
};

/* Invalidate an IBTC entry. */
void invalidate_ibtc_entry(CPUState *env, target_ulong eip);


/*
 * Trace formation
 */
#define PROFILE_THRESHOLD   50
#define PREDICT_THRESHOLD   16

/* Return a copy of TCG-dependent variables of CPUState. */
void *tcg_copy_state(CPUState *env, TranslationBlock *tb);

/* Restore the TCG-dependent variables of CPUState. */
void tcg_restore_state(CPUState *env, void *backup, int clean);

/* Copy the TCG-dependent variables of CPUStatet to the address `p'. */
int tcg_pack_state(char *p, TranslationBlock *tb);


/*
 * Profile
 */
/* Add settings for block-level profiling. */
void add_block_profile(CPUState *env);

/* Add settings for trace-level profiling. */
void add_trace_profile(CPUState *env, int profile_thres, int predict_thres);

/* Helper function to profile branch count. */
#ifdef CONFIG_NET
uint64_t **net_get_counter(uint32_t id);
#else
#define net_get_counter(a) NULL
#endif


/*
 * Misc
 */
/* Return the block-to-trace patch point (address which is patched a jump from
 * the block code cache to the optimized code cache). */
unsigned long tb_get_jmp_entry(TranslationBlock *tb);

/* Return the next address the patch point will jump to initially. */
inline unsigned long tb_get_jmp_next(TranslationBlock *tb);

/* Perform Pin's IB chaining at an translated region. */
int llvm_update_ibtc_entry(int idx, TranslationBlock *tb);

/* Determine if the optimized code cache is full. */
int llvm_tb_alloc(void);

#ifdef CONFIG_DBO
/* Register/Deregister a thread for DBO monitoring. */
int dbo_register_thread(pid_t tid);
int dbo_deregister_thread(void);
#else
#define dbo_register_thread(a)
#define dbo_deregister_thread()
#endif

#ifdef __cplusplus
}
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

