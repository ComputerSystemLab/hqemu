/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 *
 *   This file exposes C interfaces to the LLVM routines in C++.
 */

#ifndef __LLVM_WRAPPER_H
#define __LLVM_WRAPPER_H

#include <stdint.h>
#include "config-host.h"
#include "config-target.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Enable indirect branch target translation or not. */
#if defined(CONFIG_USER_ONLY)
#define ENABLE_IBTC
#endif

/* Enable Pin's IB chaning or not. Use IBTC hash table lookup if ENABLE_IBCHAIN
 * is not defined. */
#if defined(__i386__) || defined(__x86_64__) || defined(__arm__)
//#define ENABLE_IBCHAIN
#endif

#define llvm_code_gen_buffer    CodeCache
#define llvm_code_gen_ptr       CodeGenPtr

extern uint8_t *CodeCache;
extern uint8_t *CodeGenPtr;
extern int llvm_flush;


/* Initialize the LLVM translation environment. */
int llvm_gen_init(CPUState *env, void *tcg_context);

/* Destroy the LLVM translation environment. */
int llvm_gen_finalize(void);

/* Translate a block of code. */
int llvm_gen_block(CPUState *env, void *tcg_context, TranslationBlock *tb);

/* Translate a region of code. */
int llvm_gen_trace(CPUState *env, TranslationBlock **tbs, int count, int loop_idx);

/* Flush the optmizated code cache. */
int llvm_tb_flush(void);

/* Keep a record of a translated block. */
int llvm_tb_record(TranslationBlock *tb, int host_size, int64_t elapse);

/* Add a path in the GlobalCFG. */
int llvm_build_cfg(TranslationBlock *src, TranslationBlock *dst);

/* Check if a flush request is issued. */
int llvm_check_flush(void);

/* Return the patch address for the region chaining.*/
int llvm_resolve_address(unsigned long *addr);

/* Query the index of the guest instruction that causes interrupt or
 * exception. */
int llvm_lookup_inst(TranslationBlock *tb, long offset);

/* Stop the optimization service before performing fork. */
void llvm_fork_start(void);

/* Restart the optimization service after performing fork.*/
void llvm_fork_end(int child);

/* Initialize the optimization schemes. */
int optimization_init(CPUState *env);

/* Finalize the optimization schemes. */
int optimization_finalize(CPUState *env);

/* Reset to default values of the optimizatiion schemes. */
int optimization_reset(CPUState *env);

/* Update an IBTC entry. */
void update_ibtc_entry(CPUState *env, TranslationBlock *tb);

/* Insert trace profiling and prediction stubs at each block. */
void tb_gen_trace_stub(CPUState *env, TranslationBlock *tb);

/* Make a copy of the TCG-dependent variables of CPUState. */
void tcg_save_state(CPUState *env, TranslationBlock *tb);

/* Start trace profiling. */
__inline__ void tb_start_trace_profile(TranslationBlock *tb);

/* Start trace prediction. Stop prediction if `tb' is NULL. */
__inline__ void tb_start_trace_predict(TranslationBlock *tb);

#ifdef __cplusplus
}
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

