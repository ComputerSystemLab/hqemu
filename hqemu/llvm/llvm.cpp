/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <pthread.h>
#include "llvm/ADT/OwningPtr.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm-profile.h"
#include "llvm-pass.h"
#include "llvm-dbo.h"
#include "llvm.h"
#include "JIT.h"
#include "JITMemoryManager.h"
#include "llvm-wrapper.h"
#include "qemu-timer.h"

#define MAX_TRANSLATORS     8
#define MAX_SEARCH_DEPTH    8

LLVMEnv *Env;
LLVMDebug *DM;
ProfileFactory *PF;
int HybridMode;
CodeGenOpt::Level OptLevel = CodeGenOpt::Default;
llvm_lock_t llvm_global_lock;
llvm_lock_t llvm_interface_lock;
llvm_lock_t llvm_debug_lock;

bool LLVMEnv::InitOnce = false;
bool NoLazyCompilation = false;
bool isThreadActive = false;
static int MaxRegenJob = 8;
struct queue_t *TraceGenQueue;

extern int tb_flush_version;
extern uint8_t *ibtc_helper_entry;
extern uint8_t *ibchain_gen_ptr;
extern uint8_t *ibchain_gen_end;

static inline bool CheckOptimization(LLVMTranslator *Translator, CPUState *env);

/*
 * LLVMEnv()
 *  Intialize LLVM translator(s) and globally shared resources. The LLVMEnv
 *  instance must be initialized before using the underlying transaltion
 *  service and should be initialized only ONCE.
 */
LLVMEnv::LLVMEnv(CPUState *env, TCGContext *tcg_context) : NumTranslator(1), 
    HelperThread(NULL), ThreadEnv(NULL)
{
    Env = this;
    DM = new LLVMDebug;

    DM->Debug(DEBUG_LLVM, "Initializing LLVM Environment.\n");
 
    /* Initialize LLVM targets and assembly printers/parsers. */
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllAsmPrinters();
    InitializeAllAsmParsers();
#if defined(LLVM_V30) || defined(LLVM_V31) || defined(LLVM_V32svn)
    InitializeAllTargetMCs();
#endif

    ParseCommandLineOptions();
    ParseEnvironment();

    FlushCount = 0;
    TraceGenQueue = create_queue();
    llvm_lock_init(&llvm_global_lock);
    llvm_lock_init(&llvm_interface_lock);
    llvm_lock_init(&llvm_debug_lock);

    GlobalCFG = new ControlFlowGraph;
    PF = new ProfileFactory;
    dbo_init();

    /* Track all QEMU helper functions. */
    MapHelperFunctions();

    if (HybridMode == HYBRID_SERVER)
    {
        copy_tcg_context(NULL);
        reset_tcg_context();
    }

    /* Create the memory manager and intialize the optimized code cache. There
     * is only copy of the optimized code cache and is shared by all underlying
     * translators. */
    MM = createMemoryManager(CodeCacheSize);

    createTranslator(env, tcg_context, NumTranslator);

    if (HybridMode == HYBRID_CLIENT || HybridMode == HYBRID_SERVER)
        net_init();

    DM->Debug(DEBUG_LLVM, "LLVM Env initialized. guest_base %x Cache size "
            "%d bytes.\n", GUEST_BASE, (int)getCodeCacheSize());
    
    InitOnce = true;
}

LLVMEnv::~LLVMEnv()
{
    /* Stop the LLVM translation threads before the program is terminated. */
    if (isThreadActive == true)
    {
        isThreadActive = false;
        for (int i = 0; i < NumTranslator; i++)
            pthread_join(HelperThread[i], NULL);
    }
    llvm_stop_multithreaded();
    llvm_shutdown();

    net_finalize();
    dbo_finalize();

    delete [] Translator;
    delete [] HelperThread;
    delete [] ThreadEnv;

    TraceChain.clear();
    ChainPoint.clear();
    IBChainPoint.clear();

    /* Delete all translated code. */
    for (TCodeMap::iterator I = CodeBlocks.begin(), E = CodeBlocks.end();
            I != E; I++)
        delete I->second;
    for (TCodeMap::iterator I = CodeRegions.begin(), E = CodeRegions.end();
            I != E; I++)
        delete I->second;

    delete PF;
    delete DM;
    delete MM;

    /* Do not delete GlobalCFG here because when one guest thread calls
     * exit_group and destroy LLVM, other threads may insert an edge to
     * GlobalCFG at the same time. */
}

void LLVMEnv::ParseCommandLineOptions()
{
    const char *argv[] = {
        /* Disable passes changing the DebugLoc metadata which may fail our
         * block/trace chaining. */
        "-disable-branch-fold",
        "-disable-tail-duplicate",
        "-disable-early-taildup",
#if defined(TCG_TARGET_ARM)
        "-arm-long-calls",
        "-post-RA-scheduler=false",
        "-disable-prepost-index",
        "-disable-expand-pseudo",
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
        "-disable-machine-dce",
        "-disable-machine-cse",
#endif

#if defined(ENABLE_INSTCOMBINE)
        "-disable-bc-vector",
#endif
#if defined(ENABLE_STATS)
        "-stats",       /* show statistics for each pass */
        "-time-passes", /* print elapsed time for each pass */
#endif
    };

    DM->Debug(DEBUG_LLVM, "Parsing command line options.\n");
    cl::ParseCommandLineOptions(ARRAY_SIZE(argv), const_cast<char**>(argv));
}

/*
 * ParseCodeSize()
 *  Parse the user-defined optimized code cache size.
 */
size_t LLVMEnv::ParseCodeSize()
{
    char *p, *s;
    size_t base = 1;

    if (HybridMode == HYBRID_SERVER)
        return 0;

    s = getenv("LLVM_SIZE");
    if (s == NULL || strlen(s) == 0)
        return 0;

    p = s;
    for (int i = 0, e = strlen(s); i < e; i++)
    {
        if (*p < '0' || *p > '9')
            break;
        p++;
    }
    if (*p == 'K' || *p == 'k')
        base = 1024;
    else if (*p == 'M' || *p == 'm')
        base = 1024 * 1024;
    else if (*p == 'G' || *p == 'g')
        base = 1024 * 1024 * 1024;

    return strtoul(s, &p, 10) * base;
}

/*
 * ParseEnvironment()
 *   Pasrse the environment variables which control the LLVM translators.
 */
void LLVMEnv::ParseEnvironment()
{
    char *p;

    DM->Debug(DEBUG_LLVM, "Parsing environmental variables.\n");

    p = getenv("LLVM_THREAD");
    if (p != NULL && (HybridMode == HYBRID_MULTIPLE || HybridMode == HYBRID_HPM))
    {
        int NumThread = atoi(p);
        if (NumThread < 1)
        {
            NumTranslator = 1;
            HybridMode = HYBRID_SINGLE;
        }
        else
            NumTranslator = MIN(MAX_TRANSLATORS, NumThread);
    }
    else
        NumTranslator = 1;

    CodeCacheSize = ParseCodeSize();
}

/*
 * WorkerFunc()
 *  The thread routine of the LLVM translation threads.
 */
void *WorkerFunc(void *argv)
{
    int MyID = (int)(long)argv;
    LLVMTranslator *Translator = Env->getTranslator(MyID);
    CPUState *env = Env->getThreadEnv(MyID);

    env->cpu_index = -1;
    env->gen_trace = 1;
    copy_tcg_context(env);
    optimization_init(env);

    for (;;)
    {
        if (unlikely(isThreadActive == false))
            break;

        if (llvm_flush == 1)
        {
            reset_queue(TraceGenQueue);
            usleep(100);
            continue;
        }
        if (llvm_tb_alloc() == 0)
        {
            llvm_flush = 1;
            usleep(1);
            continue;
        }

        CheckOptimization(Translator, env);
        usleep(1);
    }

    pthread_exit(NULL);
}

/*
 * SpawnThread()
 *  Create worker thread(s) with the thread routine specified.
 */
void LLVMEnv::SpawnThread(int NumThread, void *(*ThreadFunc)(void*))
{
    if (isThreadActive == true)
    {
        DM->Warning("%s: helper threads already running.\n", __func__);
        return;
    }
    if (HelperThread == NULL)
        DM->Error("%s: internal error.\n", __func__);
    if (NumThread > NumTranslator)
        DM->Error("%s: exceeding maximun amount of threads.\n", __func__);

    isThreadActive = true;
    for (int i = 0; i < NumThread; i++)
    {
        int ret = pthread_create(&HelperThread[i], NULL, ThreadFunc, (void*)(long)i);
        if (ret != 0)
            DM->Error("%s: failed to create worker thread.\n", __func__);
    }
}

/*
 * createTranslator()
 *  Create LLVM translators and worker threads. We create the instances of
 *  translators and helper threads during the initialization of LLVMEnv and
 *  each helper thread will pick its own translator instance later.
 */
void LLVMEnv::createTranslator(CPUState *env, TCGContext *tcg_context, int NumThread)
{
    DM->Debug(DEBUG_LLVM, "Creating %d translator(s):\n", NumThread);

    Translator = new LLVMTranslator*[NumThread];
    HelperThread = new pthread_t[NumThread];
    ThreadEnv = new CPUState[NumThread];

    for (int i = 0; i < NumThread; i++)
    {
        /* The CPUState used by each translation thread is initialized by
         * copying the value from the main execution thread's CPUState. For the
         * DBT server, the `env' is passed as NULL and we delay the creation of
         * the CPUState after the server is synced with the DBT client. */
        if (env != NULL)
            memcpy(&ThreadEnv[i], env, sizeof(CPUState));
        Translator[i] = LLVMTranslator::createLLVMTranslator(tcg_context, i);
    }

    if (HybridMode == HYBRID_MULTIPLE || HybridMode == HYBRID_HPM)
        SpawnThread(NumThread, WorkerFunc);
}

/*
 * startFunctionBody()
 *  Lock a region of memory for code emission. The poniter to the memory region
 *  and its available size are returned.
 */
uint8_t *LLVMEnv::startFunctionBody(const Function *F, uintptr_t &ActualSize)
{
    return MM->startFunctionBody(F, ActualSize);
}

/*
 * endFunctionBody()
 *  Release the locking of the memory region for the code emission.
 */
void LLVMEnv::endFunctionBody(const Function *F, uint8_t *FunctionStart,
                uint8_t *FunctionEnd)
{
    return MM->endFunctionBody(F, FunctionStart, FunctionEnd);
}

/*
 * getCodeCacheAddr()
 *  Return the start address of the optimized code cache.
 */
void *LLVMEnv::getCodeCacheAddr()
{
    return MM->getCodeCacheAddr();
}

/*
 * getCodeCacheSize()
 *  Return the total size of the optimized code cache.
 */
size_t LLVMEnv::getCodeCacheSize()
{
    return MM->getCodeCacheSize();
}

/*
 * getCodeSize()
 *  Return the translated size of the optimized host code.
 */
size_t LLVMEnv::getCodeSize()
{
    return MM->getCodeSize();
}

TCodeMap &LLVMEnv::getCodeBlocks()
{
    return CodeBlocks;
}

TCodeMap &LLVMEnv::getCodeRegions()
{
    return CodeRegions;
}

PatchInfo &LLVMEnv::getPatch()
{
    if (HybridMode == HYBRID_SERVER)
        return *static_cast<ServerInfo*>(Peer)->CachedPatch;
    return Patch;
}

TraceChainMap &LLVMEnv::getTraceChain()
{
    if (HybridMode == HYBRID_SERVER)
        return *static_cast<ServerInfo*>(Peer)->CachedTraceChain;
    return TraceChain;
}

ChainVec &LLVMEnv::getChainPoint()
{
    if (HybridMode == HYBRID_SERVER)
        return *static_cast<ServerInfo*>(Peer)->CachedChainPoint;
    return ChainPoint;
}

IBChainVec &LLVMEnv::getIBChainPoint()
{
    return IBChainPoint;
}

/*
 * get_hybrid_mode()
 *  Get the enumeration of the translation mode.
 */
static int get_hybrid_mode(CPUState *env)
{
#if defined(CONFIG_HYBRID)
    if (!strcmp(CONFIG_HYBRID, "single"))
        return HYBRID_SINGLE;
    if (!strcmp(CONFIG_HYBRID, "multiple"))
        return HYBRID_MULTIPLE;
    if (!strcmp(CONFIG_HYBRID, "hpm"))
        return HYBRID_HPM;
    if (!strcmp(CONFIG_HYBRID, "net"))
        return (env == NULL) ? HYBRID_SERVER : HYBRID_CLIENT;
    
    DM->Error("%s: invalid hybrid mode `%s'.\n", __func__, CONFIG_HYBRID);
    return 0;
#else
    return HYBRID_NONE;
#endif
}

/*
 * createLLVMEnv()
 *  The interface to create the LLVMEnv instance.
 */
void LLVMEnv::createLLVMEnv(CPUState *env, void *tcg_context)
{
    if (InitOnce == true)
        DM->Error("%s: llvm environment already initialized.\n", __func__);

    HybridMode = get_hybrid_mode(env);

    try
    {
        llvm_start_multithreaded();
        Env = new LLVMEnv(env, (TCGContext *)tcg_context);
    }
    catch(exception& e)
    {
        DM->Error("%s: %s\n", __func__, e.what());
    }
}

/*
 * LLVM Translator
 */
LLVMTranslator::LLVMTranslator(TCGContext *tcg_context, int id) : MyID(id)
{
    DM->Debug(DEBUG_LLVM, "Starting LLVM Translator %d.\n", MyID);

    /* Create LLVM module, JIT engine and basic types. */
    InitModule();
    InitJIT();
    InitBasicTypes();

    /* Create the pass which analyzes the helper functions. */
    HelperPasses = new FunctionPassManager(Mod);
    HelperPasses->add(createHelperPass());

    /* Initialize guest-dependent and host-dependent structures. */
    InitGuest();
    InitHost();

    if (HybridMode == HYBRID_SERVER)
        return;

    /* Optimize helper functions. */
    InitHelperFunctions(tcg_context);

    /* Create the TCG IR to LLVM IR conversion module. */
    IF = new IRFactory(tcg_context, this);
    OpcFn = (FuncPtr *)IF->getOpcFn();

    DM->Debug(DEBUG_LLVM, "LLVM Translator %d initialized.\n", MyID);
}

LLVMTranslator::~LLVMTranslator()
{
    for (map<string, HelperInfo*>::iterator I = HelperFuncs.begin(),
            E = HelperFuncs.end(); I != E; I++)
        delete I->second;

    delete HelperPasses;
    delete Listener;
    delete CallBack;
    delete VF;
    delete IF;
    delete EE;
    delete Mod;
}

/*
 * getBitcodeFile()
 *  Read LLVM IR of helper functions from the bitcode file.
 */
static string getBitcodeFile()
{
    string ErrorMsg;
    string BitcodeFile = "";
    struct stat buf;
    char filename[2][256] = {"./llvm_helper_" TARGET_ARCH ".bc",
        CONFIG_LLVM_BITCODE, };

    for (int i = 0; i < 2; i++)
    {
        if (stat(filename[i], &buf) == 0)
        {
            BitcodeFile = filename[i];
            break;
        }
    }

    if (BitcodeFile == "")
        DM->Error("%s: cannot find Bitcode file %s\n", __func__,
                CONFIG_LLVM_BITCODE);

    return BitcodeFile;
}

/*
 * InitModule()
 *  Perform the initialization of the LLVM module.
 */
void LLVMTranslator::InitModule()
{
    string ErrorMsg;

    string BitcodeFile = getBitcodeFile();

#if defined(LLVM_V28)
    MemoryBuffer *Buffer = MemoryBuffer::getFile(BitcodeFile, &ErrorMsg);
    if (Buffer == NULL)
        DM->Error("Bitcode file %s: %s\n", BitcodeFile.c_str(),
                ErrorMsg.c_str());
#else
    if (error_code ec = MemoryBuffer::getFile(BitcodeFile, BufferPtr))
        DM->Error("Bitcode file %s: %s\n", BitcodeFile.c_str(),
                ec.message().c_str());

    MemoryBuffer *Buffer = BufferPtr.get();
#endif

    Mod = getLazyBitcodeModule(Buffer, Context, &ErrorMsg);
    if (Mod == NULL)
        DM->Error("%s: %s\n", __func__, ErrorMsg.c_str());

    /* If lazy compilation is disabled, materialize the helper function now. */
    if (NoLazyCompilation && Mod->MaterializeAllPermanently(&ErrorMsg))
        DM->Error("%s: %s\n", __func__, ErrorMsg.c_str());

    TargetTriple = Triple(Mod->getTargetTriple());

    DM->Debug(DEBUG_LLVM, "Module initialized for target (%s).\n", 
            TargetTriple.str().c_str());
}

/*
 * InitJIT()
 *  Create JIT engine.
 */
void LLVMTranslator::InitJIT()
{
    string ErrorMsg;

    /* Create JIT execution engine. */
    EngineBuilder builder(Mod);
    builder.setAllocateGVsWithCode(false);
    builder.setJITMemoryManager(Env->getMemoryManager());
    builder.setErrorStr(&ErrorMsg);
    builder.setEngineKind(EngineKind::JIT);
    builder.setOptLevel(OptLevel);
    EE = builder.create();
    
    if (EE == NULL)
        DM->Error("%s: %s\n", __func__, ErrorMsg.c_str());
    
    /* Create JIT event listener and link target machine. */
    CallBack = new CallBackInfo;
    CallBack->Func = NULL;
    CallBack->JSData = &JSData;

    Listener = new EventListener(((JIT*)EE)->getJITInfo(), *CallBack);
    EE->RegisterJITEventListener(Listener);
    EE->DisableLazyCompilation(NoLazyCompilation);

#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30) || defined(LLVM_V31)
    TD = EE->getTargetData();
#else
    TD = EE->getDataLayout();
#endif

    /* Link the underlying TargetMachine so that we can manipulate the host
     * registers directly. */
    TM = ((JIT*)EE)->getTargetMachine();
}

/*
 * InitBasicTypes()
 *  Initialize basic types which are used along with the translation process.
 *  Also pack the basic types in BT which are passed among the translator
 *  components.
 */
void LLVMTranslator::InitBasicTypes()
{
    BT.VoidTy   = VoidTy   = Type::getVoidTy(Context);
    BT.Int8Ty   = Int8Ty   = IntegerType::get(Context, 8);
    BT.Int16Ty  = Int16Ty  = IntegerType::get(Context, 16);
    BT.Int32Ty  = Int32Ty  = IntegerType::get(Context, 32);
    BT.Int64Ty  = Int64Ty  = IntegerType::get(Context, 64);
    BT.Int128Ty = Int128Ty = IntegerType::get(Context, 128);
    
    BT.IntPtrTy   = IntPtrTy    = TD->getIntPtrType(Context);
    BT.Int8PtrTy  = Int8PtrTy   = Type::getInt8PtrTy(Context, 0);
    BT.Int16PtrTy = Int16PtrTy  = Type::getInt16PtrTy(Context, 0);
    BT.Int32PtrTy = Int32PtrTy  = Type::getInt32PtrTy(Context, 0);
    BT.Int64PtrTy = Int64PtrTy  = Type::getInt64PtrTy(Context, 0);
    
    BT.FloatTy  = FloatTy  = Type::getFloatTy(Context);
    BT.DoubleTy = DoubleTy = Type::getDoubleTy(Context);
    BT.FP80Ty   = FP80Ty   = Type::getX86_FP80Ty(Context);
    BT.FP128Ty  = FP128Ty  = Type::getFP128Ty(Context);

    BT.FloatPtrTy  = FloatPtrTy  = Type::getFloatPtrTy(Context, 0);
    BT.DoublePtrTy = DoublePtrTy = Type::getDoublePtrTy(Context, 0);
}

/*
 * setX86FPOff()
 *  Set the offsets of the x86 FP registers in CPUState.
 */
void LLVMTranslator::setX86FPOff(int *Off)
{
    X86FPOff[0] = Off[0];
    X86FPOff[1] = Off[1];
    X86FPOff[2] = Off[2];
}

/*
 * getBaseRegStr()
 *  Parse the name of register that holds the pointer to CPUState.
 */
static char *getBaseRegStr(Value *V)
{
#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30)
    ConstantArray *CA = dyn_cast<ConstantArray>(V);
    if (CA == NULL || CA->isString() == false)
        DM->Error("%s: cannot find basereg string.\n", __func__);

    return strdup(const_cast<char*>(CA->getAsString().c_str()));
#else
    ConstantDataArray *CA = dyn_cast<ConstantDataArray>(V);
    if (CA == NULL || CA->isString() == false)
        DM->Error("%s: cannot find basereg string.\n", __func__);

    return strdup(const_cast<char*>(CA->getAsString().str().c_str()));
#endif
}

/*
 * InitGuest()
 *  Setup guest-dependent data structures.
 */
void LLVMTranslator::InitGuest()
{
    size_t Off = 0;

    /* TODO: any smart way to hack into CPUState type? */
    Value *basetype = Mod->getNamedValue("basetype");
    if (basetype == NULL)
        DM->Error("%s: cannot resolve basetype.\n", __func__);

    GlobalValue *GV = Mod->getNamedValue("basereg");
    if (GV == NULL)
        DM->Error("%s: cannot find basereg.\n", __func__);

    Constant *C = static_cast<Constant*>(GV->getOperand(0));
    C = static_cast<Constant*>(C->getOperand(0));

    BaseRegNo = TCG_AREG0;
    BaseRegStr = getBaseRegStr(C->getOperand(0));
    BaseRegType = basetype->getType();

#if defined(TARGET_I386)
    X86FPOff[0] = offsetof(CPUState, fpstt);
    X86FPOff[1] = offsetof(CPUState, fptags[0]);
    X86FPOff[2] = offsetof(CPUState, fpregs[0]);
#endif

    /* Define the new types of special registers. */
    DefineSpecialReg();

    /* Convert the CPUState of aggregate type to the list of single element
     * of primitive type. */
    FlattenType(BaseRegType->getContainedType(0), Off);
}

/*
 * Update()
 *  For the DBT server, the optimization of helper functions and the creation
 *  of IRFactory are postponed because the address information can be resolved
 *  after the synchronization between the client and the server. This is
 *  function is called by the DBT server to perform such lazy initialization.
 */
void LLVMTranslator::Update(TCGContext *tcg_context)
{
    NestedFuncMap &NestedFunc = JSData.NestedFunc;
    GlobalMap &GlobalVariable = JSData.GlobalVariable;

    /* Optimize the helper functions. */
    InitHelperFunctions(tcg_context);

    /* Bind the function pointers to those on the client. */
    for (HFuncMap::iterator I = NestedFunc.begin(), E = NestedFunc.end();
            I != E; I++)
        UpdateMapping(I->second, I->first);

    /* Bind the symbols' addresses to those on the client. */
    for (GlobalMap::iterator I = GlobalVariable.begin(),
            E = GlobalVariable.end(); I != E; I++)
        UpdateMapping(I->second, I->first);

    IF = new IRFactory(tcg_context, this);
    OpcFn = (FuncPtr *)IF->getOpcFn();
}

/*
 * DefineSpecialReg()
 *  This function defines the special registers and the new types to be reset.
 */
void LLVMTranslator::DefineSpecialReg()
{
#if defined(TARGET_I386)
    Value *SIMDReg = Mod->getNamedValue("xmm_reg");
    if (SIMDReg)
    {
        if (SIMDReg->getType()->isPointerTy() == false)
            DM->Error("%s: fatal error.\n");

        CONST Type *OldTy = SIMDReg->getType()->getContainedType(0);
#if 0
        CONST Type *NewTy = VectorType::get(Int8Ty, 16);
#else
        CONST Type *NewTy = Int128Ty;
#endif
        SpecialReg[OldTy] = make_pair(1, NewTy);
    }
#endif
}

/*
 * RemapSpecialReg()
 *  Map a special register to another type with the same size as the original
 *  type. For example, remapping a <16 * i8> type to <2 * i64> or <1 * i128>.
 */
bool LLVMTranslator::RemapSpecialReg(CONST Type *Ty, size_t &Off)
{
    if (SpecialReg.count(Ty) == 0)
        return false;
    
    pair<int, CONST Type*> &MP = SpecialReg[Ty];
    size_t size = TD->getTypeSizeInBits(MP.second) / 8;

    for (int i = 0, e = MP.first; i < e; i++)
    {
        StateType[Off] = make_pair(MP.second, size);
        Off += size;
    }
    
    return true;
}

/*
 * FlattenType()
 *  Convert the CPUState of aggregate type to the list of single element of
 *  primitive type. Each element contains a pair of offset to CPUState and its
 *  type. This list of flattened type will be used for the state mapping pass.
 */
void LLVMTranslator::FlattenType(CONST Type *Ty, size_t &Off)
{
    int size;
    switch (Ty->getTypeID())
    {
        case Type::FloatTyID:
            StateType[Off] = make_pair(Ty,4);
            Off += 4;
            break;
        case Type::DoubleTyID:
            StateType[Off] = make_pair(Ty,8);
            Off += 8;
            break;
        case Type::X86_FP80TyID:
            StateType[Off] = make_pair(Ty,10);
            Off += 10;
            break;
        case Type::FP128TyID:
            StateType[Off] = make_pair(Ty,16);
            Off += 16;
            break;
        case Type::IntegerTyID:
        {
            size = cast<IntegerType>(Ty)->getBitWidth() / 8;
            StateType[Off] = make_pair(Ty, size);
            Off += size;
            break;
        }
        case Type::StructTyID:
        {
            if (RemapSpecialReg(Ty, Off) == true)
                break;

            size_t SubOff;
            CONST StructType *STy = cast<StructType>(Ty);
            size = TD->getTypeSizeInBits(STy) / 8;

            const StructLayout *SL = TD->getStructLayout(STy);
            for (int i = 0, e = STy->getNumElements(); i < e; i++)
            {
                SubOff = Off + SL->getElementOffset(i);
                FlattenType(STy->getElementType(i), SubOff);
            }
            Off += size;

            if (SubOff != Off)
            {
                /* We go here because this struct has padding at the end and
                 * the type of the last variable is extended to include the
                 * padding size. */
#if 0
                CONST Type *OldTy = StateType.rbegin()->second.first;
                if (OldTy->getTypeID() != Type::IntegerTyID)
                    DM->Error("%s: internal error.\n", __func__);
#endif

                size_t LastOff = StateType.rbegin()->first;
                int NewSize = Off - LastOff;
                CONST Type *NewTy = IntegerType::get(Context, NewSize*8);
                StateType[LastOff] = make_pair(NewTy, NewSize);
            }
            break;
        }
        case Type::PointerTyID:
        {
            size = TD->getPointerSizeInBits() / 8;
            StateType[Off] = make_pair(Ty, size);
            Off += size;
            break;
        }
        case Type::ArrayTyID:
        {
            CONST ArrayType *ATy = cast<ArrayType>(Ty);
            int NumElems = ATy->getNumElements();
            size_t ElemSize = TD->getTypeSizeInBits(ATy->getElementType()) / 8;
            for (int i = 0; i < NumElems; i++)
            {
                size_t SubOff = Off + i * ElemSize;
                FlattenType(ATy->getElementType(), SubOff);
            }
            Off += TD->getTypeSizeInBits(ATy) / 8;
            break;
        }

        /* unsupported types. */
        case Type::VectorTyID:
        case Type::VoidTyID:
        case Type::LabelTyID:
        case Type::MetadataTyID:
        case Type::FunctionTyID: 
        case Type::PPC_FP128TyID:
        default:
            DM->Error("%s: unrecognized type %d.\n", __func__, Ty->getTypeID());
            break;
    }
}

/*
 * InitHost()
 *  Setup host-dependent data structures.
 */
void LLVMTranslator::InitHost()
{
    const TargetRegisterInfo *TRI = TM->getRegisterInfo();
    const_cast<TargetRegisterInfo*>(TRI)->setExtraReservedRegs(BaseRegStr);

    PopulateCallTable();
}

/*
 * printAsm()
 *  Display the guest assembly code of the given basic block.
 */
void LLVMTranslator::printAsm(CPUState *env, TranslationBlock *tb)
{
    int disas_flags = 0;

#if defined(TARGET_I386)
#if defined(TARGET_X86_64)
    if ((tb->flags >> HF_CS64_SHIFT) & 1)
        disas_flags = 2;
    else
#endif
        disas_flags = !((tb->flags >> HF_CS32_SHIFT) & 1);
#elif defined(TARGET_ARM)
    disas_flags = env->thumb;
#elif defined(TARGET_PPC)
    int le_mode = env->hflags & (1 << MSR_LE) ? 1 : 0;
    disas_flags = env->bfd_mach;
    disas_flags |= le_mode << 16;
#endif
    
    fprintf(stderr, "----------------\n");
    fprintf(stderr, "IN: [size=%d]\n", tb->size);
    target_disas(stderr, tb->pc, tb->size, disas_flags);
    fprintf(stderr, "\n");
}

/*
 * printOp()
 *  Display TCG IR of the given basic block.
 */
void LLVMTranslator::printOp(TCGContext *tcg_context)
{
    fprintf(stderr, "OP:\n");
    tcg_dump_ops(tcg_context, stderr);
    fprintf(stderr, "\n");
}

/*
 * dump()
 *  Display the guest assembly and TCG IR of a basic block. The message is
 *  printed serially with the threads.
 */
void LLVMTranslator::dump(TCGContext *tcg_context, CPUState *env,
        TranslationBlock *tb)
{
    uint64_t DebugMode = DM->getDebug();

    llvm_spin_lock(&llvm_debug_lock);

    DM->Debug(DEBUG_LLVM, "Translator %d dumps asm...\n", getID());
    if (DebugMode & DEBUG_INASM)
        printAsm(env, tb);
    if (DebugMode & DEBUG_OP)
        printOp(tcg_context);

    llvm_spin_unlock(&llvm_debug_lock);
}

/*
 * GenBlock()
 *  Given a QEMU basic block, translate it to LLVM IR and generate host code.
 */
TranslatedCode *LLVMTranslator::GenBlock(TCGContext *tcg_context,
        CPUState *env, TranslationBlock *tb)
{
    int64_t start_cycle, end_cycle;
    target_ulong PC = tb->pc;
    uint64_t DebugMode = DM->getDebug();

    DM->Debug(DEBUG_LLVM, "%s pc=0x%x\n", __func__, PC);

    if (*gen_opc_ptr != INDEX_op_end)
        DM->Error("%s: fatal error - cannot find op_end\n", __func__);
    
    start_cycle = cpu_get_real_ticks();
    do_liveness_analysis(tcg_context);

#ifdef DEBUG_DISAS
    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP))) 
    {
        qemu_log("OP:\n");
        tcg_dump_ops(tcg_context, logfile);
        qemu_log("\n");
    }
#endif

    if (DebugMode & (DEBUG_INASM | DEBUG_OP))
        dump(tcg_context, env, tb);

    /* Iterate over each TCG IR and translate it to associated LLVM IRs. */
    int NumOPC = gen_opc_ptr - gen_opc_buf;
    const TCGArg *Args = gen_opparam_buf;

    IF->BlockSetContext(LLVM_GEN_BLOCK, tcg_context, PC);
    for (int i = 0; i < NumOPC; i++)
    {
        CallBack->OPCIdx = i;
        TCGOpcode opc = (TCGOpcode)gen_opc_buf[i];
        Args += (IF->*OpcFn[opc])(Args);
    }

    /* JIT host code. tb is registered so that JIT event listener can record
       the patching information. */
    CallBack->TB = tb;

    IF->Compile();

    TranslatedCode *TC = new TranslatedCode;
    BlockInfo *Block = static_cast<BlockInfo*>(IF->getCodeInfo());
    Block->Commit(tb, CallBack->Size);

    TC->Count = 1;
    TC->PC = PC;
    TC->CSBase = tb->cs_base;
    TC->CKey = CodeKey(PC, tb->cs_base);
    TC->Code = CallBack->Code;
    TC->Size = CallBack->Size;
    TC->Func = IF->getFunction();
    TC->Info = Block;
    TC->Patch = NULL;

    TCodeMap &CodeBlocks = Env->getCodeBlocks();
    CodeBlocks[tb] = TC;

    end_cycle = cpu_get_real_ticks();
    PF->addBlock(Block, end_cycle-start_cycle);

    return TC;
}

/*
 * GenTrace()
 *  Given a list of QEMU basic blocks, translate it to LLVM IR and generate
 *  host code.
 */
TranslatedCode *LLVMTranslator::GenTrace(TCGContext *tcg_context,
        CPUState *env, OptimizationInfo *Opt)
{
    TCGContext *s = tcg_context;
    target_ulong pc = Opt->CFG->getGuestPC();
    TranslationBlock *EntryTB = Opt->CFG->getTB();
    uint64_t DebugMode = DM->getDebug();

    IF->TraceInit(pc, Opt->Count, Opt->CFG, Opt->Version);
    TraceInfo *Trace = IF->TBuilder->getTrace();

    for (;;)
    {
        void *backup;
        GraphNode *Node = IF->TBuilder->getNextNode();
        if (Node == NULL)
            break;

        TranslationBlock *tb = Node->getTB();

        env->gen_trace_skip = 0;
        tcg_func_start(env, s);

        backup = tcg_copy_state(env, tb);
        gen_intermediate_code(env, tb);
        tcg_restore_state(env, backup, 1);

        if (env->gen_trace_skip == 1)
            return NULL;

        if (*gen_opc_ptr != INDEX_op_end)
            DM->Error("%s: fatal error - cannot find op_end\n", __func__);
        
        do_liveness_analysis(s);

        if (DebugMode & (DEBUG_INASM | DEBUG_OP))
            dump(s, env, tb);

        /* Iterate over each TCG IR and translate it to associated LLVM IRs. */
        int NumOPC = gen_opc_ptr - gen_opc_buf;
        const TCGArg *Args = gen_opparam_buf;
        
        IF->TraceSetContext(Opt->Mode, s, Node);
        for (int i = 0; i < NumOPC; i++)
        {
            if (env->gen_opc_instr_start[i] == 1)
                IF->InsertSeparator();
            TCGOpcode opc = (TCGOpcode)gen_opc_buf[i];
            Args += (IF->*OpcFn[opc])(Args);
        }
        
        Trace->addGuestICount(tb->icount);
        Trace->addGuestSize(tb->size);
    }

    IF->TraceFinalize();
    
    /* JIT host code. */
    IF->Compile();

    Trace->Commit((unsigned long)CallBack->Code, CallBack->Size);

    TranslatedCode *TC = new TranslatedCode;
    TC->Count = Opt->Count;
    TC->PC = pc;
    TC->CSBase = EntryTB->cs_base;
    TC->CKey = CodeKey(pc, EntryTB->cs_base);
    TC->Code = CallBack->Code;
    TC->Size = CallBack->Size;
    TC->Func = IF->getFunction();
    TC->Info = Trace;
    TC->Patch = NULL;

    if (Env->is_multithreaded()) llvm_spin_lock(&llvm_global_lock);

    TCodeMap &CodeRegions = Env->getCodeRegions();
    CodeRegions[EntryTB] = TC;

    if (Env->is_multithreaded()) llvm_spin_unlock(&llvm_global_lock);

    dbo_register_region(Trace);

    return TC;
}

/*
 * RebuildCFG()
 *  Traverse the CFG to exclude the disjoint nodes.
 */
static int RebuildCFG(OptimizationInfo *Opt)
{
    Node2Map NodeMapping;
    NodeVec VisitStack;
    GraphNode *CFG = Opt->getCFG();

    VisitStack.push_back(CFG);
    while(VisitStack.size())
    {
        GraphNode *Parent = VisitStack.back();
        VisitStack.pop_back();

        if (NodeMapping.count(Parent))
            continue;

        NodeMapping[Parent] = new GraphNode(Parent->getTB());

        TranslationBlock *TB = Parent->getTB();
        NodeVec &Child = Parent->getSuccessor();
        for (int i = 0, e = Child.size(); i < e; i++)
        {
            GraphNode *Node = Child[i];
            target_ulong PC = Node->getGuestPC();

            /* If the next node is the head of another region, exclude it. */
            if (Node->getTB()->mode == 2 || Node->getTB()->tb_gen_pending == 1)
                continue;

            /* If the parent's terminator instruction is an indirect branch
             * (jmp_pc = -1), the child node is included. If the start pc of
             * the child node is the branch target of the parent, this child
             * node is include as well. */
            if (TB->jmp_pc[0] == (target_ulong)-1 ||
                    TB->jmp_pc[0] == PC || TB->jmp_pc[1] == PC)
                VisitStack.push_back(Node);
        }
    }

    /* Rebuild the links of the new CFG. */
    for (Node2Map::iterator I = NodeMapping.begin(), E = NodeMapping.end();
            I != E; I++)
    {
        GraphNode *Old = I->first;
        GraphNode *New = I->second;
        NodeVec &Child = Old->getSuccessor();
        for (int i = 0, e = Child.size(); i < e; i++)
        {
            if (NodeMapping.count(Child[i]))
                New->setSuccessor(NodeMapping[Child[i]]);
        }
    }

    Opt->setCFG(NodeMapping[CFG]);
    DeleteCFG(CFG);

    return NodeMapping.size();
}

/*
 * SearchCycle()
 *  Perform the DFS search to find any cyclic path.
 */
GraphNode *SearchCycle(map<TranslationBlock*, GraphNode*> &Visited,
        TranslationBlock *TB, int Depth)
{
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();
    GraphNode *Pred = NULL;
    GraphNode *Succ;

    vector<TranslationBlock*> &Successor = GlobalCFG->getSuccessor(TB);
    for (int i = 0, e = Successor.size(); i < e; i++)
    {
        TranslationBlock *SuccTB = Successor[i];
        if (SuccTB->invalidated == 1 || SuccTB == TB)
            continue;

        if (Visited.count(SuccTB))
        {
            Succ = Visited[SuccTB];
            goto build_link;
        }
        if (Depth < MAX_SEARCH_DEPTH - 1)
        {
            Succ = SearchCycle(Visited, SuccTB, Depth+1);
            if (Succ != NULL)
                goto build_link;
        }

        continue;

build_link:
        if (Pred == NULL)
            Visited[TB] = Pred = new GraphNode(TB);
        Pred->setSuccessor(Succ);
    }

    return Pred;
}

/*
 * ExpandTrace()
 *  Expand a NET trace to a bigger region with the NETPlus algorithm.
 *   NETPlus: trace formation algorithm based on the paper published in
 *   RESoLVE'11. D. Davis and K. Hazelwood, "Improving Region Selection Through
 *   Loop Completion," in ASPLOS Workshop on Runtime Environments/Systems,
 *   Layering, and Virtualized Environments, 2011.
 */
void ExpandTrace(OptimizationInfo *Opt)
{
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();
    int LoopIdx = Opt->getLoopIdx();
    NodeVec &CFGVec = Opt->getCFGVec();
    int Count = CFGVec.size();
    TranslationBlock *Head = (LoopIdx == -1) ? NULL : CFGVec[LoopIdx]->getTB();
    map<TranslationBlock*, GraphNode*> Visited;

    for (int i = 0; i < Count; i++)
    {
        GraphNode *Node = CFGVec[i];
        Visited[Node->getTB()] = Node;
    }

    llvm_spin_lock(&GlobalCFG->Lock);
    for (int i = 0; i < Count; i++)
    {
        GraphNode *Pred = CFGVec[i];

        TranslationBlock *TB = Pred->getTB();

#ifdef ASSERT
        if (TB->invalidated == 1)
            DM->Error("%s: internal error.\n", __func__);
#endif

        /* Get the successor nodes that had been executed. */
        vector<TranslationBlock*> &Successor = GlobalCFG->getSuccessor(TB);

        TranslationBlock *NextTB = (i == Count-1) ? Head : CFGVec[i+1]->getTB();
        for (int j = 0, e = Successor.size(); j < e; j++)
        {
            TranslationBlock *SuccTB = Successor[j];
            if (SuccTB->invalidated == 1)
                continue;

            if (Visited.count(SuccTB))
            {
                if (SuccTB != NextTB)
                    Pred->setSuccessor(Visited[SuccTB]);
                continue;
            }

            /* If we go here, this successor is not among NET's executing tail.
               Try finding any cyclic path with forward search. */
            GraphNode *Succ = SearchCycle(Visited, SuccTB, 0);
            if (Succ != NULL)
                Pred->setSuccessor(Succ);
        }
    }

    Opt->Count = RebuildCFG(Opt);
    llvm_spin_unlock(&GlobalCFG->Lock);
}

/*
 * TraceChaining()
 *  Make a jump from the head block in the block code cache to the translated
 *  host code of this region in the optimized code cache. Also patch previous
 *  built regions that have direct branch to this region.
 */
static void TraceChaining(TranslationBlock *EntryTB, TranslatedCode *TC)
{
    TraceChainMap &TraceChain = Env->getTraceChain();
    tcg_target_ulong Code = (tcg_target_ulong)(long)TC->Code;

    patch_jmp(tb_get_jmp_entry(EntryTB), Code);

    if (Env->is_multithreaded()) llvm_spin_lock(&llvm_global_lock);

    vector<uintptr_t> &AddrList = TraceChain[EntryTB->pc];
    for (vector<uintptr_t>::iterator I = AddrList.begin(), E = AddrList.end();
            I != E; I++)
    {
        patch_jmp(*I, Code);
    }

    if (Env->is_multithreaded()) llvm_spin_unlock(&llvm_global_lock);

    EntryTB->mode = 2;
    EntryTB->opt_ptr = (uint8_t*)Code;
    TC->isPatched = true;
}

/*
 * helper_cpu_gen_trace()
 *  The helper routine to translate a region.
 */
static int helper_cpu_gen_trace(LLVMTranslator *Translator, CPUState *env, 
        OptimizationInfo *Opt, uint32_t Mode)
{
    TCGContext *s = tcg_ctx_env;
    TranslatedCode *TC = NULL;
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();
    TranslationBlock *EntryTB = Opt->CFG->getTB();

    /* If the trace has already existed and we do not force the regeneration,
       skip this trace. */
    if (Mode == LLVM_GEN_TRACE)
    {
        int ret = llvm_testandset(&EntryTB->tb_gen_pending);
        if (ret == 1)
            goto skip;
    }

    if (GlobalCFG != NULL && Mode != LLVM_REGEN_TRACE)
        ExpandTrace(Opt);

    TC = Translator->GenTrace(s, env, Opt);
    if (TC == NULL || llvm_flush == 1 || tb_flush_version != Opt->CacheVersion)
        goto skip;

    /* Perform block-trace and trace-trace chaining. */
    TraceChaining(EntryTB, TC);

skip:
    delete Opt;

    return 0;
}

/*
 * CheckOptimization()
 *  Poll the translation queue for any optimization request.
 */
static inline bool CheckOptimization(LLVMTranslator *Translator, CPUState *env)
{
    OptimizationInfo *Opt = (OptimizationInfo *)dequeue(TraceGenQueue);
    if (Opt == NULL)
        return false;
    if (tb_flush_version != Opt->CacheVersion)
    {
        delete Opt;
        return false;
    }
    
    switch (Opt->Mode)
    {
        case LLVM_GEN_TRACE:
        case LLVM_REGEN_TRACE:
            {
                int64_t start_cycle, end_cycle;
                start_cycle = cpu_get_real_ticks();
                helper_cpu_gen_trace(Translator, env, Opt, Opt->Mode);
                end_cycle = cpu_get_real_ticks();
                PF->addTraceCycles(end_cycle - start_cycle);
                break;
            }
        default:
            break;
    }
    
    return true;
}

/*
 * ComposeNETTrace()
 *  Compose a NET trace. A NET trace is with the CFG of either simple-loop or
 *  a fall-through path.
 */
static OptimizationInfo *ComposeNETTrace(TranslationBlock **TBs, int Count,
        int LoopIdx)
{
    OptimizationInfo *Opt;
    GraphNode *Pred = NULL;

    Opt = new OptimizationInfo(LLVM_GEN_TRACE, Count, LoopIdx, 0, tb_flush_version);

    NodeVec &CFGVec = Opt->getCFGVec();
    for (int i = 0; i < Count; i++)
    {
        GraphNode *Node = new GraphNode(TBs[i]);
        if (i != 0)
            Pred->setSuccessor(Node);
        Pred = Node;
        CFGVec.push_back(Node);
    }

    if (LoopIdx != -1)
        Pred->setSuccessor(CFGVec[LoopIdx]);
    
    Opt->setCFG(CFGVec[0]);

    return Opt;
}

TranslatedCode::~TranslatedCode()
{
    if (PF->getProfile() == PROFILE_NONE)
        delete Info;

    if (HybridMode == HYBRID_SERVER)
        delete Patch;
    if (HybridMode == HYBRID_CLIENT)
        return;

    Func->removeFromParent();
    delete Func;
}

/*
 * LLVM C++ interfaces for C.
 */
#ifdef __cplusplus
extern "C"
{
#endif

/*
 * llvm_gen_init()
 *  Wrapper function to initialize the LLVM translation environment.
 */
int llvm_gen_init(CPUState *env, void *tcg_context)
{
    LLVMEnv::createLLVMEnv(env, tcg_context);
    return 0;
}

/*
 * llvm_gen_finalize()
 *  Wrapper function to destroy the LLVM translation environment.
 */
int llvm_gen_finalize(void)
{
    if (LLVMEnv::InitOnce == false)
        return 1;

    delete Env;

    return 0;
}

/*
 * llvm_gen_block()
 *  Wrapper function to translate a block of code.
 */
int llvm_gen_block(CPUState *env, void *tcg_context, TranslationBlock *tb)
{
    LLVMTranslator *Translator = Env->getSingleTranslator();

    TranslatedCode *TC = Translator->GenBlock((TCGContext *)tcg_context,
            env, tb);
    tb->tc_ptr = TC->Code;
    
    return TC->Size;
}

/*
 * llvm_gen_trace()
 *  Wrapper function to translate a region of code.
 */
int llvm_gen_trace(CPUState *env, TranslationBlock **predicted_tb, int count,
        int loop_idx)
{
    int64_t start_cycle, end_cycle;
    OptimizationInfo *Opt;

    if (LLVMEnv::InitOnce == false)
        return 1;
    if (llvm_flush == 1)
        return 0;

    Opt = ComposeNETTrace(predicted_tb, count, loop_idx);

    if (HybridMode == HYBRID_SINGLE)
    {
        LLVMTranslator *Translator = Env->getSingleTranslator();

        llvm_spin_lock(&llvm_global_lock);
        start_cycle = cpu_get_real_ticks();
        env->gen_trace = 1;

        helper_cpu_gen_trace(Translator, env, Opt, Opt->Mode);
        for (int i = 0; i < MaxRegenJob; i++)
        {
            bool ret = CheckOptimization(Translator, env);
            if (ret == false)
                break;
        }

        env->gen_trace = 0;
        end_cycle = cpu_get_real_ticks();
        PF->addTraceCycles(end_cycle - start_cycle);
        llvm_spin_unlock(&llvm_global_lock);
    }
    else if (HybridMode == HYBRID_MULTIPLE)
    {
        /* Push the optimization request to the translation queue and
         * continue. */
        enqueue(TraceGenQueue, Opt);
    }
    else if (HybridMode == HYBRID_CLIENT)
    {
        net_gen_trace(env, Opt, false);
        for (int i = 0; i < MaxRegenJob; i++)
        {
            Opt = (OptimizationInfo *)dequeue(TraceGenQueue);
            if (Opt == NULL)
                break;
            net_gen_trace(env, Opt, true);
        }
    }

    return 0;
}

/*
 * llvm_regen_trace()
 *  Wrapper function to insert a region retranslation request in the queue.
 */
int llvm_regen_trace(GraphNode *CFG, int count, int version)
{
    DM->Debug(DEBUG_LLVM, "%s entered.\n", __func__);

    OptimizationInfo *Opt = new OptimizationInfo(LLVM_REGEN_TRACE, count, 
            -1, version, tb_flush_version, CFG);
    enqueue(TraceGenQueue, Opt);

    return 0;
}

/*
 * llvm_tb_flush()
 *  Wrapper fucntion to flush the optmizated code cache.
 */
int llvm_tb_flush(void)
{
    if (LLVMEnv::InitOnce == false)
        return 1;

    llvm_spin_lock(&llvm_global_lock);

    /* Reset DBO and network services. */
    dbo_reset_monitoring();

    /* Flush network request */
    net_flush();

    /* Clear pending optimization requests. */
    reset_queue(TraceGenQueue);

    /* Remove all translated blocks and regions. */
    TCodeMap &CodeBlocks = Env->getCodeBlocks();
    TCodeMap &CodeRegions = Env->getCodeRegions();
    TraceChainMap &TraceChain = Env->getTraceChain();
    ChainVec &ChainPoint = Env->getChainPoint();

    for (TCodeMap::iterator I = CodeBlocks.begin(), E = CodeBlocks.end(); I != E; I++)
        delete I->second;
    for (TCodeMap::iterator I = CodeRegions.begin(), E = CodeRegions.end(); I != E; I++)
        delete I->second;

    CodeBlocks.clear();
    CodeRegions.clear();
    TraceChain.clear();
    ChainPoint.clear();

    /* Clear global cfg. */
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();
    GlobalCFG->reset();

    /* Flush trace cache. */
    DefaultJITMemoryManager *MM = Env->getMemoryManager();
    MM->Flush();

    Env->incFlushCount();
    llvm_flush = 0;

    llvm_spin_unlock(&llvm_global_lock);

    DM->Debug(DEBUG_LLVM, "%s: trace cache flushed.\n", __func__);

    return 0;
}

/*
 * llvm_tb_record()
 *  Wrapper function to keep a record of a translated block.
 */
int llvm_tb_record(TranslationBlock *tb, int host_size, int64_t elapse)
{
    if (LLVMEnv::InitOnce == false)
        return 1;

    BlockInfo *Block = new BlockInfo();
    Block->Commit(tb, host_size);

    PF->addBlock(Block, elapse);

#if defined(CONFIG_HPM)
    if (Block->GuestICount)
        dbo_register_region(Block);
#endif

    return 0;
}

/*
 * llvm_tb_alloc()
 *  Wrapper function to determine if the optimized code cache is full.
 */
int llvm_tb_alloc()
{
    if (LLVMEnv::InitOnce == false)
        return 1;

    int ret;
    llvm_spin_lock(&llvm_global_lock);
    ret = Env->getMemoryManager()->isSizeAvailable();
    llvm_spin_unlock(&llvm_global_lock);

    return ret;
}

/*
 * llvm_tb_alloc_size()
 *  Wrapper function to determine if the optimizaed code cache has enough
 *  `size'.
 */
int llvm_tb_alloc_size(int size)
{
    if (LLVMEnv::InitOnce == false)
        return 1;

    return Env->getMemoryManager()->isSizeAvailable(size);
}

/*
 * llvm_build_cfg()
 *  Wrapper function to add a path in the GlobalCFG.
 */
int llvm_build_cfg(TranslationBlock *src, TranslationBlock *dst)
{
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();

    if (LLVMEnv::InitOnce == false)
        return 1;
    if (unlikely(GlobalCFG == NULL))
        return 0;

    llvm_spin_lock(&GlobalCFG->Lock);
    GlobalCFG->addEdge(src, dst);
    llvm_spin_unlock(&GlobalCFG->Lock);

    return 0;
}

/*
 * llvm_resolve_address()
 *  Given the value returned when leaving the code cache, return the patch
 *  address for the region chaining.
 */
int llvm_resolve_address(unsigned long *addr)
{
    if (unlikely(LLVMEnv::InitOnce == false))
        return 1;

    unsigned long val = *addr;
    if (val == 0)
        return 0;
    
    ChainVec &ChainPoint = Env->getChainPoint();
    unsigned long Key = val >> 2;
    llvm_spin_lock(&llvm_interface_lock);
    *addr = ChainPoint[Key];
    llvm_spin_unlock(&llvm_interface_lock);
    
    return 0;
}

/*
 * llvm_fork_start()
 *  Wrapper function to stop the optimization service before performing fork.
 */
void llvm_fork_start(void)
{
    dbo_fork_start();
    net_fork_start();

    /* Return if threads are inactive already. */
    if (isThreadActive == false)
        return;

    int NumThread = Env->getNumTranslator();
    pthread_t *HelperThread = Env->getHelperThread();

    isThreadActive = false;
    for (int i = 0; i < NumThread; i++)
        pthread_join(HelperThread[i], NULL);
}

/*
 * llvm_fork_end()
 *  Wrapper function to restart the optimization service after performing fork.
 */
void llvm_fork_end(int child)
{
    dbo_fork_end(child);
    net_fork_end(child);

    if (child == 0)
    {
        Env->SpawnThread(Env->getNumTranslator(), WorkerFunc);
    }
    else
    {
        isThreadActive = false;

        llvm_lock_init(&llvm_global_lock);
        llvm_lock_init(&llvm_interface_lock);
        llvm_lock_init(&llvm_debug_lock);
    }
}

/*
 * llvm_update_ibtc_entry()
 *  Wrapper function to perform Pin's IB chaining.
 */
int llvm_update_ibtc_entry(int idx, TranslationBlock *tb)
{
#if defined(ENABLE_IBCHAIN)
    IBChainVec &IBChainPoint = Env->getIBChainPoint();

    llvm_spin_lock(&llvm_interface_lock);
    IBChainInfo *ChainInfo = IBChainPoint[idx];
    llvm_spin_unlock(&llvm_interface_lock);

    int Num = ChainInfo->getChainSize();
    if (Num != 0 && ChainInfo->isPCChained(tb->pc))
        return 1;

    if (Num == IBCHAIN_MAX_SIZE)
        DM->Error("%s: internal error on chain size.\n", __func__);
    if (ibchain_gen_end - ibchain_gen_ptr < 128)
        DM->Error("%s: internal error on cache size.\n", __func__);

    uint16_t PatchOffset;
    uint8_t *IBChain = ibchain_gen_ptr;
    int Size = tcg_out_ibchain(ibchain_gen_ptr, tb, &PatchOffset);
    ibchain_gen_ptr = (uint8_t *)(((uintptr_t)ibchain_gen_ptr + Size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));

    flush_icache_range((unsigned long)IBChain, (unsigned long)IBChain + Size);

    ChainInfo->addChain(IBChain, PatchOffset, tb->pc);

#ifdef IBCHAIN_INSERT_TAIL
    if (Num == IBCHAIN_MAX_SIZE - 1)
    {
        /* If this chain reaches its maximum length, force the IB target
         * translation to look up the global hash table. */
        patch_jmp((uintptr_t)IBChain + PatchOffset,
                (uintptr_t)ibtc_helper_entry);
    }

    uintptr_t PatchAddr = ChainInfo->getPatchAddr(Num-1);

    patch_jmp(PatchAddr, (uintptr_t)IBChain);
#else
    if (Num == IBCHAIN_MAX_SIZE - 1)
        patch_jmp(ChainInfo->getPatchAddr(0), (uintptr_t)ibtc_helper_entry);

    if (Num != 0)
    {
        /* If this chain reaches its maximum length, force the IB target
         * translation to look up the global hash table. */
        patch_jmp((uintptr_t)IBChain + PatchOffset,
                (uintptr_t)ChainInfo->getChain(Num-1));
    }

    patch_jmp(ChainInfo->getPatchAddr(-1), (uintptr_t)IBChain);
#endif
#endif

    return 0;
}

/*
 * copy_tcg_context()
 *  Copy the global TCG context to the thread's local TCG context.
 */
void copy_tcg_context(CPUState *env)
{
    tcg_ctx_env = (TCGContext *)llvm_malloc(sizeof(TCGContext));
    memcpy(tcg_ctx_env, &tcg_ctx, sizeof(TCGContext));
    tcg_ctx_env->temps = tcg_ctx_env->static_temps;

    if (env != NULL)
    {
        env->gen_opc_buf = (uint16_t *)
            llvm_malloc(OPC_BUF_SIZE * sizeof(uint16_t));
        env->gen_opparam_buf = (TCGArg *)
            llvm_malloc(OPPARAM_BUF_SIZE * sizeof(TCGArg));
        env->gen_opc_instr_start = (uint8_t *)
            llvm_malloc(OPC_BUF_SIZE * sizeof(uint8_t));
    }
}

/*
 * reset_tcg_context()
 */
void reset_tcg_context()
{
    /* Set nb_helpers to zero so helper functions are not optimized
     * for the first initialization. */
    TCGContext *s = tcg_ctx_env;
    if (s == NULL)
        DM->Error("%s: null tcg context.\n", __func__);

    s->nb_globals = 0;
    s->nb_helpers = 0;
    s->frame_reg = TCG_AREG0;
}

#ifdef __cplusplus
}
#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
