/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 *
 *   This file provides LLVM IR generator in terms of basic block and trace.
 */

#include "llvm/Module.h"
#include "llvm/ValueSymbolTable.h"
#include "llvm/Instructions.h"
#include "llvm/InlineAsm.h"
#include "llvm/Intrinsics.h"
#include "llvm/Attributes.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm-pass.h"
#include "llvm-profile.h"
#include "llvm-opc.h"
#include "llvm-wrapper.h"
#include "utils.h"

#define INLINE_THRESHOLD    100     /* max # inlined instructions */
#define INLINE_INSTCOUNT    20      /* max instruction count for inlining a small function */

#define IRDebug(idx)    \
            DM->Debug(DEBUG_ENTRY, "op_%s: %d %d %d\n", llvm_op_defs[idx].name,  \
            llvm_op_defs[idx].nb_oargs, llvm_op_defs[idx].nb_iargs, \
            llvm_op_defs[idx].nb_cargs)

#define IRError(fmt,args...)  DM->Error(fmt,##args)

#ifdef ASSERT
#define AssertPointer(v)                                            \
    do {                                                            \
        if ((v)->getType()->isPointerTy() == false)                 \
            IRError("%s: pointer type is required\n", __func__);    \
    } while(0)

#define AssertNotPointer(v)                                         \
    do {                                                            \
        if ((v)->getType()->isPointerTy() == true)                  \
            IRError("%s: scalar type is required\n", __func__);     \
    } while(0)

#define AssertType(c)                                               \
    do {                                                            \
        if (!(c))                                                   \
            IRError("%s: invalid type.\n", __func__);               \
    } while(0)
#else
#define AssertPointer(v) 
#define AssertNotPointer(v)
#define AssertType(c)
#endif


static TCGOpDef llvm_op_defs[] = {
#define DEF(s, oargs, iargs, cargs, flags) \
    { #s , oargs, iargs, cargs, iargs + oargs + cargs, flags },
#include "tcg-opc.h"
#undef DEF
};

static FuncPtr LLVM_Fn[] = {
#define DEF(name, oargs, iargs, cargs, flags) &IRFactory::op_ ## name,
#include "tcg-opc.h"
#undef DEF
};

enum
{
    CHAINING_TRACE_BLOCK = 0,
    CHAINING_TRACE_SELF,
};

enum
{
    COHERENCE_NONE = 0,
    COHERENCE_GLOBAL,
    COHERENCE_ALL,
};

extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern ProfileFactory *PF;
extern CodeGenOpt::Level OptLevel;
extern llvm_lock_t llvm_global_lock;
extern llvm_lock_t llvm_interface_lock;
extern llvm_lock_t llvm_debug_lock;

/*
 * IRFactory()
 *  Factory to translate TCG IR to LLVM IR.
 */
IRFactory::IRFactory(TCGContext *s, LLVMTranslator *Trans) : Context(Trans->getContext()),
    HelperFuncs(Trans->getHelperFuncs()), UID(0), Translator(Trans)
{
    Mod = Translator->getModule();
    EE = Translator->getExecutionEngine();
    CallBack = Translator->getCallBackInfo();
    TD = Translator->getTargetData();

    BaseRegNo = Translator->BaseRegNo;
    BaseRegStr = Translator->BaseRegStr;
    BaseRegType = Translator->BaseRegType;

    InitBasicTypes();
    InitContext(s);

    TBuilder = new TraceBuilder;

    /* Setup QEMU reserved registers. */
    SetBase(&BaseReg[BaseRegNo], BaseRegStr);

    /* Setup function passes.
       Do not change the order of the optimization passes. */
    Passes = new FunctionPassManager*[NUM_STAGE];
    for (int i = 0; i < NUM_STAGE; i++)
        Passes[i] = new FunctionPassManager(Mod);

    Passes[STAGE_ONE]->add(createProfilePass(this));

    Passes[STAGE_TWO]->add(createStateMappingPass(this));
    Passes[STAGE_TWO]->add(createPromoteMemoryToRegisterPass());

    Passes[STAGE_THREE]->add(createHoistCommonCodePass());
    Passes[STAGE_THREE]->add(createPHItoSelectPass());

    DM->Debug(DEBUG_LLVM, "LLVM IR Factory initialized.\n");
}

IRFactory::~IRFactory()
{
    delete TBuilder;
    delete FrameReg;
    delete [] Reg;
    delete [] Passes;
}

/*
 * getOpcFn()
 *  Get the function pointer of the IR converion routines.
 */
void *IRFactory::getOpcFn()
{
    return LLVM_Fn;
}

/*
 * getUID()
 *  Return IR factory's unique ID in terms of LLVM Value.
 */
Constant *IRFactory::getUID()
{
    if (UID == 0)
        UID++;
    return CONST32(UID++);
}

/*
 * InitBasicTypes()
 *  Initialize basic types that will be used during IR conversion.
 */
void IRFactory::InitBasicTypes()
{
    BasicType &BT = Translator->BT;

    VoidTy  = BT.VoidTy;
    Int8Ty  = BT.Int8Ty;
    Int16Ty = BT.Int16Ty;
    Int32Ty = BT.Int32Ty;
    Int64Ty = BT.Int64Ty;

    IntPtrTy   = BT.IntPtrTy;
    Int8PtrTy  = BT.Int8PtrTy;
    Int16PtrTy = BT.Int16PtrTy;
    Int32PtrTy = BT.Int32PtrTy;
    Int64PtrTy = BT.Int64PtrTy;
}

/*
 * InitContext()
 *  Initialize TCG's context including frame info and pseudo registers.
 */
void IRFactory::InitContext(TCGContext *s)
{
    nb_globals = s->nb_globals;

    FrameStart = s->frame_start;
    FrameReg = new Register;
    FrameReg->setBase(s->frame_reg);
    FrameReg->setType(32, Int32Ty);
    FrameReg->setOff(s->frame_start);
    FrameReg->setName("frame");
    FrameReg->setGlobal(false);

    Reg = new Register[TCG_MAX_TEMPS];
    for (int i = 0; i < nb_globals; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        if (tmp->type != TCG_TYPE_I32 && tmp->type != TCG_TYPE_I64)
            DM->Error("%s: unsupported register type.\n", __func__);

        int size = (tmp->type == TCG_TYPE_I32) ? 32 : 64;
        CONST IntegerType *type = (tmp->type == TCG_TYPE_I32) ? Int32Ty : Int64Ty;
        if (tmp->fixed_reg)
        {
            Reg[i].setBase(tmp->reg);
            Reg[i].setOff(0);
        }
        else
        {
            Reg[i].setBase(tmp->mem_reg);
            Reg[i].setOff(tmp->mem_offset);

        }
        Reg[i].setType(size, type);
        Reg[i].setName(tmp->name);
        Reg[i].setGlobal(true);
    }
}

/*
 * InitFunction()
 *  Prepare the LLVM Function and arrange the initial BasicBlocks and variable
 *  declaration.
 */
void IRFactory::InitFunction(target_ulong pc, int mode)
{
    char Fname[64] = {'\0'};
    vector<int> RegVec;
    CONST Type *ReturnTy;

#if defined(TCG_TARGET_I386)
    ReturnTy = VoidTy;
#elif defined(TCG_TARGET_PPC64)
    ReturnTy = Int64Ty;
#else
    ReturnTy = Int32Ty;
#endif

    /* Reset data structures. */
    AddrCache.clear();
    TypeCache.clear();
    toErase.clear();
    Separator.clear();
    InlineCalls.clear();
    NoInlineCalls.clear();
    CallIdx = 0;

    for (int i = 0; i < TCG_TARGET_NB_REGS; i++)
        BaseReg[i].Base = NULL;

    Mode = mode;

    sprintf(Fname, "%"PRIx, pc);
    FunctionType *FT = FunctionType::get(ReturnTy, false);
    Func = FUNC(FT, Fname);
    Func->setCallingConv(CallingConv::C);
#if defined(TCG_TARGET_I386)
    Func->addFnAttr(FNATTR(NoReturn));
#endif
    Func->addFnAttr(FNATTR(NoUnwind));
    Func->addFnAttr(FNATTR(Naked));

    /* Prepare all basic blocks. */
    InitBB = BasicBlock::Create(Context, "init", Func);
    CurrBB = BasicBlock::Create(Context, "entry", Func);
    ExitBB = BasicBlock::Create(Context, "exit", Func);
    InitLastInst = BranchInst::Create(CurrBB, InitBB);
    new UnreachableInst(Context, ExitBB);

    /* Setup environment registers. */
    RegVec.push_back(BaseRegNo);
    for (vector<int>::iterator I = RegVec.begin(), E = RegVec.end();
            I != E; I++)
    {
        int basereg = *I;
        CallInst *CI = CreateCall(BaseReg[basereg].Reg,
                BaseReg[basereg].Name, InitLastInst);
        CI->setDoesNotThrow();
        BaseReg[basereg].Base = CI;
    }

    char Constraint[16] = {'\0'};
    sprintf(Constraint, "={%s}", BaseRegStr);

    CPU = BaseReg[BaseRegNo].Base;
    CPU->setName("cpu");
    CPUStructAsm = InlineAsm::get(FunctionType::get(BaseRegType, false),
            "", Constraint, true);
    CPUStruct = new BitCastInst(CPU, BaseRegType, "cpu.struct", InitLastInst);
    InitGEPInst = new UnreachableInst(Context, InitLastInst);

    toErase.push_back(InitGEPInst);
}

/*
 * BlockSetContext()
 *  Initialize the context for a new translation with the block-level
 *  translation.
 */
void IRFactory::BlockSetContext(int mode, TCGContext *s, target_ulong pc)
{
    DM->Debug(DEBUG_LLVM, "%s entered.\n", __func__);

    InitFunction(pc, mode);
    LastInst = BranchInst::Create(ExitBB, CurrBB);

    Code = new BlockInfo();

    nb_temps = s->nb_temps;
    /* Check if the register has legal type. */
    for (int i = 0; i < nb_temps; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        if (tmp->type != TCG_TYPE_I32 && tmp->type != TCG_TYPE_I64)
            DM->Error("%s: unsupported register type.\n", __func__);
    }

    /* Initialize global registers. */
    for (int i = 0; i < nb_globals; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        Register *reg = &Reg[i];

        if (tmp->fixed_reg)
            reg->setMode(VAL_REV | VAL_MEM);
        else
            reg->setMode(VAL_MEM);

        int size = (tmp->type == TCG_TYPE_I32) ? 32 : 64;
        CONST IntegerType *type = (tmp->type == TCG_TYPE_I32) ? Int32Ty : Int64Ty;
        reg->reset(size, type);
    }

    /* Initialize temporary registers. */
    for (int i = nb_globals; i < nb_temps; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        Register *reg = &Reg[i];

        if (tmp->temp_local)
            reg->setMode(VAL_LOCAL);
        else
            reg->setMode(VAL_TMP);

        int size = (tmp->type == TCG_TYPE_I32) ? 32 : 64;
        CONST IntegerType *type = (tmp->type == TCG_TYPE_I32) ? Int32Ty : Int64Ty;
        reg->reset(size, type);
    }

    for (int i = 0; i < TCG_MAX_LABELS; i++)
        Labels[i] = NULL;

    FrameReg->Off = FrameStart;
}

/*
 * Init()
 *  For a new region, this function is called to reset the data structures of
 *  the new translation. The CFG of the region is parsed to calculate the depth
 *  for each CFG node.
 */
CodeInfo *TraceBuilder::Init(GraphNode *cfg, int version)
{
    NodeVec VisitStack;
    NodeSet Visited;

    Trace = new TraceInfo(version, PF->getUID());

    reset();

    CFG = cfg;
    Depth[CFG] = 0;
    VisitStack.push_back(CFG);
    do
    {
        GraphNode *Node = VisitStack.back();
        VisitStack.pop_back();
        Visited.insert(Node);

        setUniqueNode(Node);

        int depth = Depth[Node] + 1;
        NodeVec &Successor = Node->getSuccessor();
        for (int i = 0, e = Successor.size(); i < e; i++)
        {
            if (Visited.count(Successor[i]) != 0)
                continue;

            Depth[Successor[i]] = depth;
            VisitStack.push_back(Successor[i]);
        }
    } while (!VisitStack.empty());

    /* Add the first node to be processed. */
    NodeQueue.push_back(CFG);

    return Trace;
}

/*
 * setSuccessor()
 *  Wrapper function to set an unconditional branch.
 */
inline void IRFactory::setSuccessor(BranchInst *BI, BasicBlock *BB)
{
#if defined(LLVM_V28)
    BI->setUnconditionalDest(BB);
#else
    BI->setSuccessor(0, BB);
#endif
}

/*
 * TraceInit()
 *  Reset the trace builder for a new region formation.
 */
void IRFactory::TraceInit(target_ulong PC, int Count, GraphNode *CFG,
        int Version)
{
    DM->Debug(DEBUG_LLVM, "%s: predict trace pc 0x%"PRIx" length %d "
            "version %d\n", __func__, PC, Count, Version);

    Code = TBuilder->Init(CFG, Version);
}

/*
 * TraceFinalize()
 *  Finalize the translation of a region.
 */
void IRFactory::TraceFinalize()
{
    TraceInfo *Trace = TBuilder->getTrace();
    NodeMap &Depth = TBuilder->getDepth();
    NodeBBMap &Node2BB = TBuilder->getNode2BB();

    /* Reconnect links of basic blocks. The links are previously 
       set to exit BB. */
    BranchVec &Branches = TBuilder->getBranches();
    for (int i = 0, e = Branches.size(); i < e; i++)
    {
        pair<Instruction*, GraphNode*> &Branch = Branches[i];
        BranchInst *BI = static_cast<BranchInst*>(Branch.first);
        GraphNode *Node = Branch.second;
        setSuccessor(BI, Node2BB[Node]);
    }

    /* Rebuild the CFG */
    NodeVec NodeToRemove;
    for (NodeMap::iterator I = Depth.begin(), E = Depth.end(); I != E; I++)
    {
        GraphNode *Node = I->first;
        if (Node2BB.count(Node))
            Node->getSuccessor().clear();
        else
            NodeToRemove.push_back(Node);
    }
    for (int i = 0, e = NodeToRemove.size(); i < e; i++)
        delete NodeToRemove[i];

    NPSet &Links = TBuilder->getLinks();
    for (NPSet::iterator I = Links.begin(), E = Links.end(); I != E; I++)
    {
        GraphNode *Src = (*I).first;
        GraphNode *Dst = (*I).second;
        Src->setSuccessor(Dst);
    }

    Trace->NumBlock = Node2BB.size();
    Trace->CFG = TBuilder->getEntryNode();
    Trace->GuestPC = Trace->CFG->getGuestPC();

#if defined(ASSERT)
    NodeVec VisitStack;
    NodeSet Visited; 
    VisitStack.push_back(Trace->CFG);
    do 
    {
        GraphNode *Node = VisitStack.back();
        VisitStack.pop_back();

        if (Visited.count(Node))
            continue;

        Visited.insert(Node);
        NodeVec &Successor = Node->getSuccessor();
        for (int i = 0, e = Successor.size(); i < e; i++)
            VisitStack.push_back(Successor[i]);
    } while (!VisitStack.empty());

    if ((int)Visited.size() != Trace->NumBlock)
        IRError("%s: fatal error.\n", __func__);
#endif
}

/*
 * TraceSetContext()
 *  Initialize the context for a new translation with the region-level
 *  translation.
 */
void IRFactory::TraceSetContext(int mode, TCGContext *s, GraphNode *Node)
{
    target_ulong pc = Node->getGuestPC();
    bool isHead = (Node == TBuilder->getEntryNode());
    char succ_str[1024] = {'\0'};

    NodeVec &Successor = Node->getSuccessor();
    for (int i = 0, e = Successor.size(); i < e; i++)
        sprintf(succ_str, "%s%"PRIx" ", succ_str, Successor[i]->getGuestPC());

    DM->Debug(DEBUG_LLVM, "%s: pc %"PRIx" (%s): %s\n", __func__, pc,
            (isHead) ? "head" : "tail", succ_str);

    if (isHead)
        InitFunction(pc, mode);
    else 
    {
        char Name[64] = {'\0'};
        sprintf(Name, "%"PRIx, pc);
        CurrBB = BASIC(Name);
    }

    LastInst = BranchInst::Create(ExitBB, CurrBB);
    TBuilder->setNode(Node, CurrBB);

    nb_temps = s->nb_temps;
    /* Check if the register has legal type. */
    for (int i = 0; i < nb_temps; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        if (tmp->type != TCG_TYPE_I32 && tmp->type != TCG_TYPE_I64)
            DM->Error("%s: unsupported register type.\n", __func__);
    }

    /* Initialize global registers. */
    for (int i = 0; i < nb_globals; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        Register *reg = &Reg[i];
        
        if (tmp->fixed_reg)
            reg->setMode(VAL_REV | VAL_MEM);
        else
            reg->setMode(VAL_MEM);

        int size = (tmp->type == TCG_TYPE_I32) ? 32 : 64;
        CONST IntegerType *type = (tmp->type == TCG_TYPE_I32) ? Int32Ty : Int64Ty;
        reg->reset(size, type);
    }

    /* Initialize temporary registers. */
    for (int i = nb_globals; i < nb_temps; i++)
    {
        TCGTemp *tmp = &s->temps[i];
        Register *reg = &Reg[i];

        if (tmp->temp_local)
            reg->setMode(VAL_LOCAL);
        else
            reg->setMode(VAL_TMP);

        int size = (tmp->type == TCG_TYPE_I32) ? 32 : 64;
        CONST IntegerType *type = (tmp->type == TCG_TYPE_I32) ? Int32Ty : Int64Ty;
        reg->reset(size, type);
    }

    /* Reset labels. */
    for (int i = 0; i < TCG_MAX_LABELS; i++)
        Labels[i] = NULL;

    FrameReg->Off = FrameStart;
}

/*
 * AnalyzeInlineCost()
 *  The heuristic used to determine if we should inline the helper function
 *  or not.
 */
int IRFactory::AnalyzeInlineCost(CallSite CS)
{
    Function *Callee = CS.getCalledFunction();
    HelperInfo *Helper = HelperFuncs[Callee->getName()];
    int InlineCost = INLINE_THRESHOLD - Helper->Metrics.NumInsts;
    unsigned ArgNo = 0;

    if (Helper->Metrics.NumInsts <= INLINE_INSTCOUNT)
        return 1;
    
    InlineCost *= InlineConstants::InstrCost;
    for (CallSite::arg_iterator I = CS.arg_begin(), E = CS.arg_end();
            I != E; ++I, ++ArgNo) 
    {
        InlineCost -= InlineConstants::InstrCost;
        if (isa<AllocaInst>(I)) 
            InlineCost += Helper->ArgumentWeights[ArgNo].AllocaWeight;
        else if (isa<Constant>(I))
            InlineCost += Helper->ArgumentWeights[ArgNo].ConstantWeight;
    }

    return InlineCost;
}

/*
 * ProcessInline()
 *  Perform helper function inlining.
 */
bool IRFactory::ProcessInline(IVec &Calls, bool NeedAnalysis)
{
    if (Calls.empty())
        return false;

    /* If NeedAnalysis is false, simply inline all functions without any
     * analysis. */
    if (NeedAnalysis == false)
    {
        while (!Calls.empty())
        {
            CallInst *CI = static_cast<CallInst *>(Calls.back());
            Calls.pop_back();
            Instruction *OldInsertPos = Code->getInsertPos(CI->getParent());

            InlineFunctionInfo IFI(0, TD);
            InlineFunction(CI, IFI);

            /* The old BB might be split after inlining, update old insertpos
             * if required. */
            if (OldInsertPos)
                setInsertPos(OldInsertPos);
        }

        return true;
    }

    bool Changed = false;
    while (!Calls.empty())
    {
        CallInst *CI = static_cast<CallInst *>(Calls.back());
        Calls.pop_back();
        Instruction *OldInsertPos = Code->getInsertPos(CI->getParent());
        
        HelperInfo *Helper = HelperFuncs[getNameString(CI->getCalledFunction())];
        if (Helper->mayConflict == false && AnalyzeInlineCost(CallSite(CI)) > 0)
        {
            CI->setCalledFunction(Helper->Func);
            InlineFunctionInfo IFI(0, TD);
            InlineFunction(CI, IFI);
            Changed = true;

            /* The old BB might be split after inlining, update old insertpos
             * if required. */
            if (OldInsertPos)
                setInsertPos(OldInsertPos);
        }
    }

    return Changed;
}

/* 
 * PreProcess()
 *  Format function to a legal format and inline calls. Be sure to make the
 *  function in a well form before doing any furthur optimization (i.e.
 *  inlining calls). Otherwise, the optimization may fail or the result may be
 *  wrong.
 */
void IRFactory::PreProcess()
{
    DM->Debug(DEBUG_LLVM, "%s entered.\n", __func__);

    ProcessErase(toErase);

    /* Insert terminator instruction to basic blocks that branch to the
     * Exit BB. */
    for (pred_iterator PI = pred_begin(ExitBB), PE = pred_end(ExitBB);
            PI != PE; PI++)
    {
        Instruction *InsertPos = (*PI)->getTerminator();
        new UnreachableInst(Context, InsertPos);
        toErase.push_back(InsertPos);
    }
    ProcessErase(toErase);
    ExitBB->eraseFromParent();

    /* Remove unused basic blocks and instructions after the indirect branch
     * instruction. */
    BBVec BBtoErase;
    Function::BasicBlockListType &BBList = Func->getBasicBlockList();
    for (Function::BasicBlockListType::iterator I = BBList.begin(),
            E = BBList.end(); I != E; I++)
    {
        if (I != Func->getEntryBlock() &&
                distance(pred_begin(I), pred_end(I)) == 0)
            BBtoErase.push_back(I);

        if (distance(succ_begin(I), succ_end(I)) != 0)
            continue;

        Instruction *Inst = Code->getInsertPos(I);
        if (Inst == NULL || Inst == I->getTerminator() ||
                dyn_cast<IndirectBrInst>(Inst) == NULL)
            continue;

        for (BasicBlock::iterator BI = ++BasicBlock::iterator(Inst),
                BE = I->end(); BI != BE; BI++)
            toErase.push_back(BI);
    }

    ProcessErase(toErase);

    for (int i = 0, e = BBtoErase.size(); i < e; i++)
        BBtoErase[i]->eraseFromParent();

    if (DM->getDebug() & DEBUG_VERIFY)
        verifyFunction(*Func, VerifierFailureAction(PrintMessageAction));

    /* Inline helper functions. */
    ProcessInline(InlineCalls, false);

    if (Mode == LLVM_GEN_TRACE || Mode == LLVM_REGEN_TRACE)
    {
        SmallVector<std::pair<const BasicBlock*,const BasicBlock*>, 32> BackEdges;
        TraceInfo *Trace = TBuilder->getTrace();
        FindFunctionBackedges(*Func, BackEdges);
        
        for (int i = 0, e = BackEdges.size(); i < e; i++)
        {
            const BasicBlock *BackEdgeBB = BackEdges[i].first;
            const BasicBlock *LoopHeader = BackEdges[i].second;
            Value *Meta[] = { CONST32(2), CONST32(TBuilder->getBBDepth(BackEdgeBB)), };
            TerminatorInst *BI = const_cast<TerminatorInst *>(BackEdgeBB->getTerminator());
            BI->getParent()->setName("loopback");
            BI->setMetadata("exit", CreateMDNode(Context, Meta, 2));

            Trace->LoopHeader.insert(LoopHeader);
        }

        Trace->NumLoop = BackEdges.size();
        DM->Debug(DEBUG_LLVM, "%s: trace formation with pc 0x%"PRIx" length %d is_loop %d\n", 
                __func__, Trace->GuestPC, Trace->NumBlock, (Trace->NumLoop) ? true : false);
    }
}

/* 
 * PostProcess()
 *  Legalize LLVM IR after running the pre-defined passes.
 */
void IRFactory::PostProcess()
{
    DM->Debug(DEBUG_LLVM, "%s entered.\n", __func__);
}

/*
 * runPasses(
 *  Perform optimization passes.
 */
void IRFactory::runPasses()
{
    /* Pass stage 1 */
    DM->Debug(DEBUG_LLVM, "%s: running stage 1.\n", __func__);
    Passes[STAGE_ONE]->run(*Func);

    /* Pass stage 2 */
    DM->Debug(DEBUG_LLVM, "%s: running stage 2 iter 0.\n", __func__);
    Pass = 0;
    Passes[STAGE_TWO]->run(*Func);
    bool Changed = ProcessInline(NoInlineCalls, true);
    if (Changed == true)
    {
        DM->Debug(DEBUG_LLVM, "%s: running stage 2 iter 1.\n", __func__);

        if (Mode == LLVM_GEN_TRACE || Mode == LLVM_REGEN_TRACE)
        {
            SmallVector<pair<const BasicBlock*,const BasicBlock*>, 32> BackEdges;
            TraceInfo *Trace = TBuilder->getTrace();
            FindFunctionBackedges(*Func, BackEdges);
            Trace->LoopHeader.clear();
        
            for (int i = 0, e = BackEdges.size(); i < e; i++)
                Trace->LoopHeader.insert(BackEdges[i].second);
        }
    
        Pass = 1;
        Instruction *InsertPos = ++BasicBlock::iterator(CPU);
        if (InsertPos != CPUStruct)
            CPUStruct = new BitCastInst(CPU, BaseRegType, "cpu.struct", InsertPos);

        Passes[STAGE_TWO]->run(*Func);
    }

    /* Pass stage 3 */
    DM->Debug(DEBUG_LLVM, "%s: running stage 3\n", __func__);
    Passes[STAGE_THREE]->run(*Func);
}

/*
 * Compile()
 *  Start the LLVM JIT compilation.
 */
void IRFactory::Compile()
{
    uint64_t DebugMode = DM->getDebug();

    DM->Debug(DEBUG_LLVM, "Translator %d starts compiling...\n", Translator->getID());

    /* Run optimization passes. */
    PreProcess();
    runPasses();
    PostProcess();

    if (PF->getProfile() & PROFILE_DBO)
    {
        ValueToValueMapTy VMap;
        Code->Func = CloneFunction(Func, VMap, 0);
    }

    if (DebugMode & DEBUG_VERIFY)
        verifyFunction(*Func, VerifierFailureAction(PrintMessageAction));

    if (DebugMode & DEBUG_IR)
    {
        llvm_spin_lock(&llvm_debug_lock);
        Func->dump();
        llvm_spin_unlock(&llvm_debug_lock);
    }

    /* JIT. */
    CallBack->Func = Func;
    CallBack->CI = Code;
    EE->runJITOnFunction(Func);

    Func->deleteBody();

    DM->Debug(DEBUG_LLVM, "%s: done.\n", __func__);
}

/*
 * getPointerTy()
 *  Teturn the PointerType based on the input size.
 */
CONST PointerType *IRFactory::getPointerTy(int size)
{
    switch (size)
    {
        case 32: return Int32PtrTy;
        case 64: return Int64PtrTy;
        case 16: return Int16PtrTy;
        case 8:  return Int8PtrTy;
        default:
            IRError("%s: invalid bit type %d.\n", __func__, size);
    }

    return NULL;
}

/*
 * SaveGlobals()
 *  Store dirty states back to CPUState in memory.
 */
void IRFactory::SaveGlobals(int coherence_level, Instruction *InsertBefore)
{
    if (coherence_level == COHERENCE_NONE)
        return;

    for (int i = 0; i < nb_globals; i++)
    {
        Register *reg = &Reg[i];
        if (reg->isReg())
        {
#ifdef ASSERT
            if (reg->isRev())
                IRError("%s: internal error.\n", __func__);
#endif

            if (reg->isDirty())
            {
                uint64_t size = TD->getTypeSizeInBits(
                        reg->getData()->getType());

#ifdef ASSERT
                if ((int)size != reg->Size)
                    IRError("%d(%s): fatal error %d %ld\n", i,
                            reg->Name.c_str(), reg->Size, size);
#endif

                Value *Addr = LookupAddr(reg, getPointerTy(size));
                new StoreInst(reg->getData(), Addr, false, InsertBefore);
            }
            reg->Demote();
        }
    }

    if (coherence_level == COHERENCE_GLOBAL)
        return;

    /* Store local pseudo registers to the `frame' space. */
    for (int i = nb_globals; i < nb_temps; i++)
    {
        Register *reg = &Reg[i];
        if (reg->isReg())
        {
            if (reg->isLocal() && reg->isDirty())
            {
                uint64_t size = TD->getTypeSizeInBits(
                        reg->getData()->getType());
                
                int next = size % (8 * sizeof(long));
                if (next)
                    next = (8 * sizeof(long)) - next;
                SetFrame(reg, size + next);
                Value *Addr = LookupAddr(reg, getPointerTy(size));
                new StoreInst(reg->getData(), Addr, false, InsertBefore);
            }
            reg->Demote();
        }
    }
}

/*
 * LookupAddr()
 *  Get or insert the pointer to the CPU register in the AddrCache.
 */
Value *IRFactory::LookupAddr(Register *reg, CONST Type *type)
{
    CallInst *CI;
    GetElementPtrInst *GEP;
    int basereg = reg->Base;
    tcg_target_long off = reg->Off;
    
    CI = BaseReg[basereg].Base;

    if (AddrCache.count(off) == 0)
        AddrCache[off] = GetElementPtrInst::Create(CI,
                ConstantInt::get(Int32Ty, off), "", InitGEPInst);

    GEP = AddrCache[off];

    if (TypeCache[type].count(GEP) == 0)
        TypeCache[type][GEP] = new BitCastInst(GEP, type, reg->Name,
                InitLastInst);

    return TypeCache[type][GEP];
}

Value *IRFactory::LookupAddr(Value *Base, tcg_target_ulong Off,
        CONST Type *type)
{
    GetElementPtrInst *GEP;

    if (Base->stripPointerCasts() != CPU)
        DM->Error("%s: internal error.\n", __func__);

    if (AddrCache.count(Off) == 0)
        AddrCache[Off] = GetElementPtrInst::Create(CPU,
                ConstantInt::get(Int32Ty, Off), "", InitGEPInst);

    GEP = AddrCache[Off];

    if (TypeCache[type].count(GEP) == 0)
        TypeCache[type][GEP] = new BitCastInst(GEP, type, "", InitLastInst);

    return TypeCache[type][GEP];
}

/*
 * LoadInput()
 *  Load the value of CPU register from the CPUState in memory or return from
 *  the LLVM pseudo register if it has been promoted.
 */
Value *IRFactory::LoadInput(Register *reg)
{
    CONST PointerType *PTy = (reg->Size == 32) ? Int32PtrTy : Int64PtrTy;

    if (reg->isRev())
        return LookupAddr(reg, PTy);
    if (reg->isReg())
        return reg->getData();

    /* If we go here, the data is not promoted to the register. Load and promote
     * it from CPUState. */
    LoadInst *v = LOADREG(LookupAddr(reg, PTy));
    reg->setData(v);

    return v;
}

/*
 * InsertSeparator()
 *  Insert the seperator indicating the last opcode of the guest instruction.
 */
void IRFactory::InsertSeparator()
{
    return;
    if (LastInst == NULL)
        return;

    Instruction *I = new BitCastInst(CPU, BaseRegType, "", LastInst);
    Separator.push_back(I);
}

void IRFactory::InsertSeparator(Instruction *InsertPos)
{
    return;
    if (LastInst == NULL)
        return;

    Instruction *I = new BitCastInst(CPU, BaseRegType, "", InsertPos);
    Separator.push_back(I);
}

/*
 * TCG opcode to LLVM IR translation functions.
 */
int IRFactory::op_patch(const TCGArg *args)
{
    DM->Error("%s: fatal error. Unsupported opcode for LLVM.\n", __func__);
    return llvm_op_defs[INDEX_op_patch].nb_args;
}

int IRFactory::op_goto_ibchain(const TCGArg *args)
{
    DM->Error("%s: fatal error. Unsupported opcode for LLVM.\n", __func__);
    return llvm_op_defs[INDEX_op_goto_ibchain].nb_args;
}

int IRFactory::op_cmp_ibchain(const TCGArg *args)
{
    DM->Error("%s: fatal error. Unsupported opcode for LLVM.\n", __func__);
    return llvm_op_defs[INDEX_op_cmp_ibchain].nb_args;
}

/* Predefined ops */
int IRFactory::op_end(const TCGArg *args)
{
    return llvm_op_defs[INDEX_op_end].nb_args;
}

int IRFactory::op_nop(const TCGArg *args)
{
    IRDebug(INDEX_op_nop);
    return llvm_op_defs[INDEX_op_nop].nb_args;
}

int IRFactory::op_nop1(const TCGArg *args)
{
    IRDebug(INDEX_op_nop1);
    return llvm_op_defs[INDEX_op_nop1].nb_args;
}

int IRFactory::op_nop2(const TCGArg *args)
{
    IRDebug(INDEX_op_nop2);
    return llvm_op_defs[INDEX_op_nop2].nb_args;
}

int IRFactory::op_nop3(const TCGArg *args)
{
    IRDebug(INDEX_op_nop3);
    return llvm_op_defs[INDEX_op_nop3].nb_args;
}

int IRFactory::op_nopn(const TCGArg *args)
{
    IRDebug(INDEX_op_nopn);
    return args[0];
}

/*
 * op_discard()
 *  args[0]: In
 */
int IRFactory::op_discard(const TCGArg *args)
{
    IRDebug(INDEX_op_discard);

    Register *In = &Reg[args[0]];

    if (In->isReg())
        In->Demote();

    return llvm_op_defs[INDEX_op_discard].nb_args;
}

/*
 * op_set_label()
 *  args[0]: Label number
 */
int IRFactory::op_set_label(const TCGArg *args)
{
    IRDebug(INDEX_op_set_label);

    SaveGlobals(COHERENCE_ALL, LastInst);

    int label = args[0];

    if (Labels[label] == NULL)
        Labels[label] = BASIC("true_dest");

    CurrBB = Labels[label];
    if (LastInst != NULL)
    {
        if (LastInst != LastInst->getParent()->begin() &&
                dyn_cast<IndirectBrInst>(--BasicBlock::iterator(LastInst)))
            LastInst->eraseFromParent();
        else
            setSuccessor(LastInst, CurrBB);
    }

    LastInst = BranchInst::Create(ExitBB, CurrBB);

    return llvm_op_defs[INDEX_op_set_label].nb_args;
}

/*
 * op_call()
 *  args[0]                       : [nb_oargs:16][nb_iargs:16]
 *  args[1~#nb_oargs]             : out args
 *  args[1+#nb_oargs~#nb_iargs-2] : function parameters
 *  args[1+#nb_oargs+#nb_iargs-1] : function address
 *  args[1+#nb_oargs+#nb_iargs]   : flags
 */
int IRFactory::op_call(const TCGArg *args)
{
    IRDebug(INDEX_op_call);

    SmallVector<Value *, 4> Params;
    TCGArg arg = *args++;
    int nb_oargs = arg >> 16;
    int nb_iargs = arg & 0xffff;
    int nb_params = nb_iargs - 1;
    int flags = args[nb_oargs + nb_iargs];
    int func_arg = args[nb_oargs + nb_iargs - 1];

    Register *In = &Reg[func_arg];
    ConstantInt *C = dyn_cast<ConstantInt>(In->getData());
    if (C == NULL)
        IRError("%s: invalid input type.\n", __func__);

    /* Get function declaration from LLVM module. */
    const char *Fname = Env->getHelperName((uintptr_t)C->getZExtValue());
    if (Fname == NULL)
        IRError("%s: cannot resolve funtion.\n", __func__);

    Function *Fn = Mod->getFunction(Fname);
    if (Fn == NULL)
        IRError("%s: unsupported function %s.\n", __func__, Fname);

    /* Package the function parameters. 
       NOTE: There are situations where the numbers of given arguments 
       are greater than the *real* function parameters. Ex:
           declare void foo(int64, int64);
              and
           call foo(int32, int32, int32, int32);
     */
    int real_nb_params = Fn->getFunctionType()->getNumParams();
    if (nb_params == real_nb_params)
    {
        for (int i = 0; i < real_nb_params; i++)
        {
            CONST Type *ParamTy = Fn->getFunctionType()->getParamType(i);
            In = &Reg[args[nb_oargs + i]];
            Value *InData = LoadInput(In);
            
            uint64_t real_size = TD->getTypeSizeInBits(ParamTy);
            uint64_t size = TD->getTypeSizeInBits(InData->getType());

            if (size == real_size)
            {
                if (InData->getType() != ParamTy)
                    InData = new BitCastInst(InData, ParamTy, "", LastInst);
            }
            else if (real_size < size)
            {
                InData = TRUNC(InData, IntegerType::get(Context, real_size));
            }
            
            Params.push_back(InData);
        }
    }
    else
    {
        int idx = 0;
        for (int i = 0; i < real_nb_params; i++)
        {
            Value *tmp = NULL;
            CONST Type *ParamTy = Fn->getFunctionType()->getParamType(i);
            uint64_t real_size = TD->getTypeSizeInBits(ParamTy);
            uint64_t size, remain = real_size;

next:
            In = &Reg[args[nb_oargs + idx]];
            Value *InData = LoadInput(In);

            size = TD->getTypeSizeInBits(InData->getType());
            if (size == real_size)
            {
                if (InData->getType() != ParamTy)
                    InData = new BitCastInst(InData, ParamTy, "", LastInst);

                Params.push_back(InData);
                idx++;
            }
            else
            {
                if (remain == real_size)
                    tmp = ZEXT(InData, IntegerType::get(Context, real_size));
                else
                {
                    InData = ZEXT(InData, ParamTy);
                    InData = SHL(InData, ConstantInt::get(ParamTy, real_size-remain));

                    tmp = OR(tmp, InData);
                }

                remain -= size;
                idx++;

                if (remain < 0)
                    IRError("%s: fatal error.\n", __func__);
                else if (remain > 0)
                    goto next;
                else
                    Params.push_back(tmp);
            }
        }

        if (idx != nb_params)
            IRError("%s: num params not matched.\n", __func__);
    }

    /* Save global registers if this function is not TCG constant function.
       Otherwise, mark this call instruction for state mapping use. */
    if (!(flags & TCG_CALL_CONST))
        SaveGlobals(COHERENCE_GLOBAL, LastInst);

    CallInst *CI = CreateCall(Fn, Params, "", LastInst);

    if (flags & TCG_CALL_CONST)
    {
        Value *Meta[] = { getUID(), };
        CI->setMetadata("flags", CreateMDNode(Context, Meta, 1));
    }

    /* Determine if this function can be inlined. */
    if (HelperFuncs.count(Fname))
    {
        bool MustInline = false;
        HelperInfo *Helper = HelperFuncs[Fname];
        if (Helper->mayConflict == false && AnalyzeInlineCost(CallSite(CI)) > 0)
        {
            MustInline = true;
            InlineCalls.push_back(CI);
        }
        
        if (MustInline == false)
            CI->setCalledFunction(Helper->FuncNoInline);
    }

    /* Format the return value. 
       NOTE: There are situations where the return value is split and
       is used by different instructions. Ex:
           int64 ret = call foo();
           ... = opcode ret[0..31];
           ... = opcode ret[32..64];
     */
    if (nb_oargs == 1)
    {
        Register *Out = &Reg[args[0]];
        Out->setData(CI, true);
    }
    else if (nb_oargs > 1)
    {
        Value *tmp = CI;
        uint64_t size = TD->getTypeSizeInBits(Fn->getReturnType());
        uint64_t subsize = size / nb_oargs;
        for (int i = 0; i < nb_oargs; i++)
        {
            Register *Out = &Reg[args[i]];
            Value *OutData = TRUNC(tmp, IntegerType::get(Context, subsize));
            Out->setData(OutData, true);
            if (i != nb_oargs - 1)
                tmp = LSHR(tmp, ConstantInt::get(IntegerType::get(Context, size), subsize));
        }
    }

    return nb_iargs + nb_oargs + llvm_op_defs[INDEX_op_call].nb_cargs + 1;
}

/*
 * op_jmp()
 *  args[0]: In
 */
int IRFactory::op_jmp(const TCGArg *args)
{
    IRDebug(INDEX_op_jmp);

    Register *In = &Reg[args[0]];
    Value *InData = LoadInput(In);

    SaveGlobals(COHERENCE_ALL, LastInst);
    
    if (InData->getType()->isPointerTy() == false)
        InData = ITP8(InData);

    IndirectBrInst *IB = IndirectBrInst::Create(InData, 1, LastInst);
    setInsertPos(IB);

    return llvm_op_defs[INDEX_op_jmp].nb_args;
}

/*
 * op_br()
 *  args[0]: Label number
 */
int IRFactory::op_br(const TCGArg *args)
{
    IRDebug(INDEX_op_br);

    SaveGlobals(COHERENCE_ALL, LastInst);

    int label = args[0];
    if (Labels[label] == NULL)
        Labels[label] = BASIC("direct_jump_tb");

    BranchInst::Create(Labels[label], LastInst);
    LastInst->eraseFromParent();
    LastInst = NULL;

    return llvm_op_defs[INDEX_op_br].nb_args;
}

/*
 * op_mov_i32()
 *  args[0]: Out
 *  args[1]: In
 */
int IRFactory::op_mov_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_mov_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32);

    Value *InData = LoadInput(In);

    uint64_t size = TD->getTypeSizeInBits(InData->getType());
    if (size != 32)
        InData = TRUNC32(InData);

    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_mov_i32].nb_args;
}

/*
 * op_movi_i32()
 *  args[0]: Out
 *  args[1]: In  (const value)
 */
int IRFactory::op_movi_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_movi_i32);

    Register *Out = &Reg[args[0]];

    AssertType(Out->Size == 32);

    Value *InData = CONST32(args[1]);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_movi_i32].nb_args;
}

/*
 * op_setcond_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 *  args[3]: In3 (condition code)
 */
int IRFactory::op_setcond_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_setcond_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[3])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
                          /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = ICMP(InData1, InData2, pred);
    OutData = ZEXT32(OutData);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_setcond_i32].nb_args;
}

/* load/store */
int IRFactory::op_ld8u_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ld8u_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];
    
    Value *InData = LoadInput(In);
    
    AssertType(Out->Size == 32);
    AssertPointer(InData);
    
    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);
    
    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR8(GEP);
    
    InData = new LoadInst(InData, "", false, LastInst);
    InData = ZEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld8u_i32].nb_args;
}

int IRFactory::op_ld8s_i32(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_ld8s_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];
    
    Value *InData = LoadInput(In);
    
    AssertType(Out->Size == 32);
    AssertPointer(InData);
    
    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);
    
    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR8(GEP);
    
    InData = new LoadInst(InData, "", false, LastInst);
    InData = SEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld8s_i32].nb_args;
}

int IRFactory::op_ld16u_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ld16u_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];
    
    Value *InData = LoadInput(In);
    
    AssertType(Out->Size == 32);
    AssertPointer(InData);
    
    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);
    
    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR16(GEP);
    
    InData = new LoadInst(InData, "", false, LastInst);
    InData = ZEXT32(InData);
    Out->setData(InData, true);
    
    return llvm_op_defs[INDEX_op_ld16u_i32].nb_args;
}

int IRFactory::op_ld16s_i32(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_ld16s_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];
    
    Value *InData = LoadInput(In);
    
    AssertType(Out->Size == 32);
    AssertPointer(InData);
    
    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);
    
    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR16(GEP);
    
    InData = new LoadInst(InData, "", false, LastInst);
    InData = SEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld16s_i32].nb_args;
}

/*
 * op_ld_i32()
 *  args[0]: Out
 *  args[1]: In  (base)
 *  args[2]: In  (offset)
 */
int IRFactory::op_ld_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ld_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData = LoadInput(In);

    AssertType(Out->Size == 32);
    AssertPointer(InData);

    CONST Type *BaseTy = InData->getType();
    if (In->isRev())
    {
        InData = LookupAddr(InData, off, Int32PtrTy);
    }
    else if (off % 4 == 0)
    {
        if (BaseTy->isPointerTy() == false)
            InData = ITP32(InData);
        else if (BaseTy != Int32PtrTy)
            InData = CASTPTR32(InData);

        InData = GetElementPtrInst::Create(InData, CONST32(off / 4), "", LastInst);
    }
    else
    {
        if (BaseTy->isPointerTy() == false)
            InData = ITP8(InData);
        else if (BaseTy != Int8PtrTy)
            InData = CASTPTR8(InData);
        
        InData = GetElementPtrInst::Create(InData, CONST32(off), "", LastInst);
        InData = CASTPTR32(InData);
    }

    InData = new LoadInst(InData, "", false, LastInst);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld_i32].nb_args;
}

int IRFactory::op_st8_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_st8_i32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(TD->getTypeSizeInBits(InData1->getType()) == 32);
    AssertPointer(InData2);

    InData1 = TRUNC8(InData1);

    if (InData2->getType() != Int8PtrTy)
        InData2 = CASTPTR8(InData2);

    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData2,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData2 = CASTPTR8(GEP);

    new StoreInst(InData1, InData2, false, LastInst);

    return llvm_op_defs[INDEX_op_st8_i32].nb_args;
}

int IRFactory::op_st16_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_st16_i32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(TD->getTypeSizeInBits(InData1->getType()) == 32);
    AssertPointer(InData2);

    InData1 = TRUNC16(InData1);

    if (InData2->getType() != Int8PtrTy)
        InData2 = CASTPTR8(InData2);

    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData2,
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData2 = CASTPTR16(GEP);

    new StoreInst(InData1, InData2, false, LastInst);

    return llvm_op_defs[INDEX_op_st16_i32].nb_args;
}

/*
 * op_st_i32()
 *  args[0]: In1
 *  args[1]: In2 (base)
 *  args[2]: In3 (offset)
 */
int IRFactory::op_st_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_st_i32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(TD->getTypeSizeInBits(InData1->getType()) == 32);
    AssertPointer(InData2);

    CONST Type *BaseTy = InData2->getType();
    if (In2->isRev())
    {
        InData2 = LookupAddr(InData2, off, Int32PtrTy);
    }
    else if (off % 4 == 0)
    {
        if (BaseTy->isPointerTy() == false)
            InData2 = ITP32(InData2);
        else if (BaseTy != Int32PtrTy)
            InData2 = CASTPTR32(InData2);
        
        InData2 = GetElementPtrInst::Create(InData2, CONST32(off / 4), "", LastInst);
    }
    else
    {
        if (BaseTy->isPointerTy() == false)
            InData2 = ITP8(InData2);
        else if (BaseTy != Int8PtrTy)
            InData2 = CASTPTR8(InData2);
        
        InData2 = GetElementPtrInst::Create(InData2, CONST32(off), "", LastInst);
        InData2 = CASTPTR32(InData2);
    }

    new StoreInst(InData1, InData2, false, LastInst);

    return llvm_op_defs[INDEX_op_st_i32].nb_args;
}

/* arith */
/*
 * op_add_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_add_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_add_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData;
    if (In1->isRev())
    {
        tcg_target_long off = static_cast<ConstantInt*>(InData2)->getZExtValue();
        OutData = LookupAddr(InData1, off, Int32PtrTy);
    }
    else
        OutData = ADD(InData1, InData2);

    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_add_i32].nb_args;
}

/*
 * op_sub_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_sub_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_sub_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SUB(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_sub_i32].nb_args;
}

int IRFactory::op_mul_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_mul_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
        
    Value *OutData = MUL(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_mul_i32].nb_args;
}
#ifdef TCG_TARGET_HAS_div_i32
int IRFactory::op_div_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_div_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
        
    Value *OutData = SDIV(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_div_i32].nb_args;
}

int IRFactory::op_divu_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_divu_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = UDIV(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_divu_i32].nb_args;
}

int IRFactory::op_rem_i32(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_rem_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SREM(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_rem_i32].nb_args;
}

int IRFactory::op_remu_i32(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_remu_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = UREM(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_remu_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_div2_i32
int IRFactory::op_div2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_div2_i32);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In1 = &Reg[args[2]];
#if 0
    Register *In2 = &Reg[args[3]];
#endif
    Register *In3 = &Reg[args[4]];

    AssertType(Out1->Size == 32 && Out2->Size == 32 && 
            In1->Size == 32 && In3->Size == 32);

    Value *InData1 = LoadInput(In1);
#if 0
    Value *InData2 = LoadInput(In2);
#endif
    Value *InData3 = LoadInput(In3);

        
    Value *OutData1 = SDIV(InData1, InData3);
    Value *OutData2 = SREM(InData1, InData3);
    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_div2_i32].nb_args;
}

/*
 * op_divu2_i32()
 *  args[0]: Out1
 *  args[1]: Out2
 *  args[2]: In1
 *  args[3]: In2
 *  args[4]: In3
 */
int IRFactory::op_divu2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_divu2_i32);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In1 = &Reg[args[2]];
#if 0
    Register *In2 = &Reg[args[3]];
#endif
    Register *In3 = &Reg[args[4]];

    AssertType(Out1->Size == 32 && Out2->Size == 32 && 
            In1->Size == 32 && In3->Size == 32);

    Value *InData1 = LoadInput(In1);
#if 0
    Value *InData2 = LoadInput(In2);
#endif
    Value *InData3 = LoadInput(In3);
        
    Value *OutData1 = UDIV(InData1, InData3);
    Value *OutData2 = UREM(InData1, InData3);
    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_divu2_i32].nb_args;
}
#endif
/*
 * op_and_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_and_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_and_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = AND(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_and_i32].nb_args;
}

/*
 * op_or_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_or_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_or_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = OR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_or_i32].nb_args;
}

/*
 * op_xor_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_xor_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_xor_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = XOR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_xor_i32].nb_args;
}

/* shifts/rotates */
/*
 * op_shl_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_shl_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_shl_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SHL(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_shl_i32].nb_args;
}

/*
 * op_shr_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_shr_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_shr_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = LSHR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_shr_i32].nb_args;
}

/*
 * op_sar_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_sar_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_sar_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = ASHR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_sar_i32].nb_args;
}
#ifdef TCG_TARGET_HAS_rot_i32
/*
 * op_rotl_i32()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_rotl_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_rotl_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *c = LSHR(InData1, SUB(CONST32(32), InData2));
    Value *OutData = SHL(InData1, InData2);
    OutData = OR(OutData, c);

    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_rotl_i32].nb_args;
}

int IRFactory::op_rotr_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_rotr_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *c = SHL(InData1, SUB(CONST32(32), InData2));
    Value *OutData = LSHR(InData1, InData2);
    OutData = OR(OutData, c);

    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_rotr_i32].nb_args;
}
#endif
/*
 * op_brcond_i32()
 *  args[0]: In1
 *  args[1]: In2
 *  args[2]: In3 (condition code)
 *  args[3]: In4 (label number)
 */
int IRFactory::op_brcond_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_brcond_i32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    AssertType(In1->Size == 32 && In2->Size == 32);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[2])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
        /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    SaveGlobals(COHERENCE_ALL, LastInst);

    Value *Cond = ICMP(InData1, InData2, pred);

    int label = args[3];
    if (Labels[label] == NULL)
        Labels[label] = BASIC("true_dest");

    BasicBlock *ifTrue = Labels[label];
    BasicBlock *ifFalse = BASIC("false_dest");

    BranchInst::Create(ifTrue, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();
    CurrBB = ifFalse;
    LastInst = BranchInst::Create(ExitBB, CurrBB);

    return llvm_op_defs[INDEX_op_brcond_i32].nb_args;
}
#if TCG_TARGET_REG_BITS == 32
int IRFactory::op_add2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_add2_i32);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In1 = &Reg[args[2]];
    Register *In2 = &Reg[args[3]];
    Register *In3 = &Reg[args[4]];
    Register *In4 = &Reg[args[5]];

    AssertType(Out1->Size == 32 && Out2->Size == 32 && 
            In1->Size == 32 && In2->Size == 32 && 
            In3->Size == 32 && In4->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);
    Value *InData4 = LoadInput(In4);
        
    InData1 = ZEXT64(InData1);
    InData2 = SHL(ZEXT64(InData2), CONST64(32));
    InData2 = OR(InData2, InData1);

    InData3 = ZEXT64(InData3);
    InData4 = SHL(ZEXT64(InData4), CONST64(32));
    InData4 = OR(InData4, InData3);

    InData2 = ADD(InData2, InData4);

    Value *OutData1 = TRUNC32(InData2);
    Value *OutData2 = TRUNC32(LSHR(InData2, CONST64(32)));

    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_add2_i32].nb_args;
}

int IRFactory::op_sub2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_sub2_i32);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In1 = &Reg[args[2]];
    Register *In2 = &Reg[args[3]];
    Register *In3 = &Reg[args[4]];
    Register *In4 = &Reg[args[5]];

    AssertType(Out1->Size == 32 && Out2->Size == 32 && 
            In1->Size == 32 && In2->Size == 32 && 
            In3->Size == 32 && In4->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);
    Value *InData4 = LoadInput(In4);
        
    InData1 = ZEXT64(InData1);
    InData2 = SHL(ZEXT64(InData2), CONST64(32));
    InData2 = OR(InData2, InData1);

    InData3 = ZEXT64(InData3);
    InData4 = SHL(ZEXT64(InData4), CONST64(32));
    InData4 = OR(InData4, InData3);

    InData2 = SUB(InData2, InData4);

    Value *OutData1 = TRUNC32(InData2);
    Value *OutData2 = TRUNC32(LSHR(InData2, CONST64(32)));

    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_sub2_i32].nb_args;
}

int IRFactory::op_brcond2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_brcond2_i32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    Register *In3 = &Reg[args[2]];
    Register *In4 = &Reg[args[3]];

    AssertType(In1->Size == 32 && In2->Size == 32 && In3->Size == 32 && In4->Size == 32);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[4])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
        /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);
    Value *InData4 = LoadInput(In4);

    SaveGlobals(COHERENCE_ALL, LastInst);

    InData1 = ZEXT64(InData1);
    InData2 = SHL(ZEXT64(InData2), CONST64(32));
    InData3 = ZEXT64(InData3);
    InData4 = SHL(ZEXT64(InData4), CONST64(32));

    InData2 = OR(InData2, InData1);
    InData4 = OR(InData4, InData3);

    Value *Cond = ICMP(InData2, InData4, pred);

    int label = args[5];
    if (Labels[label] == NULL)
        Labels[label] = BASIC("true_dest");

    BasicBlock *ifTrue = Labels[label];
    BasicBlock *ifFalse = BASIC("false_dest");

    BranchInst::Create(ifTrue, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();
    CurrBB = ifFalse;
    LastInst = BranchInst::Create(ExitBB, CurrBB);

    return llvm_op_defs[INDEX_op_brcond2_i32].nb_args;
}

int IRFactory::op_mulu2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_mulu2_i32);
    
    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In1 = &Reg[args[2]];
    Register *In2 = &Reg[args[3]];
    
    AssertType(Out1->Size == 32 && Out2->Size == 32 &&
            In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    
    InData1 = ZEXT64(InData1);
    InData2 = ZEXT64(InData2);
    
    Value *OutData = MUL(InData1, InData2);
    Value *Low = TRUNC32(OutData);
    Value *High = TRUNC32(LSHR(OutData, CONST64(32)));
    Out1->setData(Low, true);
    Out2->setData(High, true);
    
    return llvm_op_defs[INDEX_op_mulu2_i32].nb_args;
}

int IRFactory::op_setcond2_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_setcond2_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];
    Register *In3 = &Reg[args[3]];
    Register *In4 = &Reg[args[4]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32 &&
            In3->Size == 32 && In4->Size == 32);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[5])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
                          /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);
    Value *InData4 = LoadInput(In4);

    InData1 = ZEXT64(InData1);
    InData2 = SHL(ZEXT64(InData2), CONST64(32));
    InData3 = ZEXT64(InData3);
    InData4 = SHL(ZEXT64(InData4), CONST64(32));

    InData2 = OR(InData2, InData1);
    InData4 = OR(InData4, InData3);

    Value *OutData = ICMP(InData2, InData4, pred);
    OutData = ZEXT32(OutData);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_setcond2_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext8s_i32
int IRFactory::op_ext8s_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ext8s_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = TRUNC8(InData);
    InData = SEXT32(InData);

    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext8s_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext16s_i32
int IRFactory::op_ext16s_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ext16s_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = TRUNC16(InData);
    InData = SEXT32(InData);

    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext16s_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext8u_i32
/*FIXME: we assume the input value is 32bit */
int IRFactory::op_ext8u_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ext8u_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = AND(InData, CONST32(0xFF));
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext8u_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext16u_i32
int IRFactory::op_ext16u_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_ext16u_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = AND(InData, CONST32(0xFFFF));
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext16u_i32].nb_args;
}

#endif
#ifdef TCG_TARGET_HAS_bswap16_i32
int IRFactory::op_bswap16_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_bswap16_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = TRUNC16(InData);
    InData = BSWAP16(InData);
    InData = ZEXT32(InData);
    Out->setData(InData);

    return llvm_op_defs[INDEX_op_bswap16_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_bswap32_i32
int IRFactory::op_bswap32_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_bswap32_i32);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    InData = BSWAP32(InData);
    Out->setData(InData);

    return llvm_op_defs[INDEX_op_bswap32_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_not_i32
/*
 * op_not_i32()
 *  args[0]: Out
 *  args[1]: In
 */
int IRFactory::op_not_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_not_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    Value *OutData = XOR(InData, CONST32(-1));
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_not_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_neg_i32
/*
 * op_neg_i32()
 *  args[0]: Out
 *  args[1]: In
 */
int IRFactory::op_neg_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_neg_i32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 32 && In->Size == 32);

    Value *InData = LoadInput(In);

    Value *OutData = SUB(CONST32(0), InData);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_neg_i32].nb_args;
}

#endif
#ifdef TCG_TARGET_HAS_andc_i32
int IRFactory::op_andc_i32(const TCGArg *args)
{
    IRDebug(INDEX_op_andc_i32);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 32 && In1->Size == 32 && In2->Size == 32);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    
    InData2 = XOR(InData2, CONST32(-1ULL));
    Value *OutData = AND(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_andc_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_orc_i32
int IRFactory::op_orc_i32(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_orc_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_eqv_i32
int IRFactory::op_eqv_i32(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_eqv_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_nand_i32
int IRFactory::op_nand_i32(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_nand_i32].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_nor_i32
int IRFactory::op_nor_i32(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_nor_i32].nb_args;
}
#endif
#if TCG_TARGET_REG_BITS == 64
int IRFactory::op_mov_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_mov_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64);

    Value *InData = LoadInput(In);

    uint64_t size = TD->getTypeSizeInBits(InData->getType());
    if (size != 64)
        InData = ZEXT64(InData);

    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_mov_i64].nb_args;
}

/*
 * op_movi_i64()
 *  args[0]: Out
 *  args[1]: In  (const value)
 */
int IRFactory::op_movi_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_movi_i64);

    Register *Out = &Reg[args[0]];

    AssertType(Out->Size == 64);

    Value *InData = CONST64(args[1]);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_movi_i64].nb_args;
}

int IRFactory::op_setcond_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_setcond_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[3])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
                          /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = ICMP(InData1, InData2, pred);
    OutData = ZEXT64(OutData);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_setcond_i64].nb_args;
}

/* load/store */
int IRFactory::op_ld8u_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_ld8u_i64].nb_args;
}

int IRFactory::op_ld8s_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_ld8s_i64].nb_args;
}

int IRFactory::op_ld16u_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_ld16u_i64].nb_args;
}

int IRFactory::op_ld16s_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_ld16s_i64].nb_args;
}

int IRFactory::op_ld32u_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ld32u_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData = LoadInput(In);

    AssertType(Out->Size == 64);
    AssertPointer(InData);

    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);

    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData, 
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR32(GEP);
    InData = new LoadInst(InData, "", false, LastInst);
    InData = ZEXT64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld32u_i64].nb_args;
}

int IRFactory::op_ld32s_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ld32s_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData = LoadInput(In);

    AssertType(Out->Size == 64);
    AssertPointer(InData);

    if (InData->getType() != Int8PtrTy)
        InData = CASTPTR8(InData);

    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData, 
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData = CASTPTR32(GEP);
    InData = new LoadInst(InData, "", false, LastInst);
    InData = SEXT64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld32s_i64].nb_args;
}

/*
 * op_ld_i64()
 *  args[0]: Out
 *  args[1]: In  (base)
 *  args[2]: In  (offset)
 */
int IRFactory::op_ld_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ld_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData = LoadInput(In);

    AssertType(Out->Size == 64);
    AssertPointer(InData);

    if (In->isRev())
    {
        InData = LookupAddr(InData, off, Int64PtrTy);
    }
    else if (off % 8 == 0)
    {
        if (InData->getType() != Int64PtrTy)
            InData = CASTPTR64(InData);
        
        InData = GetElementPtrInst::Create(InData, CONST64(off / 8), "", LastInst);
    }
    else
    {
        if (InData->getType() != Int8PtrTy)
            InData = CASTPTR8(InData);
        
        InData = GetElementPtrInst::Create(InData, CONST64(off), "", LastInst);
        InData = CASTPTR64(InData);
    }

    InData = new LoadInst(InData, "", false, LastInst);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ld_i64].nb_args;
}

int IRFactory::op_st8_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_st8_i64].nb_args;
}

int IRFactory::op_st16_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_st16_i64].nb_args;
}

int IRFactory::op_st32_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_st32_i64);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(TD->getTypeSizeInBits(InData1->getType()) == 64);
    AssertPointer(InData2);

    InData1 = TRUNC32(InData1);

    if (InData2->getType() != Int8PtrTy)
        InData2 = CASTPTR8(InData2);

    GetElementPtrInst *GEP = GetElementPtrInst::Create(InData2, 
            ConstantInt::get(Int32Ty, off), "", LastInst);
    InData2 = CASTPTR32(GEP);

    new StoreInst(InData1, InData2, false, LastInst);

    return llvm_op_defs[INDEX_op_st32_i64].nb_args;
}

/*
 * op_st_i64()
 *  args[0]: In1
 *  args[1]: In2 (base)
 *  args[2]: In3 (offset)
 */
int IRFactory::op_st_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_st_i64);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    tcg_target_long off = args[2];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(TD->getTypeSizeInBits(InData1->getType()) == 64);
    AssertPointer(InData2);

    if (In2->isRev())
    {
        InData2 = LookupAddr(InData2, off, Int64PtrTy);
    }
    else if (off % 8 == 0)
    {
        if (InData2->getType() != Int64PtrTy)
            InData2 = CASTPTR64(InData2);
        
        InData2 = GetElementPtrInst::Create(InData2, CONST64(off / 8), "", LastInst);
    }
    else
    {
        if (InData2->getType() != Int8PtrTy)
            InData2 = CASTPTR8(InData2);
        
        InData2 = GetElementPtrInst::Create(InData2, CONST64(off), "", LastInst);
        InData2 = CASTPTR64(InData2);
    }

    new StoreInst(InData1, InData2, false, LastInst);

    return llvm_op_defs[INDEX_op_st_i64].nb_args;
}

/* arith */
/*
 * op_add_i64()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_add_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_add_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData;
    if (In1->isRev())
    {
        tcg_target_long off = static_cast<ConstantInt*>(InData2)->getZExtValue();
        OutData = LookupAddr(InData1, off, Int64PtrTy);
    }
    else
        OutData = ADD(InData1, InData2);

    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_add_i64].nb_args;
}

int IRFactory::op_sub_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_sub_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SUB(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_sub_i64].nb_args;
}

/*
 * op_mul_i64()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_mul_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_mul_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = MUL(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_mul_i64].nb_args;
}
#ifdef TCG_TARGET_HAS_div_i64
int IRFactory::op_div_i64(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_div_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SDIV(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_div_i64].nb_args;
}

int IRFactory::op_divu_i64(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_divu_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = UDIV(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_divu_i64].nb_args;
}

int IRFactory::op_rem_i64(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_rem_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = SREM(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_rem_i64].nb_args;
}

int IRFactory::op_remu_i64(const TCGArg *args)
{
    IRError("%s: test me and remove this line if it is correct.\n", __func__);
    IRDebug(INDEX_op_remu_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = UREM(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_remu_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_div2_i64
int IRFactory::op_div2_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_div2_i64].nb_args;
}

int IRFactory::op_divu2_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_divu2_i64].nb_args;
}
#endif
int IRFactory::op_and_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_and_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

#if defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    uint64_t size = TD->getTypeSizeInBits(InData1->getType());
    if (size != 64)
        InData1 = ZEXT64(InData1);
    size = TD->getTypeSizeInBits(InData2->getType());
    if (size != 64)
        InData2 = ZEXT64(InData2);
#else
    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);
#endif

    Value *OutData = AND(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_and_i64].nb_args;
}

int IRFactory::op_or_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_or_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = OR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_or_i64].nb_args;
}

int IRFactory::op_xor_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_xor_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = XOR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_xor_i64].nb_args;
}

/* shifts/rotates */
int IRFactory::op_shl_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_shl_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
        
    Value *OutData = SHL(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_shl_i64].nb_args;
}

/*
 * op_shr_i64()
 *  args[0]: Out
 *  args[1]: In1
 *  args[2]: In2
 */
int IRFactory::op_shr_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_shr_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *OutData = LSHR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_shr_i64].nb_args;
}

int IRFactory::op_sar_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_sar_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *OutData = ASHR(InData1, InData2);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_sar_i64].nb_args;
}
#ifdef TCG_TARGET_HAS_rot_i64
int IRFactory::op_rotl_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_rotl_i64);

    Register *Out = &Reg[args[0]];
    Register *In1 = &Reg[args[1]];
    Register *In2 = &Reg[args[2]];

    AssertType(Out->Size == 64 && In1->Size == 64 && In2->Size == 64);

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    Value *c = LSHR(InData1, SUB(CONST64(64), InData2));
    Value *OutData = SHL(InData1, InData2);
    OutData = OR(OutData, c);

    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_rotl_i64].nb_args;
}

int IRFactory::op_rotr_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_rotr_i64].nb_args;
}
#endif
int IRFactory::op_brcond_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_brcond_i64);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    AssertType(In1->Size == 64 && In2->Size == 64);

    CmpInst::Predicate pred = ICmpInst::BAD_ICMP_PREDICATE;
    switch (args[2])
    {
        case TCG_COND_EQ: pred = ICmpInst::ICMP_EQ; break;
        case TCG_COND_NE: pred = ICmpInst::ICMP_NE; break;
        case TCG_COND_LT: pred = ICmpInst::ICMP_SLT; break;
        case TCG_COND_GE: pred = ICmpInst::ICMP_SGE; break;
        case TCG_COND_LE: pred = ICmpInst::ICMP_SLE; break;
        case TCG_COND_GT: pred = ICmpInst::ICMP_SGT; break;
        /* unsigned */
        case TCG_COND_LTU: pred = ICmpInst::ICMP_ULT; break;
        case TCG_COND_GEU: pred = ICmpInst::ICMP_UGE; break;
        case TCG_COND_LEU: pred = ICmpInst::ICMP_ULE; break;
        case TCG_COND_GTU: pred = ICmpInst::ICMP_UGT; break;
        default:
            IRError("%s - unsupported predicate\n", __func__);
    }

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    SaveGlobals(COHERENCE_ALL, LastInst);

    Value *Cond = ICMP(InData1, InData2, pred);

    int label = args[3];
    if (Labels[label] == NULL)
        Labels[label] = BASIC("true_dest");

    BasicBlock *ifTrue = Labels[label];
    BasicBlock *ifFalse = BASIC("false_dest");

    BranchInst::Create(ifTrue, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();
    CurrBB = ifFalse;
    LastInst = BranchInst::Create(ExitBB, CurrBB);

    return llvm_op_defs[INDEX_op_brcond_i64].nb_args;
}
#ifdef TCG_TARGET_HAS_ext8s_i64
int IRFactory::op_ext8s_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext8s_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64);

    Value *InData = LoadInput(In);

    InData = TRUNC8(InData);
    InData = SEXT64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext8s_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext16s_i64
int IRFactory::op_ext16s_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext16s_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64);

    Value *InData = LoadInput(In);

    InData = TRUNC16(InData);
    InData = SEXT64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext16s_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext32s_i64
/*
 * op_ext32s_i64()
 *  args[0]: Out
 *  args[1]: In
 */
int IRFactory::op_ext32s_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext32s_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64);

    Value *InData = LoadInput(In);

    uint64_t size = TD->getTypeSizeInBits(InData->getType());
    if (size != 32)
        InData = TRUNC32(InData);

    InData = SEXT64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext32s_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext8u_i64
int IRFactory::op_ext8u_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext8u_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64 && In->Size == 64);

    Value *InData = LoadInput(In);

    InData = AND(InData, CONST64(0xFF));
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext8u_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext16u_i64
int IRFactory::op_ext16u_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext16u_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64 && In->Size == 64);

    Value *InData = LoadInput(In);

    InData = AND(InData, CONST64(0xFFFF));
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext16u_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_ext32u_i64
/*
 * op_ext32u_i64()
 *  args[0]: Out
 *  args[1]: In
 */
int IRFactory::op_ext32u_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_ext32u_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64);

    Value *InData = LoadInput(In);

    uint64_t size = TD->getTypeSizeInBits(InData->getType());
    if (size == 32)
        InData = ZEXT64(InData);
    else
        InData = AND(InData, CONST64(0xFFFFFFFF));
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_ext32u_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_bswap16_i64
int IRFactory::op_bswap16_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_bswap16_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_bswap32_i64
int IRFactory::op_bswap32_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_bswap32_i64);
    
    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64 && In->Size == 64);

    Value *InData = LoadInput(In);

    InData = TRUNC32(InData);
    InData = BSWAP32(InData);
    InData = ZEXT64(InData);
    Out->setData(InData);

    return llvm_op_defs[INDEX_op_bswap32_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_bswap64_i64
int IRFactory::op_bswap64_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_bswap64_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_not_i64
int IRFactory::op_not_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_not_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64 && In->Size == 64);

    Value *InData = LoadInput(In);

    Value *OutData = XOR(InData, CONST64(-1));
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_not_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_neg_i64
int IRFactory::op_neg_i64(const TCGArg *args)
{
    IRDebug(INDEX_op_neg_i64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    AssertType(Out->Size == 64 && In->Size == 64);

    Value *InData = LoadInput(In);

    Value *OutData = SUB(CONST64(0), InData);
    Out->setData(OutData, true);

    return llvm_op_defs[INDEX_op_neg_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_andc_i64
int IRFactory::op_andc_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_andc_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_orc_i64
int IRFactory::op_orc_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_orc_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_eqv_i64
int IRFactory::op_eqv_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_eqv_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_nand_i64
int IRFactory::op_nand_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_nand_i64].nb_args;
}
#endif
#ifdef TCG_TARGET_HAS_nor_i64
int IRFactory::op_nor_i64(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_nor_i64].nb_args;
}
#endif
#endif

/* QEMU specific */
#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
int IRFactory::op_debug_insn_start(const TCGArg *args)
{
    IRDebug(INDEX_op_debug_insn_start);
    return llvm_op_defs[INDEX_op_debug_insn_start].nb_args;
}
#else
int IRFactory::op_debug_insn_start(const TCGArg *args)
{
    IRDebug(INDEX_op_debug_insn_start);
    return llvm_op_defs[INDEX_op_debug_insn_start].nb_args;
}
#endif

inline void IRFactory::TraceRenameBB(Instruction *I, string Append)
{
    BasicBlock *BB = I->getParent();
    BB->setName(BB->getName() + "." + Append);
}

inline void IRFactory::InsertPatchIBChain(MDNode *MD, Instruction *InsertBefore)
{
#if defined(TCG_TARGET_I386)
    static uint8_t IBChainIdx = 0;
    Value *Addr = ConstantExpr::getIntToPtr(CONST8(IBChainIdx++),
            PointerType::getUnqual(Int8Ty));
    new LoadInst(Addr, "", true, InsertBefore);
    ReturnInst *RI = ReturnInst::Create(Context, NULL, InsertBefore);
    RI->setDebugLoc(DebugLoc::get(0, 0, MD));
#elif defined(TCG_TARGET_ARM)
    CallInst *CI = dyn_cast<CallInst>(--BasicBlock::iterator(InsertBefore));
    CI->setTailCall();
    CI->setDebugLoc(DebugLoc::get(0, 0, MD));
    new UnreachableInst(Context, InsertBefore);
#else
    DM->Error("%s: IB chain not supported in this host architecture.\n", __func__);
#endif
}

inline void IRFactory::InsertPatchJmp(MDNode *MD, Instruction *InsertBefore)
{
#if defined(TCG_TARGET_I386)
    /* We need 8 bytes to emit the jump instruction in order to make the
     * address of the jump instruction to align at 4-byte boundary. Try
     * finding one or several contiguous instruction(s) to form the at least
     * 8-byte space. */

    /* Here we use the llvm.trap intrinsic to reserve the 8-byte space.
     * Originally, llvm.trap will emit a 2-byte `ud2' instruction. We change
     * the llvm backend to emit `four' ud2 instructions for the total 8-byte
     * space. */
    Function *TrapFn = Intrinsic::getDeclaration(Mod, Intrinsic::trap);
    Instruction *I = CallInst::Create(TrapFn, "", InsertBefore);
    I->setDebugLoc(DebugLoc::get(0, 0, MD));
    setInsertPos(I);
#elif defined(TCG_TARGET_ARM)
    if (Mode == LLVM_GEN_BLOCK)
    {
        ReturnInst *RI = ReturnInst::Create(Context, NULL, InsertBefore);
        RI->setDebugLoc(DebugLoc::get(0, 0, MD));
        setInsertPos(RI);
    }
    else
    {
        vector<tcg_target_ulong> &CallTable = Env->getCallTable();
        if (CallIdx == CallTable.size() - 1)
            DM->Error("%s: exceeding max address entry %d\n", __func__,
                    CallTable.size());
        
        char Name[32];
        sprintf(Name, "%x", CallTable[CallIdx++]);
        Function *Fn = Mod->getFunction(Name);
        CallInst *CI = CreateCall(Fn, "", InsertBefore);
        CI->setDebugLoc(DebugLoc::get(0, 0, MD));
        setInsertPos(CI);
    }
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    static uint8_t JmpIdx = 0;
    Value *Addr = ConstantExpr::getIntToPtr(CONST32(JmpIdx++), 
            PointerType::getUnqual(Int32Ty));
    LoadInst *LI = new LoadInst(Addr, "", true, InsertBefore);
    new LoadInst(Addr, "", true, InsertBefore);
    new LoadInst(Addr, "", true, InsertBefore);
    new LoadInst(Addr, "", true, InsertBefore);
    LI->setDebugLoc(DebugLoc::get(0, 0, MD));
    setInsertPos(LI);
#endif
}

inline void IRFactory::InsertPatchReturn(MDNode *MD, 
        unsigned long RetVal, Instruction *InsertBefore)
{
#if defined(TCG_TARGET_I386)
    if (OptLevel == CodeGenOpt::None)
    {
        /* We need the patch point at the first return, but LLVM return the
         * address of setting return returned value instead of that of the
         * return instruction if the debug meta is associated to the first
         * return instruction. Thus, we set the debug meta to the second return
         * instruction, and then subtract one to get the address of the first
         * return. The layout is as follows:
         *
         *    LLVM IR                    Host code
         *                                   mov V, %eax
         *    Return V               =>      ret 
         *    Return  <-- debugloc           ret  <-- Addr
         *    Return                         ret
         *    Return                         ret
         *    Return                         ret
         */
        ConstantInt *V = ConstantInt::get(Int32Ty, RetVal);
        ReturnInst *RI = ReturnInst::Create(Context, V, InsertBefore);
        RI = ReturnInst::Create(Context, NULL, InsertBefore);
        RI->setDebugLoc(DebugLoc::get(0, 0, MD));
        RI = ReturnInst::Create(Context, NULL, InsertBefore);
        RI = ReturnInst::Create(Context, NULL, InsertBefore);
        RI = ReturnInst::Create(Context, NULL, InsertBefore);
    }
    else
    {
        CONST IntegerType *Ty = (TCG_TARGET_REG_BITS == 32) ? Int32Ty : Int16Ty;
        ConstantInt *V = ConstantInt::get(Ty, RetVal);
        Value *Addr = ConstantExpr::getIntToPtr(V, PointerType::getUnqual(Ty));
        StoreInst *SI = new StoreInst(V, Addr, false, InsertBefore);
        SI->setDebugLoc(DebugLoc::get(0, 0, MD));
    }
    new UnreachableInst(Context, InsertBefore);
#elif defined(TCG_TARGET_ARM)
    ReturnInst *RI = ReturnInst::Create(Context, CONST32(RetVal), InsertBefore);
    RI->setDebugLoc(DebugLoc::get(0, 0, MD));
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    ReturnInst *RI = ReturnInst::Create(Context, CONST64(RetVal), InsertBefore);
    RI->setDebugLoc(DebugLoc::get(0, 0, MD));
#endif
}

static void getChainSlot(TCGArg V, tcg_target_ulong *Key, tcg_target_ulong *RetVal)
{
    tcg_target_ulong key, retval;
    ChainVec &ChainPoint = Env->getChainPoint();

    llvm_spin_lock(&llvm_interface_lock);
    key = ChainPoint.size();
    retval = (key << 2) | 3;
    ChainPoint.push_back(V);
    llvm_spin_unlock(&llvm_interface_lock);

    *Key = key;
    *RetVal = retval;
}

#if defined(TARGET_SPARC)
inline void IRFactory::InsertLookupIBTC(Value *GuestEIP, Value *GuestEIP2,
        Instruction *InsertBefore)
{
    SmallVector<Value *, 4> Params;
    Value *Meta1[] = { getUID(), };

    Function *Fn = Mod->getFunction("helper_lookup_ibtc2");
    
    Params.push_back(GuestEIP);
    Params.push_back(GuestEIP2);
    CallInst *CI = CreateCall(Fn, Params, "", InsertBefore);
    CI->setMetadata("flags", CreateMDNode(Context, Meta1, 1));

    Value *Meta2[] = { CONST32(0), CONST32(TBuilder->getCurrDepth()), };
    IndirectBrInst *IB = IndirectBrInst::Create(CI, 1, InsertBefore);
    IB->setMetadata("exit", CreateMDNode(Context, Meta2, 2));
    setInsertPos(CI);
}

void IRFactory::TraceLinkIndirectJump(GraphNode *Node, Value *GuessPC, Value *GuessNPC)
{
    DM->Debug(DEBUG_LLVM, "Find indirect branch. Guess pc %x\n", Node->getGuestPC());

    BasicBlock *ifTrue = BasicBlock::Create(Context, "main_path", Func);
    BasicBlock *ifTrue2 = BasicBlock::Create(Context, "main_path2", Func);
    BasicBlock *ifFalse = BasicBlock::Create(Context, "exit_stub", Func);
    Value *NextPC = ConstantInt::get(GuessPC->getType(), Node->getGuestPC());
    Value *NextNPC = ConstantInt::get(GuessNPC->getType(), Node->getGuestCS());

    Value *Cond = ICMP(GuessPC, NextPC, ICmpInst::ICMP_EQ);
    BranchInst::Create(ifTrue, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();
    
    BranchInst *BI = LastInst = BranchInst::Create(ExitBB, ifTrue);
    Cond = ICMP(GuessNPC, NextNPC, ICmpInst::ICMP_EQ);
    BranchInst::Create(ifTrue2, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();

    /* First set the branch to exit BB, and the link will be resolved 
       at the trace finalization procedure. */
    BI = BranchInst::Create(ExitBB, ifTrue2);
    TBuilder->setSuccessor(BI, Node);
    
    CurrBB = ifFalse;
    LastInst = BranchInst::Create(ExitBB, CurrBB);
}

void IRFactory::TraceLinkDirectJump(GraphNode *Node, StoreInst *SI, StoreInst *SI2, int mode)
{
    ConstantInt *PC = static_cast<ConstantInt *>(SI->getOperand(0));
    ConstantInt *NPC = static_cast<ConstantInt *>(SI2->getOperand(0));
    target_ulong pc = (target_ulong)PC->getZExtValue();
    DM->Debug(DEBUG_IR, "Find direct branch pc %"PRIx".\n", pc);

    SaveGlobals(COHERENCE_ALL, SI);

    if (mode == CHAINING_TRACE_SELF)
    {
        /* First set the branch to exit BB, and the link will be resolved 
           at the trace finalization procedure. */
        BranchInst *BI = BranchInst::Create(ExitBB, SI);
        TBuilder->setSuccessor(BI, Node);
        toErase.push_back(SI);
        toErase.push_back(SI2);
    }
    else if (mode == CHAINING_TRACE_BLOCK)
    {
        tcg_target_ulong Key, RetVal;
        getChainSlot(0, &Key, &RetVal);

        Value *Meta1[] = { getUID(), CONST32(PATCH_TRACE_BLOCK_CHAINING), CONST32(CallIdx), CONSTPtr(Key), PC };
        Value *Meta2[] = { getUID(), CONST32(PATCH_EXIT_TB), CONSTPtr(RetVal), };

        InsertSeparator(SI);
        InsertPatchJmp(CreateMDNode(Context, Meta1, 5), SI);
        InsertPatchReturn(CreateMDNode(Context, Meta2, 3), RetVal, LastInst);

        TBuilder->setExit(pc);
    }
    else
        IRError("%s: invalid linking mode (%d).\n", __func__, mode);
}

int IRFactory::TraceLink(StoreInst *SI)
{
    /* Locate the two instructions to store pc and npc. Associate store-pc to
     * SI and store-npc to SI2. */
    StoreInst *SI2 = dyn_cast<StoreInst>(--BasicBlock::iterator(SI));
    TraceCheckStoreEIP(SI2);
    if (SI->getPointerOperand()->getName() == "npc")
    {
        StoreInst *Tmp = SI;
        SI = SI2;
        SI2 = Tmp;
    }

    ConstantInt *CI = dyn_cast<ConstantInt>(SI->getOperand(0));
    ConstantInt *CI2 = dyn_cast<ConstantInt>(SI2->getOperand(0));
    if (CI == NULL || CI2 == NULL) /* Indirect branch */
    {
        InsertSeparator(SI);
        SaveGlobals(COHERENCE_ALL, LastInst);

        GraphNode *CurrNode = TBuilder->getCurrNode();
        NodeVec &Successor = CurrNode->getSuccessor();
        for (int i = 0, e = Successor.size(); i < e; i++)
            TraceLinkIndirectJump(Successor[i], SI->getOperand(0), SI2->getOperand(0));

        InsertLookupIBTC(SI->getOperand(0), SI2->getOperand(0), LastInst);
        return 0;
    }
    else /* Direct branch */
    {
        /* Try to link a direct branch. */
        target_ulong PC = CI->getZExtValue();
        target_ulong NPC = CI2->getZExtValue();

        GraphNode *Successor = TBuilder->lookupNode(PC, NPC);
        if (Successor != NULL)
        {
            TraceLinkDirectJump(Successor, SI, SI2, CHAINING_TRACE_SELF);
            return 2;
        }

        TraceLinkDirectJump(NULL, SI, SI2, CHAINING_TRACE_BLOCK);
        TraceRenameBB(SI, "block");
        return 1;
    }
}

int IRFactory::TraceLinkPHI()
{
    int i = 0;
    StoreInst *SI[2];

    for (pred_iterator PI = pred_begin(CurrBB), PE = pred_end(CurrBB); PI != PE; ++PI)
    {
        SI[i] = dyn_cast<StoreInst>(--BasicBlock::iterator((*PI)->back()));
        TraceCheckStoreEIP(SI[i]);
        i++;
    }

    PHINode *PN = CreatePHI(SI[0]->getOperand(0)->getType(), 2, LastInst);
    PN->addIncoming(SI[0]->getOperand(0), SI[0]->getParent());
    PN->addIncoming(SI[1]->getOperand(0), SI[1]->getParent());

    SaveGlobals(COHERENCE_ALL, LastInst);

    BasicBlock *PredBB = SI[0]->getParent()->getUniquePredecessor();
    StoreInst *StoreEIP = NULL;
    for (BasicBlock::iterator BI = PredBB->end(), BE = PredBB->begin(); BI != BE; BI--)
    {
        StoreInst *I = dyn_cast<StoreInst>(BI);
        if (I && I->getPointerOperand()->getName() == "pc")
        {
            TraceCheckStoreEIP(I);
            StoreEIP = I;
            break;
        }
    }

    GraphNode *CurrNode = TBuilder->getCurrNode();
    int NumSucc = CurrNode->getSuccessor().size();
    if (NumSucc > 1)
        IRError("%s: FIXME. %d\n", __func__, NumSucc);

    if (NumSucc == 1)
        TraceLinkIndirectJump(CurrNode->getSuccessor().back(), StoreEIP->getOperand(0), PN);
    
    InsertLookupIBTC(StoreEIP->getOperand(0), PN, LastInst);

    return 0;
}
#else

inline void IRFactory::InsertLookupIBTC(Value *GuestEIP, 
        Instruction *InsertBefore)
{
    SmallVector<Value *, 4> Params;

#if defined(ENABLE_IBCHAIN)
    unsigned long Key;
    IBChainVec &IBChainPoint = Env->getIBChainPoint();
    IBChainInfo *ChainInfo = new IBChainInfo();

    llvm_spin_lock(&llvm_interface_lock);
    Key = IBChainPoint.size();
    Key = (Key << 2) | 3;
    IBChainPoint.push_back(ChainInfo);
    llvm_spin_unlock(&llvm_interface_lock);

    Value *Meta1[] = { getUID(), CONST32(PATCH_IBCHAIN), CONSTPtr((unsigned long)ChainInfo)};
    MDNode *MD = CreateMDNode(Context, Meta1, 3);
    Value *KeyVal = ConstantExpr::getIntToPtr(CONSTPtr((uintptr_t)Key),
            PointerType::getUnqual(Int8Ty));

    Function *Fn = Mod->getFunction("helper_lookup_ibchain");
    Params.push_back(GuestEIP);
    Params.push_back(KeyVal);
    CallInst *CI = CreateCall(Fn, Params, "", InsertBefore);
    CI->setMetadata("flags", MD);
    CI->setMetadata("exit", MD);

    InsertPatchIBChain(MD, InsertBefore);
    setInsertPos(CI);
#else
    Value *Meta1[] = { getUID(), };

    Function *Fn = Mod->getFunction("helper_lookup_ibtc");
    
    Params.push_back(GuestEIP);
    CallInst *CI = CreateCall(Fn, Params, "", InsertBefore);
    CI->setMetadata("flags", CreateMDNode(Context, Meta1, 1));

    Value *Meta2[] = { CONST32(0), CONST32(TBuilder->getCurrDepth()), };
    IndirectBrInst *IB = IndirectBrInst::Create(CI, 1, InsertBefore);
    IB->setMetadata("exit", CreateMDNode(Context, Meta2, 2));
    setInsertPos(CI);
#endif
}

void IRFactory::TraceLinkIndirectJump(GraphNode *Node, Value *GuessPC)
{
    DM->Debug(DEBUG_LLVM, "Find indirect branch. Guess pc %x\n", Node->getGuestPC());

    BasicBlock *ifTrue = BasicBlock::Create(Context, "main_path", Func);
    BasicBlock *ifFalse = BasicBlock::Create(Context, "exit_stub", Func);
    Value *NextPC = ConstantInt::get(GuessPC->getType(), Node->getGuestPC());

    Value *Cond = ICMP(GuessPC, NextPC, ICmpInst::ICMP_EQ);
    BranchInst::Create(ifTrue, ifFalse, Cond, LastInst);
    LastInst->eraseFromParent();
    
    /* First set the branch to exit BB, and the link will be resolved 
       at the trace finalization procedure. */
    BranchInst *BI = BranchInst::Create(ExitBB, ifTrue);
    TBuilder->setSuccessor(BI, Node);
    
    CurrBB = ifFalse;
    LastInst = BranchInst::Create(ExitBB, CurrBB);
}

void IRFactory::TraceLinkDirectJump(GraphNode *Node, StoreInst *SI, int mode)
{
    ConstantInt *PC = static_cast<ConstantInt *>(SI->getOperand(0));
    target_ulong pc = (target_ulong)PC->getZExtValue();
    DM->Debug(DEBUG_IR, "Find direct branch pc %"PRIx".\n", pc);

    SaveGlobals(COHERENCE_ALL, SI);

    if (mode == CHAINING_TRACE_SELF)
    {
        /* First set the branch to exit BB, and the link will be resolved 
           at the trace finalization procedure. */
        BranchInst *BI = BranchInst::Create(ExitBB, SI);
        TBuilder->setSuccessor(BI, Node);
        toErase.push_back(SI);
    }
    else if (mode == CHAINING_TRACE_BLOCK)
    {
        tcg_target_ulong Key, RetVal;
        getChainSlot(0, &Key, &RetVal);

        Value *Meta1[] = { getUID(), CONST32(PATCH_TRACE_BLOCK_CHAINING), CONST32(CallIdx), CONSTPtr(Key), PC };
        Value *Meta2[] = { getUID(), CONST32(PATCH_EXIT_TB), CONSTPtr(RetVal), };

        InsertSeparator(SI);
        InsertPatchJmp(CreateMDNode(Context, Meta1, 5), SI);
        InsertPatchReturn(CreateMDNode(Context, Meta2, 3), RetVal, LastInst);

        TBuilder->setExit(pc);
    }
    else
        IRError("%s: invalid linking mode (%d).\n", __func__, mode);
}

int IRFactory::TraceLink(StoreInst *SI)
{
    ConstantInt *CI = dyn_cast<ConstantInt>(SI->getOperand(0));
    if (CI == NULL) /* Indirect branch */
    {
        InsertSeparator(SI);
        SaveGlobals(COHERENCE_ALL, LastInst);

        GraphNode *CurrNode = TBuilder->getCurrNode();
        NodeVec &Successor = CurrNode->getSuccessor();
        for (int i = 0, e = Successor.size(); i < e; i++)
            TraceLinkIndirectJump(Successor[i], SI->getOperand(0));

        InsertLookupIBTC(SI->getOperand(0), LastInst);
        return 0;
    }
    else /* Direct branch */
    {
        /* Try to link a direct branch. */
        target_ulong pc = CI->getZExtValue();
        target_ulong cs_base = TBuilder->getCurrNode()->getGuestCS();

        GraphNode *Successor = TBuilder->lookupNode(pc, cs_base);
        if (Successor != NULL)
        {
            TraceLinkDirectJump(Successor, SI, CHAINING_TRACE_SELF);
            return 2;
        }

        TraceLinkDirectJump(NULL, SI, CHAINING_TRACE_BLOCK);
        TraceRenameBB(SI, "block");
        return 1;
    }
}

int IRFactory::TraceLinkPHI()
{
    int i = 0;
    StoreInst *SI[2];

    for (pred_iterator PI = pred_begin(CurrBB), PE = pred_end(CurrBB); PI != PE; ++PI)
    {

#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
        SI[i] = ReformatStoreEIP(BasicBlock::iterator((*PI)->back()));
#else
        SI[i] = dyn_cast<StoreInst>(--BasicBlock::iterator((*PI)->back()));
#endif
        TraceCheckStoreEIP(SI[i]);
        i++;
    }

    PHINode *PN = CreatePHI(SI[0]->getOperand(0)->getType(), 2, LastInst);
    PN->addIncoming(SI[0]->getOperand(0), SI[0]->getParent());
    PN->addIncoming(SI[1]->getOperand(0), SI[1]->getParent());

    SaveGlobals(COHERENCE_ALL, LastInst);

    GraphNode *CurrNode = TBuilder->getCurrNode();
    int NumSucc = CurrNode->getSuccessor().size();
    if (NumSucc > 1)
        IRError("%s: FIXME. %d\n", __func__, NumSucc);

    if (NumSucc == 1)
        TraceLinkIndirectJump(CurrNode->getSuccessor().back(), PN);
    
    InsertLookupIBTC(PN, LastInst);

    return 0;
}
#endif /* TARGET_SPARC */


StoreInst *IRFactory::ReformatStoreEIP(Instruction *Inst)
{
    vector< pair<size_t, StoreInst*> > StoreEIP;

    /* Search for the store instructions which write value to the EIP regisgter
     * in this basic block */
    bool hasConstEIP = true;
    size_t MinOff = (size_t)-1;
    BasicBlock *BB = Inst->getParent();
    for (BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; BI++)
    {
        if (StoreInst *SI = dyn_cast<StoreInst>(BI))
        {
            size_t Off = 0;
            Value *Base = getBaseWithConstantOffset(TD, SI->getPointerOperand(), Off);
            if (Base == BaseReg[BaseRegNo].Base && isStoreEIP(Off))
            {
                StoreEIP.push_back(make_pair(Off, SI));
                if (Off < MinOff)
                    MinOff = Off;
                if (isa<ConstantInt>(SI->getOperand(0)) == false)
                    hasConstEIP = false;
            }
        }
    }

    if (StoreEIP.size() == 1)
        return StoreEIP[0].second;

    /* Reformat the instructions writing to EIP with one single store instruction. */
    StoreInst *LastStoreEIP = NULL;
    size_t StoreSize = 0;
    target_ulong NewConstEIP = 0;
    Value *NewEIP = NULL;
    CONST Type *Ty = (TARGET_LONG_BITS == 32) ? Int32Ty : Int64Ty;
    for (int i = 0, e = StoreEIP.size(); i < e; i++)
    {
        size_t Shift = (StoreEIP[i].first - MinOff) * 8;
        StoreInst *SI = StoreEIP[i].second;
        size_t Size = TD->getTypeSizeInBits(SI->getOperand(0)->getType());

        StoreSize += Size;
        if (hasConstEIP)
        {
            target_ulong ConstEIP = static_cast<ConstantInt*>(SI->getOperand(0))->getZExtValue();
            if (Shift)
                ConstEIP <<= Shift;
            NewConstEIP |= ConstEIP;
        }
        else
        {
            Value *V = ZEXT(SI->getOperand(0), Ty);
            if (Shift)
                V = SHL(V, ConstantInt::get(Ty, Shift));
            NewEIP = (NewEIP == NULL) ? V : OR(NewEIP, V);
        }

        /* If the total write size is equal to the guest EIP size, create 
         * the store instruction now. */
        if (StoreSize == TARGET_LONG_BITS)
        {
            if (hasConstEIP)
                NewEIP = ConstantInt::get(Ty, NewConstEIP);

            Instruction *InsertPos = (i == e - 1) ? LastInst : BasicBlock::iterator(SI);
            Value *Addr = GetElementPtrInst::Create(CPU, CONST32(MinOff), "", InsertPos);
            Addr = new BitCastInst(Addr, getPointerTy(TARGET_LONG_BITS), "", InsertPos);
            LastStoreEIP = new StoreInst(NewEIP, Addr, false, InsertPos);

            StoreSize = 0;
            NewConstEIP = 0;
            NewEIP = NULL;
        }
        toErase.push_back(SI);
    }

    return LastStoreEIP;
}

/*
 * op_exit_tb()
 *  args[0]: return value
 */
int IRFactory::op_exit_tb(const TCGArg *args)
{
    IRDebug(INDEX_op_exit_tb);

    if (Mode == LLVM_GEN_BLOCK)
    {
        SaveGlobals(COHERENCE_ALL, LastInst);

        tcg_target_ulong Key, RetVal;
        getChainSlot(args[0], &Key, &RetVal);

        Value *Meta[] = { getUID(), CONST32(PATCH_EXIT_TB), CONSTPtr(RetVal), };
        InsertPatchReturn(CreateMDNode(Context, Meta, 3), RetVal, LastInst);
    }
    else if (Mode == LLVM_GEN_TRACE || Mode == LLVM_REGEN_TRACE)
    {
        /* Architectures whose EIP is not populated to the LLVM IR (e.g. ARM), 
           we must store EIP before we continue to do trace building. */
        SaveTraceEIP(LastInst);

        int ret = 0;
        if (LastInst == LastInst->getParent()->begin())
        {
            if (strstr(getNameCString(CurrBB), "direct_jump_tb") != NULL)
            {
                int NumPreds = std::distance(pred_begin(CurrBB), pred_end(CurrBB));
                if (NumPreds != 2)
                    IRError("%s: internal error. NumPreds = %d\n", __func__, NumPreds);
                
                ret = TraceLinkPHI();
            }
            else
            {
                
                SaveGlobals(COHERENCE_ALL, LastInst);

                tcg_target_ulong Key, RetVal;
                getChainSlot(args[0], &Key, &RetVal);

                Value *Meta[] = { getUID(), CONST32(PATCH_EXIT_TB), CONSTPtr(RetVal), };
                InsertPatchReturn(CreateMDNode(Context, Meta, 3), RetVal, LastInst);
                ret = 1;
            }
        }
        else if (CallInst *CI = dyn_cast<CallInst>(--BasicBlock::iterator(LastInst)))
        {
            tcg_target_ulong Key, RetVal;
            getChainSlot(args[0], &Key, &RetVal);

            Value *Meta[] = { getUID(), CONST32(PATCH_EXIT_TB), CONSTPtr(RetVal), };
            InsertPatchReturn(CreateMDNode(Context, Meta, 3), RetVal, LastInst);
            setInsertPos(CI);
            ret = 1;
        }
        else
        {
#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
            StoreInst *SI = ReformatStoreEIP(LastInst);
#else
            StoreInst *SI = dyn_cast<StoreInst>(--BasicBlock::iterator(LastInst));
#endif
            TraceCheckStoreEIP(SI);
            ret = TraceLink(SI);
        }

        /* Add unreachable instruction to the end of block if it goes 
           out of trace or to the trace loop header. */
        if (ret == 1)
        {
            Value *Meta[] = { CONST32(1), CONST32(TBuilder->getCurrDepth()), };
            Instruction *I = --BasicBlock::iterator(LastInst);
            if (dyn_cast<TerminatorInst>(I) == NULL)
                DM->Error("%s: cannot find terminator instruction.\n", __func__);
            
            I->setMetadata("exit", CreateMDNode(Context, Meta, 2));
        }
    }
    else
        IRError("%s: invalid mode %d\n", __func__, Mode);

    LastInst->eraseFromParent();
    LastInst = NULL;
    return llvm_op_defs[INDEX_op_exit_tb].nb_args;
}

/*
 * op_goto_tb()
 *  args[0]: jump index
 */
int IRFactory::op_goto_tb(const TCGArg *args)
{
    IRDebug(INDEX_op_goto_tb);

#ifdef USE_DIRECT_JUMP
    int jmp_idx = args[0];

    if (Mode == LLVM_GEN_BLOCK)
    {
        Value *Meta[] = { getUID(), CONST32(PATCH_DIRECT_JUMP), CONST32(CallIdx), CONST32(jmp_idx) };

        SaveGlobals(COHERENCE_ALL, LastInst);
        InsertPatchJmp(CreateMDNode(Context, Meta, 4), LastInst);
    }
#else
    IRError("%s: TODO: fix me.\n", __func__);
#endif

    return llvm_op_defs[INDEX_op_goto_tb].nb_args;
}

#if TCG_TARGET_REG_BITS == 32
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld8u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld8u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates()

    InData = QEMULoad(InData, GUEST_BASE, 0, args[2]);
    InData = ZEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld8u].nb_args;
}
#else
int IRFactory::op_qemu_ld8u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld8u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates()

    InData = QEMULoad(InData, GUEST_BASE, 0, args[2]);
    InData = ZEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld8u].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld8s(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld8s);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 0, args[2]);
    InData = SEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld8s].nb_args;
}

#else
int IRFactory::op_qemu_ld8s(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_qemu_ld8s].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld16u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld16u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 1, args[2]);
    DO_BSWAP16(InData);
    InData = ZEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld16u].nb_args;
}
#else
int IRFactory::op_qemu_ld16u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld16u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 1, args[2]);
    DO_BSWAP16(InData);
    InData = ZEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld16u].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld16s(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld16s);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 1, args[2]);
    DO_BSWAP16(InData);
    InData = SEXT32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld16s].nb_args;
}
#else
int IRFactory::op_qemu_ld16s(const TCGArg *args)
{
    IRError("%s not implemented.\n", __func__);
    return llvm_op_defs[INDEX_op_qemu_ld16s].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 2, args[2]);
    DO_BSWAP32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld32].nb_args;
}
#else
int IRFactory::op_qemu_ld32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 2, args[2]);
    DO_BSWAP32(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld32].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_ld64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld64);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In = &Reg[args[2]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = ZEXT64(InData);
    Value *OutData = QEMULoad(InData, GUEST_BASE, 3, args[3]);

    DO_BSWAP64(OutData);

    Value *OutData1 = TRUNC32(OutData);
    Value *OutData2 = TRUNC32(LSHR(OutData, CONST64(32)));

    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_qemu_ld64].nb_args;
}
#else
int IRFactory::op_qemu_ld64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld64);

    Register *Out1 = &Reg[args[0]];
    Register *Out2 = &Reg[args[1]];
    Register *In = &Reg[args[2]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = ZEXT64(InData);
    Value *OutData = QEMULoad(InData, GUEST_BASE, 3, args[3]);

    DO_BSWAP64(OutData);

    Value *OutData1 = TRUNC32(OutData);
    Value *OutData2 = TRUNC32(LSHR(OutData, CONST64(32)));

    Out1->setData(OutData1, true);
    Out2->setData(OutData2, true);

    return llvm_op_defs[INDEX_op_qemu_ld64].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_st8(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st8);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC8(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 0, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st8].nb_args;
}
#else
int IRFactory::op_qemu_st8(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st8);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC8(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 0, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st8].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_st16(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st16);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC16(InData1);
    DO_BSWAP16(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 1, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st16].nb_args;
}
#else
int IRFactory::op_qemu_st16(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st16);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC16(InData1);
    DO_BSWAP16(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 1, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st16].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_st32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    DO_BSWAP32(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 2, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st32].nb_args;
}
#else
int IRFactory::op_qemu_st32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    DO_BSWAP32(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 2, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st32].nb_args;
}
#endif
#if TARGET_LONG_BITS == 32
int IRFactory::op_qemu_st64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st64);
    
    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    Register *In3 = &Reg[args[2]];
    
    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);

    AssertNotPointer(InData3);

    SaveStates();

    InData1 = ZEXT64(InData1);
    InData2 = ZEXT64(InData2);
    InData3 = ZEXT64(InData3);

    InData2 = SHL(InData2, CONST64(32));
    InData1 = OR(InData1, InData2);
    DO_BSWAP64(InData1);
    QEMUStore(InData1, InData3, GUEST_BASE, 3, args[3]);

    return llvm_op_defs[INDEX_op_qemu_st64].nb_args;
}
#else
int IRFactory::op_qemu_st64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st64);
    
    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];
    Register *In3 = &Reg[args[2]];
    
    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);
    Value *InData3 = LoadInput(In3);

    AssertNotPointer(InData3);

    SaveStates();

    InData1 = ZEXT64(InData1);
    InData2 = ZEXT64(InData2);
    InData3 = ZEXT64(InData3);

    InData2 = SHL(InData2, CONST64(32));
    InData1 = OR(InData1, InData2);
    DO_BSWAP64(InData1);
    QEMUStore(InData1, InData3, GUEST_BASE, 3, args[3]);

    return llvm_op_defs[INDEX_op_qemu_st64].nb_args;
}
#endif
#else /* TCG_TARGET_REG_BITS == 32 */
/*
 * op_qemu_ld8u()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld8u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld8u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 0, args[2]);
    InData = ZEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld8u].nb_args;
}

/*
 * op_qemu_ld8s()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld8s(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld8s);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 0, args[2]);
    InData = SEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld8s].nb_args;
}

/*
 * op_qemu_ld16u()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld16u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld16u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 1, args[2]);
    DO_BSWAP16(InData);
    InData = ZEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld16u].nb_args;
}

/*
 * op_qemu_ld16s()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld16s(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld16s);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 1, args[2]);
    DO_BSWAP16(InData);
    InData = SEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld16s].nb_args;
}

/*
 * op_qemu_ld32()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld32);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 2, args[2]);
    DO_BSWAP32(InData);
    if (Out->Size == 64)
        InData = ZEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld32].nb_args;
}

int IRFactory::op_qemu_ld32u(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld32u);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 2, args[2]);
    DO_BSWAP32(InData);
    if (Out->Size == 64)
        InData = ZEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld32u].nb_args;
}

int IRFactory::op_qemu_ld32s(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld32s);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 2, args[2]);
    DO_BSWAP32(InData);
    if (Out->Size == 64)
        InData = SEXT(InData, Out->Type);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld32s].nb_args;
}

/*
 * op_qemu_ld64()
 *  args[0]: Out (data reg)
 *  args[1]: In  (base)
 */
int IRFactory::op_qemu_ld64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_ld64);

    Register *Out = &Reg[args[0]];
    Register *In = &Reg[args[1]];

    Value *InData = LoadInput(In);

    AssertNotPointer(InData);

    SaveStates();

    InData = QEMULoad(InData, GUEST_BASE, 3, args[2]);
    DO_BSWAP64(InData);
    Out->setData(InData, true);

    return llvm_op_defs[INDEX_op_qemu_ld64].nb_args;
}

/*
 * op_qemu_st8()
 *  args[0]: In1 (data reg)
 *  args[1]: In2 (base)
 */
int IRFactory::op_qemu_st8(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st8);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC8(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 0, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st8].nb_args;
}

/*
 * op_qemu_st16()
 *  args[0]: In1 (data reg)
 *  args[1]: In2 (base)
 */
int IRFactory::op_qemu_st16(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st16);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    InData1 = TRUNC16(InData1);
    DO_BSWAP16(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 1, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st16].nb_args;
}

/*
 * op_qemu_st32()
 *  args[0]: In1 (data reg)
 *  args[1]: In2 (base)
 */
int IRFactory::op_qemu_st32(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st32);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    if (In1->Size == 64)
        InData1 = TRUNC32(InData1);

    DO_BSWAP32(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 2, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st32].nb_args;
}

/*
 * op_qemu_st64()
 *  args[0]: In1 (data reg)
 *  args[1]: In2 (base)
 */
int IRFactory::op_qemu_st64(const TCGArg *args)
{
    IRDebug(INDEX_op_qemu_st64);

    Register *In1 = &Reg[args[0]];
    Register *In2 = &Reg[args[1]];

    Value *InData1 = LoadInput(In1);
    Value *InData2 = LoadInput(In2);

    AssertNotPointer(InData2);

    SaveStates();

    DO_BSWAP64(InData1);
    QEMUStore(InData1, InData2, GUEST_BASE, 3, args[2]);

    return llvm_op_defs[INDEX_op_qemu_st64].nb_args;
}

#endif /* TCG_TARGET_REG_BITS != 32 */

Value *IRFactory::QEMULoad(Value *base, tcg_target_long offset, int sizeop,
        int mem_index)
{
    CONST PointerType *PtrTy=NULL;

    switch (sizeop)
    {
        case 2: PtrTy = Int32PtrTy; break;
        case 3: PtrTy = Int64PtrTy; break;
        case 0: PtrTy = Int8PtrTy;  break;
        case 1: PtrTy = Int16PtrTy; break;
        default:
            IRError("%s: invalid bit type.\n", __func__);
    }

    offset >>= sizeop;
    if (offset)
        return LOAD(ITP(base, PtrTy), offset);
    else
        return new LoadInst(ITP(base, PtrTy), "", true, LastInst);
}

void IRFactory::QEMUStore(Value *data, Value *base, tcg_target_long offset,
        int sizeop, int mem_index)
{
    CONST PointerType *PtrTy=NULL;

    switch (sizeop)
    {
        case 2: PtrTy = Int32PtrTy; break;
        case 3: PtrTy = Int64PtrTy; break;
        case 0: PtrTy = Int8PtrTy;  break;
        case 1: PtrTy = Int16PtrTy; break;
        default:
            IRError("%s: invalid bit type.\n", __func__);
    }

    offset >>= sizeop;
    if (offset)
        STORE(data, ITP(base, PtrTy), offset);
    else
        new StoreInst(data, ITP(base, PtrTy), true, LastInst);
}

inline Value *IRFactory::CreateBSwap(CONST Type *Ty, Value *V, Instruction *InsertPos)
{
    SmallVector<Value *, 4> Params;
    CONST Type *Tys[] = { Ty };

#if defined(LLVM_V28) || defined(LLVM_V29)
    Function *Fn = Intrinsic::getDeclaration(Mod, Intrinsic::bswap, Tys, 1);
#else
    Function *Fn = Intrinsic::getDeclaration(Mod, Intrinsic::bswap, Tys);
#endif

    Params.push_back(V);
    return CreateCall(Fn, Params, "", InsertPos);
}

MDNode *CreateMDNode(LLVMContext &Context, Value **M, int count)
{
#if 0
    return MDNode::getWhenValsUnresolved(Context, M, count, true);
#endif
#if defined(LLVM_V28) || defined(LLVM_V29)
    return MDNode::get(Context, M, count);
#else
    SmallVector<Value*, 4> V;
    for (int i = 0; i < count; i++)
        V.push_back(M[i]);
    return MDNode::get(Context, V);
#endif
}

CallInst *CreateCall(Value *Fn, const char *Name, Instruction *InsertPos)
{
    return CallInst::Create(Fn, Name, InsertPos);
}

CallInst *CreateCall(Value *Fn, SmallVector<Value *, 4> &Params,
        const char *Name, Instruction *InsertPos)
{
#if defined(LLVM_V28) || defined(LLVM_V29)
    return CallInst::Create(Fn, Params.begin(), Params.end(),
            Name, InsertPos);
#else
    return CallInst::Create(Fn, Params, Name, InsertPos);
#endif
}

PHINode *CreatePHI(CONST Type *Ty, int Count, Instruction *InsertPos)
{
#if defined(LLVM_V28) || defined(LLVM_V29)
    return PHINode::Create(Ty, "", InsertPos);
#else
    return PHINode::Create(Ty, Count, "", InsertPos);
#endif
}

#undef IRDebug
#undef IRError

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
