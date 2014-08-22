/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_TARGET_H
#define __LLVM_TARGET_H

#include <map>
#include "config-target.h"
#include "llvm/Instructions.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Target/TargetJITInfo.h"
#include "llvm-debug.h"

extern "C"
{
#include "tcg.h"
#include "disas.h"
#include "exec-all.h"
#include "optimization.h"

#define FILE _IO_FILE
#include "exec.h"
#undef FILE
}

using namespace llvm;
using namespace std;

class JITSyncData;
class CallBackInfo;

#if TARGET_LONG_BITS == 32
#define PRIx    "x"
#else
#define PRIx    __PRI64_PREFIX "x"
#endif

#if defined(TCG_TARGET_I386) || defined(TCG_TARGET_ARM)  /* Little Endian */
#  ifdef TARGET_WORDS_BIGENDIAN
#    define NEED_BSWAP
#  endif
#else /* Big Endian */
#  ifndef TARGET_WORDS_BIGENDIAN
#    define NEED_BSWAP
#  endif
#endif

/*
 * Runtime helper information (used for 64-bit to 32-bit emulation).
 */
struct TCGRuntimeHelper
{
    const char *fname;  /* Helper function name */
    void *func;         /* Function pointer */
};

#if defined(TCG_TARGET_ARM)
#define TO_CPSR(opc) ((opc == ARITH_CMP || opc == ARITH_CMN || opc == ARITH_TST) << 20)
enum arm_cond_code_e
{
    COND_EQ = 0x0,
    COND_NE = 0x1,
    COND_CS = 0x2,      /* Unsigned greater or equal */
    COND_CC = 0x3,      /* Unsigned less than */
    COND_MI = 0x4,      /* Negative */
    COND_PL = 0x5,      /* Zero or greater */
    COND_VS = 0x6,      /* Overflow */
    COND_VC = 0x7,      /* No overflow */
    COND_HI = 0x8,      /* Unsigned greater than */
    COND_LS = 0x9,      /* Unsigned less or equal */
    COND_GE = 0xa,
    COND_LT = 0xb,
    COND_GT = 0xc,
    COND_LE = 0xd,
    COND_AL = 0xe,
};

enum arm_data_opc_e
{
    ARITH_AND = 0x0,
    ARITH_EOR = 0x1,
    ARITH_SUB = 0x2,
    ARITH_RSB = 0x3,
    ARITH_ADD = 0x4,
    ARITH_ADC = 0x5,
    ARITH_SBC = 0x6,
    ARITH_RSC = 0x7,
    ARITH_TST = 0x8,
    ARITH_CMP = 0xa,
    ARITH_CMN = 0xb,
    ARITH_ORR = 0xc,
    ARITH_MOV = 0xd,
    ARITH_BIC = 0xe,
    ARITH_MVN = 0xf,
};
#endif

/*
 * EventListener is used by the JIT to notify clients about significant events
 * during compilation.
 */
class EventListener : public JITEventListener
{
private:
    TargetJITInfo &TJI;
    CallBackInfo &CallBack;  /* Info to pass among translator and JIT */
    JITSyncData &JSData;     /* Synchronous data for client/server mode */

public:
    EventListener(TargetJITInfo &tji, CallBackInfo &callback);
    virtual void NotifyFunctionEmitted(const Function &F,
            void *Code, size_t Size,
            const EmittedFunctionDetails &Details);

    /* HandleEmittedCodeInfo - Set patch rules for client/server mode. */
    void HandleEmittedCodeInfo(void *Code, const EmittedCodeInfo &ECInfo);
};

/* Generate a trampoline, which jumps to the epilog, at the start of the
 * optimized code cache. */
int gen_target_stub(void *Stub);

/* Patch the address of external symbols (used only for Client/Server model). */
void PatchExternalSymbol(uintptr_t Code, JITSyncData &JSData, 
        vector< pair<uint32_t,int> > &ExternalSymbol);

/* Patch the address of constant pools (used only for Clinet/Server model). */
void PatchMovConstPool(uintptr_t Code, int Off, int CPollOff);

/* Patch a jump. */
void PatchCodeShort(TargetJITInfo &TJI, uintptr_t Old, uintptr_t New);
void PatchCode(TargetJITInfo &TJI, uintptr_t Old, uintptr_t New, int JmpOff);

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

