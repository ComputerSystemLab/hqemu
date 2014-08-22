/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <inttypes.h>
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm-debug.h"
#include "llvm-profile.h"
#include "llvm-opc.h"
#include "llvm.h"
#include "utils.h"

extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern ProfileFactory *PF;
extern int HybridMode;
#ifdef CONFIG_NET
extern uint32_t SendSize;
extern uint32_t RecvSize;
#endif

/*
 * ProfileFactory
 */
ProfileFactory::ProfileFactory(uint64_t mode) : Mode(PROFILE_NONE)
{
    uid = 0;
    llvm_lock_init(&lock);
    
    if (mode == PROFILE_NONE)
        ParseProfileMode();
    else
        Mode = mode;

    BlockTransCycles = 0;
    TraceTransCycles = 0;
}

ProfileFactory::~ProfileFactory()
{
    if (getProfile() == PROFILE_NONE)
        return;

    if (HybridMode == HYBRID_NONE)
        printBlockHistory();
    else
        printTraceHistory();
}

void ProfileFactory::ParseProfileMode()
{
    static char profile_str[PROFILE_NUM][65] = {
        "none", "basic", "trace", "cache", "breakdown", "mem", "dbo", "all"
    };
    static uint64_t profile_enum[PROFILE_NUM] = {
        PROFILE_NONE, PROFILE_BASIC, PROFILE_TRACE, 
        PROFILE_CACHE, PROFILE_BREAKDOWN, PROFILE_MEM, 
        PROFILE_DBO, PROFILE_ALL,
    };
    
    char *s, *p, *e, buf[65];
    
    p = getenv("LLVM_PROFILE");
    if (p == NULL || strlen(p) == 0)
        return;
    
    e = p + strlen(p);
    s = buf;
    while (p <= e)
    {
        if (*p == ',' || p == e)
        {
            *s = '\0';
            for (int i = 0; i < PROFILE_NUM; i++)
            {
                if (!strcmp(buf, profile_str[i]))
                {
                    Mode |= profile_enum[i];
                    break;
                }
            }
            s = buf;
            p++;
            continue;
        }
        *s++ = *p++;
        if (s-buf > 64)
            s = buf;
    }
}

void ProfileFactory::reset()
{
    llvm_spin_lock(&lock);
    uid = 0;
    llvm_spin_unlock(&lock);
}

uint32_t ProfileFactory::getUID()
{
    uint32_t id;
    llvm_spin_lock(&lock);
    id = uid++;
    llvm_spin_unlock(&lock);

    return id;
}

void ProfileFactory::resetUID()
{
    llvm_spin_lock(&lock);
    uid = 0;
    llvm_spin_unlock(&lock);
}

void ProfileFactory::addCPU(int cpuid)
{
    llvm_spin_lock(&lock);
    CPUID.push_back(cpuid);
    Exec.push_back(new ExecProfile());
    llvm_spin_unlock(&lock);
}

void ProfileFactory::setExecProfile(int CPUIndex, ExecProfile &Info)
{
    ExecProfile *EP = Exec[CPUIndex];
    EP->Block = Info.Block;
    EP->Trace = Info.Trace;
    EP->Other = Info.Other;
}

void ProfileFactory::addBlock(BlockInfo *Block, int64_t elapse)
{
    Blocks.push_back(Block);
    BlockTransCycles += elapse;
}

void ProfileFactory::addTrace(TraceInfo *Trace)
{
    llvm_spin_lock(&lock);
    Traces[Trace->ID] = Trace;
    llvm_spin_unlock(&lock);
}

void ProfileFactory::addTraceCycles(int64_t elapse)
{
    TraceTransCycles += elapse;
}

void ProfileFactory::printBlockHistory()
{
    if (!(Mode & PROFILE_BASIC))
        return;

    /* Basic block information: # BBs, guest/host size, guest icount, 
       translation time. */
    int GuestICount=0, GuestSize=0, HostSize=0;
    for (BIVec::iterator I = Blocks.begin(), E = Blocks.end(); I != E; I++)
    {
        BlockInfo *Block = static_cast<BlockInfo*>(*I);
        GuestICount += Block->GuestICount;
        GuestSize += Block->GuestSize;
        HostSize += Block->HostSize;
    }

    unsigned long freq = get_cpu_speed();
    fprintf(stderr, "-------------------------\n");
    fprintf(stderr, "CPU Frequency  : %ld MHz\n\n", freq);
    fprintf(stderr, "Block statistic:\n");
    fprintf(stderr, "Num of Blocks  : %d\n", (int)Blocks.size());
    fprintf(stderr, "G/H Code Size  : %d/%d bytes\n", GuestSize, HostSize);
    fprintf(stderr, "Guest ICount   : %d\n", GuestICount);
    if (freq == 0)
        fprintf(stderr, "Trans. Cycles  : %"PRId64" cycles\n", BlockTransCycles);
    else
        fprintf(stderr, "Trans. Time    : %.4f seconds\n", BlockTransCycles / freq * 1e-6);
}

void ProfileFactory::printTraceHistory()
{
    int NumTraces = Traces.size();
    int Len = 0, MaxLen = 0;
    int Exit = 0, MaxExit = 0;
    int IndirectBranch = 0, MaxIndirectBranch = 0;
    map<int,int> CountMap;  /* # traces per length */
    int GuestSize = 0, HostSize = 0, GuestICount = 0;

    /* Static information */
    if (Mode & PROFILE_BASIC)
    {
        map<int, int> VersionMap;
        unsigned long freq = get_cpu_speed();

        printBlockHistory();

        /* # block, exit, indirect branch and duplicate block. */
        for (TIMap::iterator I = Traces.begin(), E = Traces.end(); I != E; I++)
        {
            TraceInfo *Trace = static_cast<TraceInfo *>(I->second);
            if (Trace->Committed == false)
                continue;

            GuestSize += Trace->GuestSize;
            GuestICount += Trace->GuestICount;
            HostSize += Trace->HostSize;

            Len += Trace->NumBlock;
            Exit += Trace->NumExit;
            IndirectBranch += Trace->NumIndirectBranch;
            if (Trace->NumBlock > MaxLen) MaxLen = Trace->NumBlock;
            if (Trace->NumExit > MaxExit) MaxExit = Trace->NumExit;
            if (Trace->NumIndirectBranch > MaxIndirectBranch)
                MaxIndirectBranch = Trace->NumIndirectBranch;
            
            CountMap[Trace->NumBlock]++;
            VersionMap[Trace->Version]++;
        }

        fprintf(stderr, "\nTrace statistic:\n");
        fprintf(stderr, "Num of Traces  : %d\n", NumTraces);
        fprintf(stderr, "Profile Thres. : %d\n", TraceProfileThres);
        fprintf(stderr, "Predict Thres. : %d\n", TracePredictThres);
        fprintf(stderr, "G/H Code Size  : %d/%d bytes\n", GuestSize, HostSize);
        if (freq == 0)
            fprintf(stderr, "Trans. Cycles  : %"PRId64" cycles\n", TraceTransCycles);
        else
            fprintf(stderr, "Trans. Time    : %.4f seconds\n", TraceTransCycles / freq * 1e-6);
        fprintf(stderr, "Average Len    : %.1f (max=%d)\n", (double)Len/NumTraces, MaxLen);
        fprintf(stderr, "Average # Exit : %.1f (max=%d)\n", (double)Exit/NumTraces, MaxExit);
        fprintf(stderr, "Average # IB   : %.1f (max=%d)\n", (double)IndirectBranch/NumTraces, MaxIndirectBranch);
        fprintf(stderr, "Flush Count    : %d\n", Env->getFlushCount());

#ifdef CONFIG_NET
        fprintf(stderr, "Send Size      : %d bytes\n", SendSize);
        fprintf(stderr, "Recv Size      : %d bytes\n", RecvSize);
#endif
        fprintf(stderr, "\n");

        fprintf(stderr, "Length distribution: (1-%d)\n    ", MaxLen);
        for (int i = 1; i <= MaxLen; i++)
            fprintf(stderr, "%d ", CountMap[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Version distribution: (0-%d)\n    ", (int)VersionMap.size()-1);
        for (map<int, int>::iterator I = VersionMap.begin(), E = VersionMap.end(); I != E; I++)
            fprintf(stderr, "%d ", I->second);
        fprintf(stderr, "\n");
    }

    if (Mode & PROFILE_BREAKDOWN)
    {
        fprintf(stderr, "\nBreakdown:\n");
        for (int i = 0, e = Exec.size(); i < e; i++)
        {
            ExecProfile *EP = Exec[i];
            uint64_t Total = EP->Block + EP->Trace + EP->Other;
            if (Total == 0)
                fprintf(stderr, "Thread %d: no sample\n", i);
            else
                fprintf(stderr, "Thread %d: block %.1f%% trace %.1f%% other %.1f%%\n", i,
                        (double)EP->Block * 100 / Total, (double)EP->Trace * 100 / Total,
                        (double)EP->Other * 100 / Total);
        }
        fprintf(stderr, "\n");
    }

    if (Mode & PROFILE_MEM)
    {
        fprintf(stderr, "Memory statistic:\n");
        for (int i = 0, e = Exec.size(); i < e; i++)
        {
            ExecProfile *EP = Exec[i];
            fprintf(stderr, "Thread %d: NumLoads=%"PRId64" NumStores=%"PRId64"\n", i, EP->NumLoads, EP->NumStores);
        }
        fprintf(stderr, "\n");
    }

    /* Code cache infomation - start address and size */
    if (Mode & PROFILE_CACHE)
    {
        fprintf(stderr, "-------------------------\n");
        fprintf(stderr, "Block/Trace Cache information:\n");
        fprintf(stderr, "Block: start=%p size=%d code=%d\n", qemu_get_cache_address(), (int)qemu_get_cache_size(),
                (int)qemu_get_code_size());
        fprintf(stderr, "Trace: start=%p size=%d code=%d\n", Env->getCodeCacheAddr(), (int)Env->getCodeCacheSize(),
                (int)Env->getCodeSize());
        fprintf(stderr, "\n");
    }

    if (!(Mode & PROFILE_TRACE))
        return;

    /* Detailed trace information and runtime counters. */
    fprintf(stderr, "-------------------------\n");
    fprintf(stderr, "Trace information:\n");

#ifndef CONFIG_NET
    fprintf(stderr, " Id (Ver)(Len)  PC of TBs\n");
    for (TIMap::iterator I = Traces.begin(), E = Traces.end(); I != E; I++)
    {
        TraceInfo *Trace = static_cast<TraceInfo*>(I->second);
        
        fprintf(stderr, "%3d (%3d)(%3d): ", I->first, Trace->Version, Trace->NumBlock);
        for (int i = 0, e = Trace->PC.size(); i < e; i++)
            fprintf(stderr, "0x%"PRIx" ", Trace->PC[i]);
        fprintf(stderr, "\n");
    }
#endif

    for (int i = 0, e = CPUID.size(); i < e; i++)
    {
        int cpuid = CPUID[i];
        int used = 0;

        fprintf(stderr, "-------------------------\n");
        fprintf(stderr, "Thread %d:\n", cpuid);
        fprintf(stderr, "  Id (Ver)(L:Ex)     IBTC  LoopCnt/   Count: ( Len) Accumulated Exit Cnt\n");
        for (TIMap::iterator I = Traces.begin(), E = Traces.end(); I != E; I++)
        {
            TraceInfo *Trace = static_cast<TraceInfo*>(I->second);
            uint64_t *Exit = Trace->Exit[cpuid];
            uint64_t len;
            uint64_t count = Exit[0];
            
            len = Exit[0] * Trace->NumBlock;   /* first, get accumulated loop count */
            for (int j = 0; j < Trace->NumBlock; j++)
            {
                int idx = j + 2;
                count += Exit[idx];
                len += Exit[idx] * (j+1);
            }

            if (len == 0)
                continue;
            
            used++;
            fprintf(stderr, "%4d (%3d)(%d:%2d) %8"PRId64" %8"PRId64"/%8"PRId64": (%4.1f) ", 
                    Trace->ID, Trace->Version, Trace->NumLoop, Trace->NumExit,
                    Exit[1], Exit[0], count, (double)len/(count-Exit[0]));
            
            for (int j = 0; j < Trace->NumBlock; j++)
                fprintf(stderr, "%"PRId64" ", Exit[j+2]);
            
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "Trace used: %d/%d\n", used, NumTraces);
    }
}

/*
 * wrapper functions
 */
#ifdef __cplusplus
extern "C" {
#endif

void add_block_profile(CPUState *env)
{
    if (PF == NULL)
        DM->Error("%s: profile factory not initialized.\n", __func__);
}

void add_trace_profile(CPUState *env, int profile_thres, int predict_thres)
{
    if (PF == NULL)
        DM->Error("%s: profile factory not initialized.\n", __func__);

    if (env->cpu_index >= MAX_PROFILE_THREADS)
        DM->Error("%s: exceeding max number of threads (%d)\n", __func__,
                MAX_PROFILE_THREADS);

    PF->TraceProfileThres = profile_thres;
    PF->TracePredictThres = predict_thres;
    PF->addCPU(env->cpu_index);
}

#ifdef __cplusplus
}
#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

