/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_PROFILE_H
#define __LLVM_PROFILE_H

#include "llvm/LLVMContext.h"
#include "llvm/Type.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm-debug.h"
#include "llvm.h"
#include "list.h"

using namespace llvm;
using namespace std;

#define MAX_PROFILE_THREADS 256

#define PROFILE_NONE        (uint64_t)0
#define PROFILE_BASIC       ((uint64_t)1 << 0)
#define PROFILE_TRACE       ((uint64_t)1 << 1)
#define PROFILE_CACHE       ((uint64_t)1 << 2)
#define PROFILE_BREAKDOWN   ((uint64_t)1 << 3)
#define PROFILE_MEM         ((uint64_t)1 << 4)
#define PROFILE_DBO         ((uint64_t)1 << 5)
#define PROFILE_ALL         PROFILE_BASIC | PROFILE_TRACE | PROFILE_CACHE | PROFILE_BREAKDOWN | PROFILE_DBO
#define PROFILE_NUM         8

typedef vector<CodeInfo*> BIVec;
typedef map<int,CodeInfo*> TIMap;

class ProfileFactory
{
private:
    uint64_t Mode;
    llvm_lock_t lock;
    uint32_t uid;
    vector<int> CPUID;
    vector<ExecProfile*> Exec;

public:
    ProfileFactory(uint64_t mode=PROFILE_NONE);
    ~ProfileFactory();

    void reset();
    void ParseProfileMode();
    uint32_t getUID();
    void resetUID();
    uint64_t getProfile() { return Mode; }
    void setProfile(uint64_t mode) { Mode = mode; }
    void setExecProfile(int CPUIndex, ExecProfile &Exec);
    void addCPU(int cpuid);

    /* block */
    int64_t BlockTransCycles;
    BIVec Blocks;
    void addBlock(BlockInfo *Block, int64_t elapse);
    void printBlockHistory();

    /* trace */
    int64_t TraceTransCycles;
    TIMap Traces;
    int TraceProfileThres;
    int TracePredictThres;
    void addTrace(TraceInfo *Trace);
    void addTraceCycles(int64_t elapse);
    void printTraceHistory();
};

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
