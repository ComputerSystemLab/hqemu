/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_H
#define __LLVM_H

using namespace std;

extern "C"
{
#include "cpu-all.h"
}
#include "llvm-opc.h"
#include "llvm-net.h"

class LLVMTranslator;
class OptimizationInfo;
class DefaultJITMemoryManager;

/* Translation mode */
enum
{
    HYBRID_NONE = 0,    /* Block granularity */
    HYBRID_SINGLE,      /* Single thread */
    HYBRID_MULTIPLE,    /* Multiple threads */
    HYBRID_HPM,         /* HPM-based region selection */
    HYBRID_SERVER,      /* DBT server */
    HYBRID_CLIENT,      /* DBT client */
};

/* Optimization mode */
enum
{
    LLVM_GEN_BLOCK = 0,
    LLVM_GEN_TRACE,
    LLVM_REGEN_TRACE,
};

extern int HybridMode;

/*
 * LLVMEnv is the top level container of whole LLVM translation environment
 * which manages the LLVM translator(s) and globally shared resources. The 
 * LLVMEnv instance must be initialized before using the underlying transaltion
 * service and can only be initialized ONCE.
 */
class LLVMEnv
{
private:
    LLVMEnv(CPUState *env, TCGContext *tcg_context);

    int FlushCount;               /* The amount of trace cache flushed */
    ControlFlowGraph *GlobalCFG;  /* The whole-program CFG containing the 
                                     translated blocks only */
    DefaultJITMemoryManager *MM;  /* Trace cache manager */
    int NumTranslator;            /* The amount of LLVM translators */
    LLVMTranslator **Translator;  /* LLVM translators */
    pthread_t *HelperThread;      /* LLVM translation threads */
    CPUState *ThreadEnv;          /* Cloned CPU state for each helper thread.
                                     Required when invoking TCG front-end. */
    size_t CodeCacheSize;         /* Size of the optimized code cache */
    TCodeMap CodeBlocks;          /* Info of translated blocks. */
    TCodeMap CodeRegions;         /* Info of translated regions. */
    PatchInfo Patch;              /* Info for patching code */
    TraceChainMap TraceChain;     /* Used for trace chaining. */
    ChainVec ChainPoint;          /* Chaining positions of traces */
    IBChainVec IBChainPoint;      /* Chaining positions for IBTC chaining */
    vector<tcg_target_ulong> CallTable;      /* CallInst to reserve space */
    map<uintptr_t, const char *> HelperMap;  /* Address/Name of QEMU helpers */

    /* MapHelperFunctions - Keep track of the function pointer and name pair of
     * helper functions in QEMU for emulating guest architecture. */
    void MapHelperFunctions();

    /* ParseCodeSize - Parse the environment variable for the size of optimized
     * code cache. */
    size_t ParseCodeSize();

    /* ParseCommandLineOptions - Parse the command line options. */
    void ParseCommandLineOptions();

    /* ParseEnvironment - Parse the environment variables controlling the LLVM
     * translators. */
    void ParseEnvironment();

    /* createTranslator - Create LLVM translators and worker threads. */
    void createTranslator(CPUState *env, TCGContext *tcg_context,
            int NumThread);

public:
    ~LLVMEnv();

    static bool InitOnce;  /* Whether if LLVMEnv has been initialized */
    PeerInfo *Peer;        /* Component for network communication */

    /* createLLVMEnv - Interface to create the LLVMEnv instance. */
    static void createLLVMEnv(CPUState *env, void *tcg_context);

    /* SpawnThread - Create specific amount of worker thread(s) with the thread
     * routine specified. */
    void SpawnThread(int NumThread, void *(*ThreadFunc)(void*));

    /* getNumTranslator - Get the amount the translator created. */
    int getNumTranslator()                { return NumTranslator;  }

    /* getTranslator - Get specific LLVM translator with index. */
    LLVMTranslator *getTranslator(int ID) { return Translator[ID]; }

    /* getSingleTranslator - Get the first (index 0) LLVM translator. */
    LLVMTranslator *getSingleTranslator() { return Translator[0];  }

    /* getHelperThread - Get the structure of the helper thread list. */
    pthread_t *getHelperThread()          { return HelperThread;   }

    /* getThreadEnv - Get specific CPU state of helper thread with index. */
    CPUState *getThreadEnv(int ID)        { return &ThreadEnv[ID]; }

    /* getGlobalCFG - Get GlobalCFG. */
    ControlFlowGraph *getGlobalCFG()      { return GlobalCFG; }

    /* getHelperName - Get the name of QEMU helper function. */
    const char *getHelperName(uintptr_t addr)   { return HelperMap[addr]; }

    /*
     * Interfaces for other components to get info of LLVM environment.
     */
    DefaultJITMemoryManager *getMemoryManager() { return MM;        }
    vector<tcg_target_ulong> &getCallTable()    { return CallTable; }
    TCodeMap &getCodeBlocks();
    TCodeMap &getCodeRegions();
    PatchInfo &getPatch();
    TraceChainMap &getTraceChain();
    ChainVec &getChainPoint();
    IBChainVec &getIBChainPoint();
    JITSyncData &getJITSyncData();

    int getFlushCount()     { return FlushCount; }
    /* incFlushCount - Increase the flush count by one. */
    void incFlushCount()    { FlushCount++;      }

    /* getCodeCacheAddr - Get the start address of the optimized code cache. */
    void *getCodeCacheAddr();
    /* getCodeCacheSize - Get the size of the optimized code cache. */
    size_t getCodeCacheSize();
    /* getCodeSize - Get the size of optimized code translated. */
    size_t getCodeSize();

    /* startFunctionBody - Lock a region of memory for emitting code. Return
     * a poniter to it and its actual size. */
    uint8_t *startFunctionBody(const Function *F, uintptr_t &ActualSize);

    /* endFunctionBody - Release lock of the memory region for emitting code. */
    void endFunctionBody(const Function *F, uint8_t *FunctionStart, 
            uint8_t *FunctionEnd);

    /* is_multithreaded - Determine if the translator is running multiple
     * threads. */
    static inline bool is_multithreaded()
    {
        return (HybridMode == HYBRID_MULTIPLE || HybridMode == HYBRID_HPM);
    }
};

/*
 * LLVM Translator
 */
class LLVMTranslator
{
private:
    LLVMTranslator(TCGContext *tcg_context, int id);

    /* Basic types */
    CONST Type *VoidTy;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *Int128Ty;
    CONST IntegerType *IntPtrTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int16PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;
    CONST Type *FloatTy;
    CONST Type *DoubleTy;
    CONST Type *FP80Ty;
    CONST Type *FP128Ty;
    CONST PointerType *FloatPtrTy;
    CONST PointerType *DoublePtrTy;

    int MyID;                 /* Translator ID */
    LLVMContext Context;      /* Translator local context */
    Module *Mod;              /* The LLVM module */
    ExecutionEngine *EE;      /* The JIT compiler */
    EventListener *Listener;  /* The JIT listener */
    CallBackInfo *CallBack;   /* Info to be set by the JIT listener */
    TargetMachine *TM;        /* Target machine used by this translator */
    const TargetData *TD;     /* Target data */
    OwningPtr<MemoryBuffer> BufferPtr;  /* Storage for bitcode module */
    Triple TargetTriple;                /* The triple value */

    FunctionPassManager *HelperPasses;     /* PassManager for helpers */
    map<string, HelperInfo*> HelperFuncs;  /* Helper with special handling */
    VectorFactory *VF;        /* SIMDization factory */
    IRFactory *IF;            /* TCG-to-LLVM IR converter */
    FuncPtr *OpcFn;           /* Pointers to IR conversion routines */

    StateTypeMap StateType;   /* Offset and type of guest registers */
    SPRegMap SpecialReg;      /* Special registers to remap its type */
    JITSyncData JSData;       /* Sync data by client/server mode */
    IVec toErase;             /* Instructions to be removed */

    /* InitModule - Initialization of the LLVM module. */
    void InitModule();

    /* InitJIT - JIT compiler creation. */
    void InitJIT();

    /* InitBasicTypes - Initialize basic types which are used along with the
     * translation process. */
    void InitBasicTypes();

    /* InitGuest - Setup guest-dependent structures. */
    void InitGuest();

    /* InitHost - Setup host-dependent structures. */
    void InitHost();

    /* InitHelperFunctions - Initialization of helper functions. */
    void InitHelperFunctions(TCGContext *s);

    /* DefineSpecialReg - Setup special registers. */
    void DefineSpecialReg();

    /* RemapSpecialReg - Map a special register to another type with the same
     * size as the original type. */
    bool RemapSpecialReg(CONST Type *Ty, size_t &Off);

    /* FlattenType - Convert the CPUState of aggregate type to the list of
     * single element of primitive type. */
    void FlattenType(CONST Type *Ty, size_t &Off);

    /* UpdateDependentFunctions - Bind addresses to the nested functions which
     * are used by the helper fucntions. */
    void UpdateDependentFunctions();

    /* UpdateFPUFunctions - Bind addresses to the nested functions which are
     * used by the softfloat helper functions. */
    void UpdateFPUFunctions();

    /* UpdateMapping - Bind an address to a function. */
    void UpdateMapping(string Name, uintptr_t Addr);

    /* OptimizeHelperFunc - Analyze and optimize a helper function. */
    int OptimizeHelperFunc(Function *F);

    int SplitState(Instruction *I, size_t Off, size_t Size);

    /* OptimizeSIMDFunc - Convert the SIMD helper functions which are
     * implemeted by the scalar operations to LLVM vector operations. */
    int OptimizeSIMDFunc(Function *F);

    /* OptimizeSelection: Convert the min/max helper functions to use LLVM
     * SelectInst instruction. */
    int OptimizeSelection(string Name, Value *I, Value *Base);

    /* PopulateCallTable - Initialize CallTable which is used to reserve space in
     * the code cache for trace chaining. */
    void PopulateCallTable();

    /* resolveLibcall - Get the address of external library symbols. */
    void resolveLibcall();

    /* resolveGlobalVariable - Get the address of the global variables used by
     * the helper functions. */
    void resolveGlobalVariable();

    /* MoveToReg - Generate the inline assembly of moving the value V to the
     * register Reg. */
    void MoveToReg(Value *V, string Reg, Instruction *InsertPos);

public:
    ~LLVMTranslator();

    int BaseRegNo;           /* Base register # to hold the CPUState pointer */
    char *BaseRegStr;        /* Name of the base register */
    CONST Type *BaseRegType; /* LLVM Type of the base register */
    BasicType BT;       /* Container of basic types to be passed among the
                           translator components */
    int X86FPOff[3];    /* Offsets to the x86 FP registers (x86 guest only) */

    /* setX86FPOff - Set the offsets of the x86 FP registers in CPUState. */
    void setX86FPOff(int *Off);

    /* Update - Update address to symbols and create IRFactory lazily. (used
     * for the client/server mode only) */
    void Update(TCGContext *tcg_context);

    int getID()                                { return MyID;        }
    Module *getModule()                        { return Mod;         }
    LLVMContext &getContext()                  { return Context;     }
    ExecutionEngine *getExecutionEngine()      { return EE;          }
    const TargetData *getTargetData()          { return TD;          }
    StateTypeMap &getStateType()               { return StateType;   }
    CallBackInfo *getCallBackInfo()            { return CallBack;    }
    JITSyncData &getSyncData()                 { return JSData;      }
    map<string, HelperInfo*> &getHelperFuncs() { return HelperFuncs; }

    /* getTargetArch - Get the string of the guest architecture. */
    string getTargetArch()
    {
        return TargetTriple.getArchName().str();
    }

    /* printAsm - Show guest assembly code for each compiled TB. */
    static void printAsm(CPUState *env, TranslationBlock *tb);

    /* printOp - Show TCG micro ops for each compiled TB. */
    static void printOp(TCGContext *tcg_context);

    /* dump - Show Asm and Op debug messages. */
    void dump(TCGContext *tcg_context, CPUState *env, TranslationBlock *tb);

    /* GenBlock - Translate a block of guest code. */
    TranslatedCode *GenBlock(TCGContext *tcg_context, CPUState *env,
            TranslationBlock *tb);

    /* GenTrace - Translate a trace of guest code. This function is used by the
     * non-client/server mode. */
    TranslatedCode *GenTrace(TCGContext *tcg_context, CPUState *env,
            OptimizationInfo *Opt);

    /* GenTrace - Translate a trace of guest code. This function is used by the
     * client/server mode only. */
    TranslatedCode *GenTrace(TCGContext *tcg_context, CPUState *env,
            OptimizationInfo *Opt, TraceRequest &Req);

    /* createLLVMTranslator - Interface to create the LLVMTranslator
     * instrance. */
    static LLVMTranslator *createLLVMTranslator(TCGContext *tcg_context, int id)
    {
        return new LLVMTranslator(tcg_context, id);
    }
};

/*
 * OptimizationInfo is the description to an optimization request. It consists
 * of the optimization mode and the control-flow-graph of the trace.
 */
class OptimizationInfo
{
public:
    OptimizationInfo(uint32_t mode, int count, int idx, int version,
            int cache_version, GraphNode *cfg=NULL) : 
        Mode(mode), Count(count), LoopIdx(idx), CFG(cfg), Version(version),
        CacheVersion(cache_version)
    {
        TID = gettid();
    }

    pid_t TID;         /* Thread ID to send this request */
    uint32_t Mode;     /* Optimization mode: first gen or regen */
    int Count;         /* Block count of the NET trace */
    int LoopIdx;       /* Index to the loopback block of the NET trace */
    GraphNode *CFG;    /* CFG of the NETPlus region */
    NodeVec CFGVec;    /* Block list of the NETPlus region */
    int Version;       /* Version of the region */
    int CacheVersion;  /* Current QEMU code cache version */

    int getLoopIdx()               { return LoopIdx;      }
    GraphNode *getCFG()            { return CFG;          }
    NodeVec &getCFGVec()           { return CFGVec;       }
    TranslationBlock *getHeadTB()  { return CFG->getTB(); }
    void setCFG(GraphNode *cfg)    { CFG = cfg;           }
};

/* Expand a NET trace to a region with the NETPlus algorithm. */
void ExpandTrace(OptimizationInfo *Opt);

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
