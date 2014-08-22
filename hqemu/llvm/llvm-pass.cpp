/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include "llvm/Operator.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm-profile.h"
#include "llvm.h"
#include "utils.h"


extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern ProfileFactory *PF;
extern int HybridMode;

class StateInfo;

typedef set< Instruction* > ISet;
typedef map< size_t, AllocaInst* > AIMap;
typedef map< size_t, StateInfo > StateInfoMap;
typedef vector< pair<size_t, Instruction*> > MemOpVec;
typedef map< BasicBlock*, MemOpVec > MemOpMap;
typedef set< size_t > StateSet;
typedef map< size_t, Value* > StateMap;
typedef map< BasicBlock*, vector<StateMap> > StateBucket;
typedef map< BasicBlock*, BBVec > PredBucket;
typedef vector< pair<Value*,Value*> > PVec;
typedef map< Value*, int > ValMap;

#define CINT(a) ConstantInt::get(V->getType(), a)


/*
 * HelperPass is used to determine if a Function consists of any loop.
 */
class HelperPass : public FunctionPass
{
public:
    static char ID;
    explicit HelperPass() : FunctionPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage &AU) const
    {
        AU.addRequired<LoopInfo>();
    }
    
    bool runOnFunction(Function &F);
};

bool HelperPass::runOnFunction(Function &F)
{
    LoopInfo *LI = &getAnalysis<LoopInfo>();
    return !LI->empty();
}

char HelperPass::ID = 0;
static RegisterPass<HelperPass> HelperP("helper", "Helper Function Pass", false, false);
FunctionPass *createHelperPass()
{
    return new HelperPass();
}


/*
 * StateInfo is used to provide the information about a state of CPUState after
 * analyzing the load/store operations within a translation function.
 */
struct StateInfo
{
    StateInfo() : isSameTy(true), isAligned(true), isVector(false), Off(-1U), 
                  Ty(NULL), Count(0) {}

    bool isSameTy;   /* The loads/stores have the same access type or not */
    bool isAligned;  /* The loads/stores access aligned address or not */
    bool isVector;   /* The state is with a vector type or not */
    size_t Off;      /* The offset of the state in CPUState */
    CONST Type *Ty;  /* Type of the state */
    size_t Size;     /* The size of the state */
    Value *Addr;     /* The address of the state in the memory */
    AllocaInst *AI;  /* The mapped Alloca of the state */
    string Name;     /* The name of the state if it is a global register */
    uint16_t Count;

    /* setState - Set the information of the state. */
    void setState(const TargetData *TD, size_t off, Value *v, Value *addr)
    {
        if (Ty == NULL)
        {
            Off = off;
            Ty = v->getType();
            Size = TD->getTypeSizeInBits(Ty);
            Addr = addr;
            Name = getNameString(addr);
            if (Name != "")
                Name = Name + ".a";
        }
        else
        {
            if (off == Off && v->getType() == Ty)
                return;

            if (off != Off || TD->getTypeSizeInBits(v->getType()) != Size)
                isAligned = false;
            else
                isSameTy = false;
        }
        Count++;
    }
};

/*
 * StateMappingPass is used to eliminate the redundant loads and stores to the
 * CPUState. The loads and stores of the guest memory operations are not
 * removed in order not to violate the memory model of the guest architecture.
 */
class StateMappingPass : public FunctionPass 
{
private:
    /* Basic types. */
    CONST Type *VoidTy;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *IntPtrTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int16PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;

    IRFactory *IF;               /* Uplink to the IRFactory */
    LLVMTranslator *Translator;  /* Uplink to the LLVMTranslator */
    LLVMContext *Context;        /* LLVM context */
    const TargetData *TD;        /* Target data */
    Instruction *CPU;            /* Base register with (char*) type */
    Instruction *CPUStruct;      /* Base register with (struct CPUState*) type */
    Value *CPUStructAsm;         /* Inline asm of base register with 
                                    (struct CPUState*) type */
    BasicBlock *InitBB;          /* The BasicBlock for variable decalaration */
    TraceInfo *Trace;            /* The region to be translated */
    int Pass;

    map<string, HelperInfo*> HelperFuncs;  /* Info of QEMU helper functions */

    int BaseRegNo;           /* Base register # to hold the CPUState pointer */
    BaseRegister *BaseReg;   /* Base register */
    CONST Type *BaseRegType; /* LLVM Type of the base register */
    StateBucket StateBuck;   /* The states after processing a BasicBlock */
    PredBucket PredBuck;     /* The list of the predecessor BasicBlocks */
    BBSet ExitBB;            /* The leaf BasicBlocks */
    BBVec Worklist;          /* The work queue for non-loop Function */
    BBMap AcyclicBB;         /* The BasicBlocks having multiple predecessors */
    Instruction *PreloadPos; /* The position to preload the CPU states */
    IVec toErase;            /* The instructions to be removed */

    StateInfoMap State;      /* The accessed states in the Function */
    MemOpVec OpVec;          /* CPUState loads/stores in the Function */
    MemOpMap OpMap;          /* CPUState loads/stores in a BasicBlock */
    IVec Calls;              /* Helper function calls in the Function */

public:
    static char ID;
    explicit StateMappingPass() : FunctionPass(ID) {}
    explicit StateMappingPass(IRFactory *IRF) : FunctionPass(ID), IF(IRF),
                Context(&IF->getContext())
    {
        /* Get information from IRFactory and LLVMTranslator. */
        LLVMTranslator *Translator = IF->Translator;
        TD = Translator->getTargetData();
        HelperFuncs = Translator->getHelperFuncs();

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

        BaseRegNo = Translator->BaseRegNo;
        BaseRegType = Translator->BaseRegType;

#if defined(TARGET_I386)
        X86FPSttOff = Translator->X86FPOff[0];
        X86FPTagOff = Translator->X86FPOff[1];
        X86FPRegOff = Translator->X86FPOff[2];
#endif
    }

    /* StripPointer - Get the pointer without GEP and BitCast. */
    void StripPointer(Value *V, IVec &IV);

    /* MovePointer - Move the pointer before InsertPos. */
    void MovePointer(Value *V, Instruction *InsertPos);

    /* RearrangePreload - Move store/load/alloca instructions to the correct
     * position. */
    void RearrangePreload();

    /* ConvertType - Bitcast a value to another type. */
    Value *ConvertType(Value *V, CONST Type *Ty, Instruction *InsertPos);

    /* LoadUnalignedState - Load value from an unaligned state. */
    Value *LoadUnalignedState(Value *V, Value *SubV, size_t Diff,
            Instruction *InsertPos);

    /* StoreUnalignedState - Store value to an unaligned state. */
    Value *StoreUnalignedState(Value *V, Value *SubV, size_t Diff,
            Instruction *InsertPos);

    /* LoadVector - Load value from a vector element. */
    Value *LoadVector(Value *V, Value *SubV, size_t Diff,
            Instruction *InsertPos);

    /* StoreVector - Store value to a vector element. */
    Value *StoreVector(Value *V, Value *SubV, size_t Diff,
            Instruction *InsertPos);

    /* MergeState - Merge the incoming states in a convergent block. */
    void MergeState(BasicBlock *BB, StateMap &Merge);

    /* MapState - Propagate the states to the successor block. */
    void MapState(BasicBlock *BB, StateMap &PredState);

    /* CopyState - Load state from Src and store it to Dest. */
    void CopyState(Value *Dest, Value *Src, Instruction *InsertPos);

    /* ProcessExitBB - Store dirty states at the leaf blocks. */
    void ProcessExitBB(BasicBlock *BB, StateSet &StateDef);
    void ProcessExitBB(BasicBlock *BB, StateMap &SM);

    /* AnalyzeStates - Scan all instruction in a Function and analyze the
     * instructions that access the CPUState. */
    void AnalyzeStates(Function &F);

    bool SyncHelperState(StateSet &StateUse, StateSet &StateDef);
    void SyncDef(StateSet &DeadDef, Instruction *InsertPos);
    void SyncUse(StateSet &DeadUse, Instruction *InsertPos);

#if defined(TARGET_I386)
    size_t X86FPSttOff;  /* Offset of the x86 fpstt state */
    size_t X86FPTagOff;  /* Offset of the x86 fptag state */
    size_t X86FPRegOff;  /* Offset of the x86 fpreg state */

    Value *getX86FPSttPtr(Value *V);
    int getX86FPIndex(Value *V, ValMap &FPSttIndex);
    int setX86FPIndex(Value *V, ValMap &FPSttIndex, int &LastIndex);
    bool X86FPMapState(PVec &FPOps, ValMap &FPSttIndex);
    bool runOnX86FPStack(Function &F);
#endif

    bool run(Function &F);
    bool runOnLoop(Function &F);
    bool runOnFunction(Function &F);
};

char StateMappingPass::ID = 0;
static RegisterPass<StateMappingPass> StateMappingP("statemap", 
        "Eliminate redundant loads/stores by mapping CPUState", false, false);
FunctionPass *createStateMappingPass(IRFactory *IRF) 
{
    return new StateMappingPass(IRF);
}

/* 
 * LocalOp()
 *  Determine if the load or store instruction accesses the TCG local register.
 */
inline bool LocalOp(LoadInst *LI)
{
    if (strstr(getNameCString(LI->getPointerOperand()), "frame"))
        return true;
    return false;
}
inline bool LocalOp(StoreInst *SI)
{
    if (strstr(getNameCString(SI->getPointerOperand()), "frame"))
        return true;
    return false;
}

/*
 * LocalOp()
 *  Determine if the offset is to access the temporary state.
 */
inline bool LocalOp(size_t Off)
{
#if defined(TARGET_I386)
    if (Off == offsetof(CPUState, xmm_t0))
        return true;
#endif
    return false;
}

/* 
 * StripPointer()
 *  Get the pointer without GEP and BitCast. The stripped GEP and BitCast
 *  instructions are returned to the caller.
 */
void StateMappingPass::StripPointer(Value *V, IVec &IV)
{
    if (!V->getType()->isPointerTy())
        return;
    
    SmallPtrSet<Value*, 4> Visited;
    Visited.insert(V);
    do
    {
        if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V))
        {
            if (GEP->getParent() == InitBB)
                return;
            IV.push_back(GEP);
            V = cast<Operator>(V)->getOperand(0);
        }
        else if (BitCastInst *BCI = dyn_cast<BitCastInst>(V))
        {
            if (BCI->getParent() == InitBB)
                return;
            IV.push_back(BCI);
            V = cast<Operator>(V)->getOperand(0);
        }
        else
            return;
    } while (Visited.insert(V));
    
    return;
}

/* 
 * MovePointer
 *  Move the pointer before InsertPos.
 */
void StateMappingPass::MovePointer(Value *V, Instruction *InsertPos)
{
    IVec toMove;

    StripPointer(V, toMove);
    while (!toMove.empty())
    {
        Instruction *I = toMove.back();
        toMove.pop_back();
        I->moveBefore(InsertPos);
    }
}

/* 
 * RearrangePreload
 *  Move store/load/alloca instructions to the correct position.
 */
void StateMappingPass::RearrangePreload()
{
    IVec toMove;
    Instruction *InsertPos = InitBB->getTerminator();
    PreloadPos = new UnreachableInst(*Context, InsertPos);
    toErase.push_back(PreloadPos);
    
    for (BasicBlock::iterator I = InitBB->begin(), E = InitBB->end(); I != E;
            I++)
    {
        Operator *Op = dyn_cast<Operator>(I);
        if (Op->getOpcode() == Instruction::Load
                || Op->getOpcode() == Instruction::Store
                || Op->getOpcode() == Instruction::Alloca)
            toMove.push_back(I);
    }
    
    for (int i = 0, e = toMove.size(); i < e; i++)
        toMove[i]->moveBefore(InsertPos);
}

/*
 * State mapping for x86 floating point state  operations.
 */
#if defined(TARGET_I386)
#define X86FP_STACKSIZE  8
#define X86FP_STACKMASK  (X86FP_STACKSIZE - 1)
/*
 * getX86FPSttPtr()
 *  Given a FP load/store address, return the FP stack top pointer.
 */
Value *StateMappingPass::getX86FPSttPtr(Value *V)
{
    Operator *Op = dyn_cast<Operator>(V);
    if (Op->getOpcode() == Instruction::ZExt)
        return getX86FPSttPtr(Op->getOperand(0));
    else if (Op->getOpcode() == Instruction::And)
        return Op;
    else if (Op->getOpcode() == Instruction::Load)
        return Op;

    DM->Error("%s: invalid operator.\n", __func__);
    return NULL;
}

/*
 * setX86FPIndex()
 *  Set relative FP stack index.
 */
int StateMappingPass::setX86FPIndex(Value *V, ValMap &FPSttIndex, int &LastIndex)
{
    Operator *Op = dyn_cast<Operator>(V);
    if (Op == NULL)
        return 0;
    if (Op->getOpcode() == Instruction::And)
    {
        Op = dyn_cast<Operator>(Op->getOperand(0));
        ConstantInt *C = dyn_cast<ConstantInt>(Op->getOperand(1));
        if (C == NULL)
            return 0;

        LastIndex += C->getZExtValue();
        LastIndex &= X86FP_STACKMASK;
    }
    
    FPSttIndex[V] = LastIndex;
    return 1;
}

/*
 * getX86FPIndex()
 *  Return relative FP stack index.
 */
int StateMappingPass::getX86FPIndex(Value *V, ValMap &FPSttIndex)
{
    if (FPSttIndex.count(V))
        return FPSttIndex[V];

    Operator *Op = dyn_cast<Operator>(V);
#ifdef ASSERT
    if (Op->getOpcode() != Instruction::And)
        DM->Error("%s: invalid op (expect And)\n", __func__);
#endif

    Op = dyn_cast<Operator>(Op->getOperand(0));
    ConstantInt *C = dyn_cast<ConstantInt>(Op->getOperand(1));
    if (C == NULL)
        return -1;

    Op = dyn_cast<Operator>(Op->getOperand(0));
#ifdef ASSERT
    if (Op->getOpcode() != Instruction::Load)
        DM->Error("%s: invalid op (expect Load)\n", __func__);

    if (FPSttIndex.count(Op) == 0)
        DM->Error("%s: cannot find fp top\n", __func__);
#endif

    return (FPSttIndex[Op] + C->getZExtValue()) & X86FP_STACKMASK;
}

/*
 * X86FPMapState()
 *  Map states of X86 float point stack operations.
 */
bool StateMappingPass::X86FPMapState(PVec &FPOps, ValMap &FPSttIndex)
{
    bool Changed = false;
    StateMap StateCache;
    StateMap StoreCache;

    for (int i = 0, e = FPOps.size(); i < e; i++)
    {
        if (dyn_cast<CallInst>(FPOps[i].second))
        {
            StateCache.clear();
            StoreCache.clear();
            continue;
        }

        int Index = getX86FPIndex(FPOps[i].second, FPSttIndex);
        if (Index == -1)
            break;

        Value *FPOp = FPOps[i].first;
        if (LoadInst *LI = dyn_cast<LoadInst>(FPOp))
        {
            Value *LastState = 
                (StateCache.count(Index) == 0) ? NULL : StateCache[Index];
#ifdef ASSERT
            if (LastState && LI->getType() != LastState->getType())
                DM->Error("%s: inconsistant load type.\n", __func__);
#endif
            if (LastState == NULL || LI->isVolatile())
            {
                StateCache[Index] = LI;
                StoreCache.erase(Index);
            }
            else
            {
                LI->replaceAllUsesWith(LastState);
                toErase.push_back(LI);
            }
        }
        else if (StoreInst *SI = dyn_cast<StoreInst>(FPOp))
        {
#ifdef ASSERT
            if (StoreCache.count(Index) && 
                    SI->getOperand(0)->getType() != StateCache[Index]->getType())
                DM->Error("%s: inconsistant store type.\n", __func__);
#endif

            if (StoreCache.count(Index))
                toErase.push_back(static_cast<StoreInst*>(StoreCache[Index]));

            StateCache[Index] = SI->getOperand(0);
            if (SI->isVolatile())
                StoreCache.erase(Index);
            else
                StoreCache[Index] = SI;
        }
    }

    return Changed;
}

/*
 * runOnX86FPStack()
 *  Entry function of state mapping for x86 floating point stack operation.
 *  This state mapping is only applied within an LLVM block.
 */
bool StateMappingPass::runOnX86FPStack(Function &F)
{
    bool Changed = false;
    Value *FPStt = NULL;
    Value *FPSttPtr;
    size_t Off;
    Value *LastPtr;
    Value *Base;

    /* Get FP stack top pointer. */
    for (BasicBlock::iterator BI = F.begin()->begin(), BE = F.begin()->end();
            BI != BE; BI++)
    {
        if (LoadInst *LI = dyn_cast<LoadInst>(BI))
        {
            size_t Off = 0;
            Value *Base = getBaseWithConstantOffset(TD, LI->getPointerOperand(), Off);
            if (Base == BaseReg[BaseRegNo].Base && Off == X86FPSttOff)
            {
                StoreInst *SI = dyn_cast<StoreInst>(++BasicBlock::iterator(LI));
                if (SI == NULL)
                    DM->Error("%s: internal error.\n", __func__);
                FPStt = SI->getPointerOperand();
                break;
            }
        }
    }

    if (FPStt == NULL)
        return Changed;

    for (Function::iterator FI = F.begin(), FE = F.end(); FI != FE; FI++)
    {
        PVec FPRegs;
        PVec FPTags;
        int Index = 0;
        ValMap FPSttIndex;
        int VerIndex = 0;
        ValMap Version;

        /* Collect load/store instructions that access the FP states in
         * this BasicBlock. Call instructions are collected as well. */
        for (BasicBlock::iterator BI = FI->begin(), BE = FI->end();
                BI != BE; BI++)
        {
            Version[BI] = VerIndex;
            if (LoadInst *LI = dyn_cast<LoadInst>(BI))
            {
                Off = 0;
                Base = getBaseWithPossibleOffset(TD, LI->getPointerOperand(),
                        Off, &LastPtr);
                if (Base == FPStt)
                {
                    int ret = setX86FPIndex(LI, FPSttIndex, Index);
                    if (ret == 0)
                        break;

                    continue;
                }
                if (Base->getType() != BaseRegType)
                    continue;

                FPSttPtr = getX86FPSttPtr(LastPtr);
                if (Version.count(FPSttPtr) == 0 || Version[FPSttPtr] != VerIndex)
                    break;

                if (Off == X86FPRegOff)
                    FPRegs.push_back(make_pair(BI, FPSttPtr));
                else if (Off == X86FPTagOff)
                    FPTags.push_back(make_pair(BI, FPSttPtr));
                else
                    DM->Error("%s: internal error on load.\n", __func__);
            }
            else if (StoreInst *SI = dyn_cast<StoreInst>(BI))
            {
                Off = 0;
                Base = getBaseWithPossibleOffset(TD, SI->getPointerOperand(), Off, &LastPtr);
                if (Base == FPStt)
                {
                    int ret = setX86FPIndex(SI->getOperand(0), FPSttIndex, Index);
                    if (ret == 0)
                        break;

                    continue;
                }
                if (Base->getType() != BaseRegType)
                    continue;

                FPSttPtr = getX86FPSttPtr(LastPtr);
                if (Version.count(FPSttPtr) == 0 || Version[FPSttPtr] != VerIndex)
                    break;

                if (Off == X86FPRegOff)
                    FPRegs.push_back(make_pair(BI, FPSttPtr));
                else if (Off == X86FPTagOff)
                    FPTags.push_back(make_pair(BI, FPSttPtr));
                else
                    DM->Error("%s: internal error on store.\n", __func__);
            }
            else if (CallInst *CI = dyn_cast<CallInst>(BI))
            {
                MDNode *MD = CI->getMetadata("flags");
                if (MD || CI->isInlineAsm())
                    continue;

                string Fname = CI->getCalledFunction()->getName();
                if (HelperFuncs.count(Fname))
                {
                    HelperInfo *Helper = HelperFuncs[Fname];
                    if (Helper->hasFP == false)
                        continue;
                }

                VerIndex++;
                Index = 0;
                FPRegs.push_back(make_pair(BI, BI));
                FPTags.push_back(make_pair(BI, BI));
            }
        }

        /* Do state mapping in this BasicBlock. */
        if (FPRegs.size())
            Changed |= X86FPMapState(FPRegs, FPSttIndex);
        if (FPTags.size())
            Changed |= X86FPMapState(FPTags, FPSttIndex);
    }

    return Changed;
}
#endif

/*
 * CopyState()
 *  Copy memory data from src address to destination address.
 */
void StateMappingPass::CopyState(Value *Dest, Value *Src,
        Instruction *InsertPos)
{
    if (dyn_cast<AllocaInst>(Src) == NULL)
        MovePointer(Src, PreloadPos);

    if (dyn_cast<AllocaInst>(Dest) == NULL)
        MovePointer(Dest, PreloadPos);

    Value *V = new LoadInst(Src, "", false, InsertPos);
    new StoreInst(V, Dest, false, InsertPos);

#ifdef ASSERT
    if (static_cast<Instruction*>(V)->getParent() != InsertPos->getParent())
        DM->Error("%s: fatal error.\n", __func__);
#endif
}

/*
 * ConvertType()
 *  Cast no-op value to specified type.
 */
Value *StateMappingPass::ConvertType(Value *V, CONST Type *Ty,
        Instruction *InsertPos)
{
    if (V->getType() == Ty)
        return V;

    return new BitCastInst(V, Ty, "", InsertPos);
}

/*
 * LoadUnalignedState()
 *  Loading value from an unaligned state, or loading at an aligned state but
 *  with different sizes. For example: (1) two memory operations where one
 *  accesses %AL and the other one accesses %AH, or (2) one accesses %AL and
 *  the other one accesses $EAX.
 */
Value *StateMappingPass::LoadUnalignedState(Value *V, Value *SubV, size_t Diff,
        Instruction *InsertPos)
{
    CONST Type *Ty = V->getType();
    CONST Type *SubTy = SubV->getType();
    size_t Size = TD->getTypeSizeInBits(Ty);
    size_t SubSize = TD->getTypeSizeInBits(SubTy);

    if (Diff >= Size)
        DM->Error("%s: invalid offset %ld\n", __func__, Diff);

    /* If the state size is equal to the access size, just return the state. */
    if (Size == SubSize)
        return (Ty == SubTy) ? V : new BitCastInst(V, SubTy, "", InsertPos);

    /* First, casting it to integer type. */
    if (Ty->isIntegerTy() == false)
        V = new BitCastInst(V, Type::getIntNTy(*Context, Size), "", InsertPos);

#ifdef NEED_BSWAP
    Diff = Size - (Diff + SubSize);
#endif

    /* Discard the lower bits. */
    if (Diff != 0)
        V = BinaryOperator::Create(Instruction::LShr, V, CINT(Diff), "", InsertPos);

    /* Discard the higher bits. */
    V = new TruncInst(V, Type::getIntNTy(*Context, SubSize), "", InsertPos);

    /* Casting back to the original type. */
    if (SubTy->isIntegerTy() == false)
        V = new BitCastInst(V, SubTy, "", InsertPos);

    return V;
}

/*
 * StoreUnalignedState()
 *  Storing value from an unaligned state, or storing at an aligned state but
 *  with different sizes. For example: (1) two memory operations where one
 *  accesses %AL and the other one accesses %AH, or (2) one accesses %AL and
 *  the other one accesses $EAX.
 */
Value *StateMappingPass::StoreUnalignedState(Value *V, Value *SubV,
        size_t Diff, Instruction *InsertPos)
{
    Value *LowerPart = NULL;
    CONST Type *Ty = V->getType();
    CONST Type *SubTy = SubV->getType();
    size_t Size = TD->getTypeSizeInBits(Ty);
    size_t SubSize = TD->getTypeSizeInBits(SubTy);

    if (Diff >= Size)
        DM->Error("%s: invalid offset %ld\n", __func__, Diff);

    /* If the state size is equal to the access size, just return the state. */
    if (Size == SubSize)
        return (Ty == SubTy) ? SubV : new BitCastInst(SubV, Ty, "", InsertPos);

    /* First, casting it to integer type. */
    if (Ty->isIntegerTy() == false)
        V = new BitCastInst(V, Type::getIntNTy(*Context, Size), "", InsertPos);

    /* Also casting the access type to integer type. */
    if (SubTy->isIntegerTy() == false)
        SubV = new BitCastInst(SubV, Type::getIntNTy(*Context, SubSize), "", InsertPos);

#ifdef NEED_BSWAP
    Diff = Size - (Diff + SubSize);
#endif

    /* Extract the lower part if existed. */
    if (Diff != 0)
    {
        LowerPart = new TruncInst(V, Type::getIntNTy(*Context, Diff), "", InsertPos);
        LowerPart = new ZExtInst(LowerPart, V->getType(), "", InsertPos);
    }

    /* Extract the higher part. */
    SubV = new ZExtInst(SubV, V->getType(), "", InsertPos);
    if (Diff + SubSize == Size)
        V = SubV;
    else
    {
        V = BinaryOperator::Create(Instruction::LShr, V, CINT(Diff + SubSize), "", InsertPos);
        V = BinaryOperator::Create(Instruction::Shl, V, CINT(SubSize), "", InsertPos);
        V = BinaryOperator::Create(Instruction::Or, V, SubV, "", InsertPos);
    }

    if (Diff != 0)
    {
        V = BinaryOperator::Create(Instruction::Shl, V, CINT(Diff), "", InsertPos);
        V = BinaryOperator::Create(Instruction::Or, V, LowerPart, "", InsertPos);
    }

    /* Casting back to the original type. */
    if (Ty->isIntegerTy() == false)
        V = new BitCastInst(V, Ty, "", InsertPos);

    return V;
}

/* 
 * LoadVector()
 *  Load value from a vector element.
 */
Value *StateMappingPass::LoadVector(Value *V, Value *SubV, size_t Diff,
        Instruction *InsertPos)
{
    CONST Type *Ty = V->getType();
    CONST Type *SubTy = SubV->getType();
    CONST Type *ElementTy = static_cast<CONST VectorType *>(Ty)->getElementType();
    size_t Size = TD->getTypeSizeInBits(Ty);
    size_t SubSize = TD->getTypeSizeInBits(SubTy);

    if (Diff >= Size)
        DM->Error("%s: invalid offset %ld\n", __func__, Diff);
    if (Size % SubSize != 0 || Diff % SubSize != 0)
        DM->Error("%s: invalid element offset %ld size %d\n", __func__, Diff,
                (int)SubSize);

    if (Size == SubSize)
        return (Ty == SubTy) ? V : new BitCastInst(V, SubTy, "", InsertPos);

    if (ElementTy != SubTy)
        V = new BitCastInst(V, VectorType::get(SubTy, Size/SubSize), "", InsertPos);

    V = ExtractElementInst::Create(V, CONST32(Diff/SubSize), "", InsertPos);

    if (ElementTy != SubTy && SubTy->isVectorTy())
        V = new BitCastInst(V, SubTy, "", InsertPos);

    return V;
}

/*
 * StoreVector()
 *  Store value to a vector element.
 */
Value *StateMappingPass::StoreVector(Value *V, Value *SubV, size_t Diff,
        Instruction *InsertPos)
{
    CONST Type *Ty = V->getType();
    CONST Type *SubTy = SubV->getType();
    CONST Type *ElementTy = static_cast<CONST VectorType *>(Ty)->getElementType();
    size_t Size = TD->getTypeSizeInBits(Ty);
    size_t SubSize = TD->getTypeSizeInBits(SubTy);

    if (Diff >= Size)
        DM->Error("%s: invalid offset %ld\n", __func__, Diff);
    if (Size % SubSize != 0 || Diff % SubSize != 0)
        DM->Error("%s: invalid element offset %ld size %d\n", __func__, Diff,
                (int)SubSize);

    if (Size == SubSize)
        return (Ty == SubTy) ? SubV : new BitCastInst(SubV, Ty, "", InsertPos);

    if (ElementTy != SubTy)
        V = new BitCastInst(V, VectorType::get(SubTy, Size/SubSize), "", InsertPos);

    V = InsertElementInst::Create(V, SubV, CONST32(Diff/SubSize), "", InsertPos);

    if (ElementTy != SubTy)
        V = new BitCastInst(V, Ty, "", InsertPos);

    return V;
}

/*
 * MergeState()
 *   Merge the incoming states from the predecessors in the convergent block.
 */
void StateMappingPass::MergeState(BasicBlock *BB, StateMap &Merge)
{
    Merge.clear();

    vector<StateMap> &StateList = StateBuck[BB];
    if (StateList.size() == 0)
        return;

    /* If a state is dominated by all its predecessors, this state can be
       propagated to the successor. Otherwise, the state must be reset and
       written back to memory if it is dirty. */
    map<size_t,int> NumMerged;
    BBVec &PredList = PredBuck[BB];
    int NumPred = StateList.size();

#ifdef ASSERT
    if (NumPred != distance(pred_begin(BB), pred_end(BB)))
        DM->Error("%s: internal error.\n", __func__);
#endif

    /* Determine the number of predecessors and check if the state is dirty
       or not. */
    for (int i = 0; i < NumPred; i++)
    {
        StateMap &SM = StateList[i];
        for (StateMap::iterator I = SM.begin(), E = SM.end(); I != E; I++)
        {
            size_t Off = I->first;
            NumMerged[Off]++;
            if (Merge.count(Off) == 0)
                Merge[Off] = I->second;

            if (dyn_cast<StoreInst>(I->second))
                Merge[Off] = I->second;
        }
    }

    /* Move state to alloca if the predecessor does not come this state. */
    for (map<size_t,int>::iterator I = NumMerged.begin(), E = NumMerged.end();
            I != E; I++)
    {
        if (I->second == NumPred)
            continue;

        size_t Off = I->first;
        StateInfo &SInfo = State[Off];
        for (int i = 0; i < NumPred; i++)
        {
            StateMap &SM = StateList[i];
            if (SM.count(Off) == 0)
                CopyState(SInfo.AI, SInfo.Addr, PredList[i]->getTerminator());
        }
    }
}

/*
 * MapState()
 *  Propagate the states to the successor block.
 */
void StateMappingPass::MapState(BasicBlock *BB, StateMap &PredState)
{
    StateMap CurrState(PredState);

    /* Iterate over memory instructions in this basic block and replace them
       to alloca operations. */
    for (MemOpVec::iterator MI = OpMap[BB].begin(), ME = OpMap[BB].end(); MI != ME; MI++)
    {
        bool isLocal = false;
        size_t Off = MI->first;
        Instruction *Inst = MI->second;

        StateInfo &SInfo = State[Off];
        if (LoadInst *LI = dyn_cast<LoadInst>(Inst))
        {
            isLocal = LocalOp(LI);
            if (!isLocal && CurrState.count(Off) == 0)
            {
                /* First encounter this state, preload it. */
                CopyState(SInfo.AI, SInfo.Addr, LI);
                CurrState[Off] = LI;
            }

            Value *V = new LoadInst(SInfo.AI, "", false, LI);
            if (SInfo.isAligned == false)
            {
                size_t SubOff = 0;
                getBaseWithConstantOffset(TD, LI->getPointerOperand(), SubOff);
                V = LoadUnalignedState(V, LI, (SubOff-Off)*8, LI);
            }
            else if (SInfo.isSameTy == false)
                V = ConvertType(V, LI->getType(), LI);

            LI->replaceAllUsesWith(V);
            toErase.push_back(LI);
        }
        else if (StoreInst *SI = dyn_cast<StoreInst>(Inst))
        {
            isLocal = LocalOp(SI);
            Value *V = SI->getOperand(0);
            if (SInfo.isAligned == false)
            {
                if (CurrState.count(Off) == 0)
                    CopyState(SInfo.AI, SInfo.Addr, SI);

                size_t SubOff = 0;
                getBaseWithConstantOffset(TD, SI->getPointerOperand(), SubOff);
                Value *SuperV = new LoadInst(SInfo.AI, "", false, SI);
                V = StoreUnalignedState(SuperV, V, (SubOff-Off)*8, SI);
            }
            else if (SInfo.isSameTy == false)
                V = ConvertType(V, SInfo.Ty, SI);

            new StoreInst(V, SInfo.AI, false, SI);
            toErase.push_back(SI);

            if (!isLocal)
                CurrState[Off] = SI;
        }
        else if (CallInst *CI = dyn_cast<CallInst>(Inst))
        {
            StateSet DeadUse;
            StateSet DeadDef;
            string Fname = CI->getCalledFunction()->getName();

            HelperInfo *Helper = HelperFuncs.count(Fname) ? HelperFuncs[Fname] : NULL;
            if (Helper && Helper->hasNestedCall == false)
            {
                for (StateMap::iterator I = CurrState.begin(), E = CurrState.end(); I != E; I++)
                {
                    size_t RegOff = I->first;
                    if (Helper->StateDef.count(RegOff))
                        DeadUse.insert(RegOff);
                    if (dyn_cast<StoreInst>(I->second) && Helper->StateUse.count(RegOff))
                        DeadDef.insert(RegOff);
                }

                if (Helper->mayConflictArg == true)
                {
                    int NumArgs = CI->getNumArgOperands();
                    for (int i = 0; i < NumArgs; i++)
                    {
                        Value *Arg = CI->getArgOperand(i);
                        if (Arg->getType() == BaseRegType)
                            continue;

                        size_t RegOff = 0;
                        getBaseWithConstantOffset(TD, Arg, RegOff);
                        if (CurrState.count(RegOff))
                        {
                            DeadUse.insert(RegOff);
                            DeadDef.insert(RegOff);
                        }
                    }
                }
            }
            else
            {
                for (StateMap::iterator I = CurrState.begin(), E = CurrState.end(); I != E; I++)
                {
                    if (dyn_cast<StoreInst>(I->second))
                        DeadDef.insert(I->first);
                    
                    DeadUse.insert(I->first);
                }
            }
            
            /* Write back. */
            for (StateSet::iterator I = DeadDef.begin(), E = DeadDef.end(); I != E; I++)
            {
                StateInfo &SInfo = State[*I];
                CopyState(SInfo.Addr, SInfo.AI, CI);
                CurrState.erase(*I);
            }

            for (StateSet::iterator I = DeadUse.begin(), E = DeadUse.end(); I != E; I++)
                CurrState.erase(*I);
        }
    }

    /* Store the dirty states to CPUState at the exit blocks. */
    int NumSuccs = distance(succ_begin(BB), succ_end(BB));
    if (NumSuccs == 0)
    {
        ProcessExitBB(BB, CurrState);
        return;
    }

    /* Non-leaf basic block. */
    for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; SI++)
    {
        BasicBlock *NextBB = *SI;
        if (AcyclicBB.count(NextBB) != 0)
        {
            /* Basic block that has multiple predecessors. Copy state cache and
               store cache to this basic block and add it to work list if all
               state/store caches are collected. */
            StateBuck[NextBB].push_back(CurrState);
            PredBuck[NextBB].push_back(BB);

            AcyclicBB[NextBB]--;
            if (AcyclicBB[NextBB] == 0)
                Worklist.push_back(NextBB);
            continue;
        }

        MapState(NextBB, CurrState);
    }
}

/*
 * ProcessExitBB()
 *  Store dirty states at the leaf blocks.
 */
void StateMappingPass::ProcessExitBB(BasicBlock *BB, StateSet &StateDef)
{
#ifdef ASSERT
    if (BB == InitBB)
        DM->Error("%s: cannot be entry block\n", __func__);
#endif

    if (ExitBB.count(BB) != 0)
        return;

    /* If we cannot find the insertion point, this may happen because
       this block is changed due to inlining helper function. */
    Instruction *InsertPos = IF->getCodeInfo()->getInsertPos(BB);
    if (InsertPos == NULL)
    {
        for (BasicBlock::iterator BI = BB->begin(), BE = BB->end();
                BI != BE; BI++)
        {
            if ((BI->hasMetadata() && BI->getMetadata("flags") == NULL) ||
                    (BI->hasMetadata() && BI->getMetadata("flags") && BI->getMetadata("exit")) ||
                    isa<IndirectBrInst>(BI))
            {
                InsertPos = BI;
                break;
            }
        }
    }
    
    /* If we still cannot find a insertion point, there are two possible
       conditions. One is that this is a exception handling block that
       would jump back to dispatcher. The other one is that it is the block
       connecting the non-loop block and the loop dominating blocks. */
    if (InsertPos == NULL)
        InsertPos = BB->getTerminator();
    
    for (StateSet::iterator SI = StateDef.begin(), SE = StateDef.end();
            SI != SE; SI++)
    {
        StateInfo &SInfo = State[*SI];
        if (LocalOp(SInfo.Off) == true)
            continue;
        CopyState(SInfo.Addr, SInfo.AI, InsertPos);
    }

    ExitBB.insert(BB);
}

/*
 * ProcessExitBB()
 *  Store dirty states at the leaf blocks.
 */
void StateMappingPass::ProcessExitBB(BasicBlock *BB, StateMap &SM)
{
    StateSet StateDef;
    for (StateMap::iterator I = SM.begin(), E = SM.end(); I != E; I++)
    {
        if (dyn_cast<StoreInst>(I->second))
            StateDef.insert(I->first);
    }
    if (StateDef.size() == 0)
        return;

    ProcessExitBB(BB, StateDef);
}

/*
 * AnalyzeStates()
 *  Scan all instruction in a Function and analyze the instructions that access
 *  the CPUState.
 */
void StateMappingPass::AnalyzeStates(Function &F)
{
    StateTypeMap &StateType = IF->Translator->getStateType();

    RearrangePreload();

    for (Function::iterator FI = F.begin(), FE = F.end(); FI != FE; FI++)
    {
        /* 
         * Collect all CPU state load/store operations and also search for 
         * call instruction because callee may ruin the CPU state mapping. 
         *
         * Aligned cpu state represents same start offset and data size. The
         * types can be different. If the start offsets or data sizes are
         * different or data sizes are not the same, it is unaligned.
         */
        BasicBlock *BB = FI;
        for (BasicBlock::iterator BI = FI->begin(), BE = FI->end();
                BI != BE; BI++)
        {
            if (LoadInst *LI = dyn_cast<LoadInst>(BI))
            {
                size_t Off = 0;
                Value *Base = getBaseWithConstantOffset(TD, LI->getPointerOperand(), Off);
                if (Base->getType() == BaseRegType || Base == BaseReg[BaseRegNo].Base)
                {
                    size_t Size = TD->getTypeSizeInBits(LI->getType())/8;
                    StateTypeMap::iterator TI = --StateType.upper_bound(Off);
                    size_t StateStart = TI->first;
                    size_t StateEnd = StateStart + TI->second.second;
                    if (Off + Size > StateEnd)
                        DM->Error("%s: fatal error on load state %d:%d (%d:%d).\n", __func__,
                                Off, Size, StateStart, StateEnd);

                    State[StateStart].setState(TD, Off, LI, LI->getPointerOperand());
                    OpVec.push_back(make_pair(StateStart, LI));
                    OpMap[BB].push_back(make_pair(StateStart, LI));
                }
            }
            else if (StoreInst *SI = dyn_cast<StoreInst>(BI))
            {
                size_t Off = 0;
                Value *Base = getBaseWithConstantOffset(TD, SI->getPointerOperand(), Off);
                if (Base->getType() == BaseRegType || Base == BaseReg[BaseRegNo].Base)
                {
                    if (isStoreEIP(Off) == true)
                        continue;

                    size_t Size = TD->getTypeSizeInBits(SI->getOperand(0)->getType())/8;
                    StateTypeMap::iterator TI = --StateType.upper_bound(Off);
                    size_t StateStart = TI->first;
                    size_t StateEnd = StateStart + TI->second.second;
                    if (Off + Size > StateEnd)
                        DM->Error("%s: fatal error on store state %d:%d (%d:%d).\n", __func__,
                                Off, Size, StateStart, StateEnd);

                    State[StateStart].setState(TD, Off, SI->getOperand(0), SI->getPointerOperand());
                    OpVec.push_back(make_pair(StateStart, SI));
                    OpMap[BB].push_back(make_pair(StateStart, SI));
                }
            }
            else if (CallInst *CI = dyn_cast<CallInst>(BI))
            {
                MDNode *MD = CI->getMetadata("flags");
                if (MD) continue;   /* skip const helper functions. */

                /* Map base regs' inline asm that is generated by llvm-gcc to
                   our defined inline asm. */
                if (CI->isInlineAsm())
                {
                    if (CI != CPUStruct && CI->getCalledValue() == CPUStructAsm)
                    {
                        CI->replaceAllUsesWith(CPUStruct);
                        toErase.push_back(CI);
                    }
                    continue;
                }

                Function *Fn = CI->getCalledFunction();
                if (Fn->isIntrinsic())
                    continue;

                string Fname = getNameString(Fn);
                if (Pass == 0 && HelperFuncs.count(Fname))
                {
                    HelperInfo *Helper = HelperFuncs[Fname];
                    if (Helper->FuncNoInline == Fn && Helper->mayConflict == false)
                        IF->addNoInlineCall(CI);
                }

                Calls.push_back(CI);
                OpMap[BB].push_back(make_pair(-1U, CI));
            }
        }
    }

    /* Prepare alloca for each state. */
    int NumAlloca = 0;
    for (StateInfoMap::iterator I = State.begin(), E = State.end(); I != E; I++)
    {
        StateInfo &SInfo = I->second;

        if (SInfo.Name == "")
        {
            char Name[32] = {'\0'};
            sprintf(Name, "state%d.a", NumAlloca++);
            SInfo.Name = Name;
        }
        
        if (SInfo.isAligned == true)
            SInfo.AI = new AllocaInst(SInfo.Ty, SInfo.Name, PreloadPos);
        else
        {
            /* If the state is unaligned, we allocate a big alloca that is the 
               superset of the collected cpu states */
            StateTypeMap::iterator TI = --StateType.upper_bound(SInfo.Off);
            SInfo.Off = TI->first;
            SInfo.Ty = TI->second.first;
            SInfo.isVector = SInfo.Ty->isVectorTy();
            GetElementPtrInst *GEP = GetElementPtrInst::Create(CPU, 
                    ConstantInt::get(Int32Ty, SInfo.Off), "", PreloadPos);
            SInfo.Addr = new BitCastInst(GEP, PointerType::getUnqual(SInfo.Ty),
                    "", PreloadPos);
            SInfo.AI = new AllocaInst(SInfo.Ty, "", PreloadPos);
        }
    }
}

void StateMappingPass::SyncDef(StateSet &DeadDef, Instruction *InsertPos)
{
    /* Write dead Def back to memory. */
    for (StateSet::iterator I = DeadDef.begin(), E = DeadDef.end(); I != E; I++)
    {
        StateInfo &SInfo = State[*I];
        CopyState(SInfo.Addr, SInfo.AI, InsertPos);
    }
}

void StateMappingPass::SyncUse(StateSet &DeadUse, Instruction *InsertPos)
{
    if (dyn_cast<UnreachableInst>(InsertPos) != NULL) /* tail call */
        return;

    /* Reload dead Use from memory. */
    for (StateSet::iterator I = DeadUse.begin(), E = DeadUse.end(); I != E; I++)
    {
        StateInfo &SInfo = State[*I];
        CopyState(SInfo.AI, SInfo.Addr, InsertPos);
    }
}

bool StateMappingPass::SyncHelperState(StateSet &StateUse, StateSet &StateDef)
{
    bool hasUnknownHelper = false;
    StateSet MaskedUse;
    StateSet MaskedDef;

    MaskedUse.insert(StateUse.begin(), StateUse.end());
    MaskedDef.insert(StateDef.begin(), StateDef.end());

    for (int i = 0, e = Calls.size(); i < e; i++)
    {
        CallInst *CI = static_cast<CallInst *>(Calls[i]);
        string Fname = CI->getCalledFunction()->getName();
        HelperInfo *Helper = HelperFuncs.count(Fname) ? HelperFuncs[Fname] : NULL;
        if (Helper == NULL || Helper->hasNestedCall)
        {
            hasUnknownHelper = true;
            break;
        }

        for (StateSet::iterator I = Helper->StateDef.begin(), E = Helper->StateDef.end();
                I != E; I++)
        {
            size_t Off = *I;
            if (MaskedDef.count(Off))
                MaskedUse.insert(Off);
        }
    }

    if (hasUnknownHelper)
        MaskedUse.insert(StateDef.begin(), StateDef.end());

    for (int i = 0, e = Calls.size(); i < e; i++)
    {
        StateSet DeadUse;
        StateSet DeadDef;

        CallInst *CI = static_cast<CallInst *>(Calls[i]);
        string Fname = CI->getCalledFunction()->getName();
        HelperInfo *Helper = HelperFuncs.count(Fname) ? HelperFuncs[Fname] : NULL;
        if (Helper == NULL || Helper->hasNestedCall)
        {
            DeadDef.insert(MaskedDef.begin(), MaskedDef.end());
            DeadUse.insert(MaskedUse.begin(), MaskedUse.end());
        }
        else
        {
            for (StateSet::iterator I = Helper->StateUse.begin(), E = Helper->StateUse.end();
                    I != E; I++)
            {
                size_t Off = *I;
                if (MaskedDef.count(Off))
                    DeadDef.insert(Off);
            }
            for (StateSet::iterator I = Helper->StateDef.begin(), E = Helper->StateDef.end();
                    I != E; I++)
            {
                size_t Off = *I;
                if (MaskedUse.count(Off))
                    DeadUse.insert(Off);
            }

            if (Helper->mayConflictArg)
            {
                int NumArgs = CI->getNumArgOperands();
                for (int j = 0; j < NumArgs; j++)
                {
                    Value *Arg = CI->getArgOperand(j);
                    if (Arg->getType() == BaseRegType)
                        continue;

                    size_t Off = 0;
                    getBaseWithConstantOffset(TD, Arg, Off);
                    if (MaskedUse.count(Off))
                        DeadUse.insert(Off);
                    if (MaskedDef.count(Off))
                    {
                        DeadUse.insert(Off);
                        DeadDef.insert(Off);
                    }
                }
            }
        }

        SyncDef(DeadDef, CI);
        SyncUse(DeadUse, ++BasicBlock::iterator(CI));
    }

    return false;
}

/*
 * runOnLoop()
 *  Trace level state mapping. This function only map states within loops
 *  and all dominated successors of the loops.
 */
bool StateMappingPass::runOnLoop(Function &F)
{
    StateSet StateUse;
    StateSet StateDef;
    StateSet Preload;

    /* Iterate over all memory operations and replace them to accessing allocas. */
    for (MemOpVec::iterator MI = OpVec.begin(), ME = OpVec.end(); MI != ME; MI++)
    {
        bool isLocal = false;
        size_t Off = MI->first;
        Instruction *Inst = MI->second;

        StateInfo &SInfo = State[Off];
        if (LoadInst *LI = dyn_cast<LoadInst>(Inst))
        {
            isLocal = LocalOp(LI);
            if (!isLocal && StateUse.count(Off) == 0)
                StateUse.insert(Off);

            Value *V = new LoadInst(SInfo.AI, "", false, LI);
            if (SInfo.isAligned == false)
            {
                size_t SubOff = 0;
                getBaseWithConstantOffset(TD, LI->getPointerOperand(), SubOff);
                V = LoadUnalignedState(V, LI, (SubOff-Off)*8, LI);
            }
            else if (SInfo.isSameTy == false)
                V = ConvertType(V, LI->getType(), LI);

            LI->replaceAllUsesWith(V);
            toErase.push_back(LI);
        }
        else if (StoreInst *SI = dyn_cast<StoreInst>(Inst))
        {
            isLocal = LocalOp(SI);
            if (!isLocal && StateDef.count(Off) == 0)
                StateDef.insert(Off);

            Value *V = SI->getOperand(0);
            if (SInfo.isAligned == false)
            {
                size_t SubOff = 0;
                getBaseWithConstantOffset(TD, SI->getPointerOperand(), SubOff);
                Value *SuperV = new LoadInst(SInfo.AI, "", false, SI);
                V = StoreUnalignedState(SuperV, V, (SubOff-Off)*8, SI);
            }
            else if (SInfo.isSameTy == false)
                V = ConvertType(V, SInfo.Ty, SI);

            new StoreInst(V, SInfo.AI, false, SI);
            toErase.push_back(SI);
        }

        if (!isLocal && Preload.count(Off) == 0)
            Preload.insert(Off);
    }

    /* Preload state from CPUState to alloca. */
    for (StateSet::iterator I = Preload.begin(), E = Preload.end(); I != E; I++)
    {
        StateInfo &SInfo = State[*I];
        CopyState(SInfo.AI, SInfo.Addr, PreloadPos);
    }

    if (StateUse.size() == 0 && StateDef.size() == 0)
        return false;

    if (!Calls.empty())
        SyncHelperState(StateUse, StateDef);

    /* Post-store values back to CPU states for each exit block. */
    Function::BasicBlockListType &BBList = F.getBasicBlockList();
    for (Function::BasicBlockListType::iterator I = BBList.begin(), E = BBList.end();
            I != E; I++)
    {
        /* If a tail call that leaves code cache inside this call exists,
         * skip the post-stores.*/
        Instruction *LastInst = I->getTerminator();
        if ((dyn_cast<UnreachableInst>(LastInst) || dyn_cast<ReturnInst>(LastInst)) &&
                dyn_cast<CallInst>(--BasicBlock::iterator(LastInst)))
        {
            CallInst *CI = dyn_cast<CallInst>(--BasicBlock::iterator(LastInst));
            if (CI->getCalledFunction()->getName() != "helper_lookup_ibchain")
                continue;
        }

        if (distance(succ_begin(I), succ_end(I)) == 0)    /* leaf node */
            ProcessExitBB(I, StateDef);
    }

    return true;
}

bool StateMappingPass::run(Function &F)
{
    bool Changed = false;

    if (Trace && Trace->LoopHeader.size())
        return runOnLoop(F);

    /* Locate blocks that have multiple predecessors. */
    BasicBlock *EntryBB = (F.size() == 1) ? F.begin() : ++F.begin();
    for (Function::iterator I = EntryBB, E = F.end(); I != E; I++)
    {
        int NumPreds = distance(pred_begin(I), pred_end(I));
        if (NumPreds < 2)
            continue;

        /* Basic block that has multiple predecessors. */
        AcyclicBB[I] = NumPreds;
    }

    Worklist.push_back(EntryBB);
    while (!Worklist.empty())
    {
        BasicBlock *BB = Worklist.back();
        Worklist.pop_back();
        
        StateMap InitState;
        MergeState(BB, InitState);
        MapState(BB, InitState);
    }

#ifdef ASSERT
    for (BBMap::iterator I = AcyclicBB.begin(), E = AcyclicBB.end(); I != E; I++)
    {
        if (I->second != 0)
            DM->Error("%s: error on acyclic BB %d\n", __func__, I->second);
    }
#endif

    return Changed;
}

/*
 * runOnFunction()
 *  Entry point of CPU state mapping pass.
 */
bool StateMappingPass::runOnFunction(Function &F)
{
    bool Changed = false;

    ExitBB.clear();
    AcyclicBB.clear();
    Worklist.clear();
    StateBuck.clear();
    PredBuck.clear();
    State.clear();
    OpVec.clear();
    OpMap.clear();
    Calls.clear();

    Pass = IF->getPass();
    CPU = IF->CPU;
    CPUStruct = IF->CPUStruct;
    CPUStructAsm = IF->CPUStructAsm;
    BaseReg = IF->getBaseReg();
    Trace = IF->TBuilder->getTrace();

    InitBB = &F.getEntryBlock();

    AnalyzeStates(F);
    Changed |= run(F);

    ProcessErase(toErase);

#if defined(TARGET_I386)
    if (Pass == 0)
    {
        Changed |= runOnX86FPStack(F);
        ProcessErase(toErase);
    }
#endif

    return Changed;
}

/*
 * HoistCommonCode Pass
 *  This pass is to Hoist any common code in the two blocks
 *  up into the branch block.
 *  
 *  (This pass is copied from Transforms/Utils/SimplifyCFG.cpp in the
 *  llvm-3.0 source tree)
 */
class HoistCommonCodePass : public FunctionPass
{
private:

public:
    static char ID;
    explicit HoistCommonCodePass() : FunctionPass(ID) {}

    bool HoistCommonCode(BranchInst *BI);
    bool runOnFunction(Function &F);
};

char HoistCommonCodePass::ID = 0;
static RegisterPass<HoistCommonCodePass> HoistCommonCodePassP("hoistcc", 
        "Hoist common code up into the parent block", false, false);
FunctionPass *createHoistCommonCodePass() 
{
    return new HoistCommonCodePass();
}

/*
 * isSafeToHoistInvoke()
 *  If we would need to insert a select that uses the value of this invoke,
 *  we can't hoist the invoke, as there is nowhere to put the select in this case.
 */
static bool isSafeToHoistInvoke(BasicBlock *BB1, BasicBlock *BB2,
        Instruction *I1, Instruction *I2)
{
    for (succ_iterator SI = succ_begin(BB1), E = succ_end(BB1); SI != E; ++SI)
    {
        PHINode *PN;
        for (BasicBlock::iterator BBI = SI->begin();
                (PN = dyn_cast<PHINode>(BBI)); ++BBI)
        {
            Value *BB1V = PN->getIncomingValueForBlock(BB1);
            Value *BB2V = PN->getIncomingValueForBlock(BB2);
            if (BB1V != BB2V && (BB1V==I1 || BB2V==I2))
                return false;
        }
    }
    return true;
}

/* 
 * HoistCommonCode()
 *  Given a conditional branch that goes to BB1 and BB2, hoist any common code
 *  in the two blocks up into the branch block. The caller of this function must
 *  guarantee that BI's block dominates BB1 and BB2.
 */
bool HoistCommonCodePass::HoistCommonCode(BranchInst *BI)
{
    BasicBlock *BB1 = BI->getSuccessor(0);
    BasicBlock *BB2 = BI->getSuccessor(1);

    BasicBlock::iterator BB1_Itr = BB1->begin();
    BasicBlock::iterator BB2_Itr = BB2->begin();

    Instruction *I1 = BB1_Itr++, *I2 = BB2_Itr++;

    /* Skip debug info if it is not identical. */
    DbgInfoIntrinsic *DBI1 = dyn_cast<DbgInfoIntrinsic>(I1);
    DbgInfoIntrinsic *DBI2 = dyn_cast<DbgInfoIntrinsic>(I2);
    if (!DBI1 || !DBI2 || !DBI1->isIdenticalToWhenDefined(DBI2)) {
        while (isa<DbgInfoIntrinsic>(I1))
            I1 = BB1_Itr++;
        while (isa<DbgInfoIntrinsic>(I2))
            I2 = BB2_Itr++;
    }

    if (isa<PHINode>(I1) || !I1->isIdenticalToWhenDefined(I2) ||
            (isa<InvokeInst>(I1) && !isSafeToHoistInvoke(BB1, BB2, I1, I2)))
        return false;

    /* If we get here, we can hoist at least one instruction. */
    BasicBlock *BIParent = BI->getParent();
    do {
        /* Don't hoist the terminator instruction. */
        if (isa<TerminatorInst>(I1) || isa<IntrinsicInst>(I1) || I1->hasMetadata())
            break;
        
        /* For a normal instruction, we just move one to right before the branch,
         * then replace all uses of the other with the first. Finally, we remove
         * the now redundant second instruction. */
        BIParent->getInstList().splice(BI, BB1->getInstList(), I1);
        if (!I2->use_empty())
            I2->replaceAllUsesWith(I1);
        
        I1->intersectOptionalDataWith(I2);
        I2->eraseFromParent();

        I1 = BB1_Itr++;
        I2 = BB2_Itr++;

        /* Skip debug info if it is not identical. */
        DbgInfoIntrinsic *DBI1 = dyn_cast<DbgInfoIntrinsic>(I1);
        DbgInfoIntrinsic *DBI2 = dyn_cast<DbgInfoIntrinsic>(I2);
        if (!DBI1 || !DBI2 || !DBI1->isIdenticalToWhenDefined(DBI2))
        {
            while (isa<DbgInfoIntrinsic>(I1))
                I1 = BB1_Itr++;
            while (isa<DbgInfoIntrinsic>(I2))
                I2 = BB2_Itr++;
        }
    } while (I1->isIdenticalToWhenDefined(I2));

    return true;
}

bool HoistCommonCodePass::runOnFunction(Function &F)
{
    bool Changed = false;

    Function::BasicBlockListType &BBList = F.getBasicBlockList();
    for (Function::BasicBlockListType::iterator I = BBList.begin(), E = BBList.end();
            I != E; I++)
    {
        BranchInst *BI = dyn_cast<BranchInst>(I->getTerminator());
        if (BI == NULL || BI->isUnconditional() || BI->getNumSuccessors() != 2)
            continue;
        
        if (BI->getSuccessor(0)->getSinglePredecessor() != 0 &&
                BI->getSuccessor(1)->getSinglePredecessor() != 0)
        {
            Changed |= HoistCommonCode(BI);
        }
    }

    return Changed;
}

/*
 * Simplify the specified two-entry PHI node to Select instruction.
 */
class PHItoSelectPass : public FunctionPass
{
private:

public:
    static char ID;
    explicit PHItoSelectPass() : FunctionPass(ID) {}

    bool FoldTwoEntryPHINode(PHINode *PN);
    bool runOnFunction(Function &F);
};

char PHItoSelectPass::ID = 0;
static RegisterPass<PHItoSelectPass> PHItoSelectPassP("phitoselect", 
        "Fold two-entry PHI node to Select instruction", false, false);
FunctionPass *createPHItoSelectPass() 
{
    return new PHItoSelectPass();
}

/*
 * DominatesMergePoint()
 *  If we have a merge point of an "if condition" as accepted above, return
 *  true if the specified value dominates the block.  We don't handle the
 *  true generality of domination here, just a special case which works well
 *  enough for us.
 *  If AggressiveInsts is non-null, and if V does not dominate BB, we check to
 *  see if V (which must be an instruction) is cheap to compute and is
 *  non-trapping.  If both are true, the instruction is inserted into the set
 *  and true is returned.
 */
static bool DominatesMergePoint(Value *V, BasicBlock *BB, set<Instruction*> *AggressiveInsts)
{
    Instruction *I = dyn_cast<Instruction>(V);
    if (!I)
    {
    /* Non-instructions all dominate instructions, but not all constantexprs
     * can be executed unconditionally.*/
        if (ConstantExpr *C = dyn_cast<ConstantExpr>(V))
            if (C->canTrap())
                return false;
        return true;
    }
    BasicBlock *PBB = I->getParent();

    /* We don't want to allow weird loops that might have the "if condition" in
     * the bottom of this block. */
    if (PBB == BB)
        return false;

    /* If this instruction is defined in a block that contains an unconditional
     * branch to BB, then it must be in the 'conditional' part of the "if
     * statement". */
    if (BranchInst *BI = dyn_cast<BranchInst>(PBB->getTerminator()))
    {
        if (BI->isUnconditional() && BI->getSuccessor(0) == BB)
        {
            if (!AggressiveInsts)
                return false;
            /* Okay, it looks like the instruction IS in the "condition".  Check to
             * see if it's a cheap instruction to unconditionally compute, and if it
             * only uses stuff defined outside of the condition.  If so, hoist it out. */
#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30)
            if (!I->isSafeToSpeculativelyExecute())
#else
            if (!isSafeToSpeculativelyExecute(I))
#endif
                return false;

            switch (I->getOpcode())
            {
                default: return false;  /* Cannot hoist this out safely. */
                case Instruction::Load:
                {
                /* We have to check to make sure there are no instructions before the
                 * load in its basic block, as we are going to hoist the loop out to
                 * its predecessor. */
                    BasicBlock::iterator IP = PBB->begin();
                    while (isa<DbgInfoIntrinsic>(IP))
                        IP++;
                    if (IP != BasicBlock::iterator(I))
                        return false;
                    break;
                }
                case Instruction::Add:
                case Instruction::Sub:
                case Instruction::And:
                case Instruction::Or:
                case Instruction::Xor:
                case Instruction::Shl:
                case Instruction::LShr:
                case Instruction::AShr:
                case Instruction::ICmp:
                break;   /* These are all cheap and non-trapping instructions. */
            }

            /* Okay, we can only really hoist these out if their operands are not
             * defined in the conditional region. */
            for (User::op_iterator i = I->op_begin(), e = I->op_end(); i != e; ++i)
            {
                if (!DominatesMergePoint(*i, BB, 0))
                    return false;
            }
            /* Okay, it's safe to do this!  Remember this instruction. */
            AggressiveInsts->insert(I);
        }
    }

    return true;
}

/* 
 * GetIfCondition()
 *  Given a basic block (BB) with two predecessors (and presumably PHI nodes in it),
 *  check to see if the merge at this block is due to an "if condition". If so,
 *  return the boolean condition that determines which entry into BB will be taken.
 *  Also, return by references the block that will be entered from if the condition
 *  is true, and the block that will be entered if the condition is false.
 */
static Value *GetIfCondition(BasicBlock *BB, BasicBlock *&IfTrue, BasicBlock *&IfFalse)
{
    BasicBlock *Pred1 = *pred_begin(BB);
    BasicBlock *Pred2 = *++pred_begin(BB);

    /* We can only handle branches.  Other control flow will be lowered to
     * branches if possible anyway. */
    if (!isa<BranchInst>(Pred1->getTerminator()) ||
            !isa<BranchInst>(Pred2->getTerminator()))
        return NULL;

    BranchInst *Pred1Br = cast<BranchInst>(Pred1->getTerminator());
    BranchInst *Pred2Br = cast<BranchInst>(Pred2->getTerminator());

    /* Eliminate code duplication by ensuring that Pred1Br is conditional if
     * either are. */
    if (Pred2Br->isConditional())
    {
        /* If both branches are conditional, we don't have an "if statement".  In
         * reality, we could transform this case, but since the condition will be
         * required anyway, we stand no chance of eliminating it, so the xform is
         * probably not profitable. */
        if (Pred1Br->isConditional())
            return NULL;

        std::swap(Pred1, Pred2);
        std::swap(Pred1Br, Pred2Br);
    }

    if (Pred1Br->isConditional())
    {
        /* If we found a conditional branch predecessor, make sure that it branches
         * to BB and Pred2Br.  If it doesn't, this isn't an "if statement". */
        if (Pred1Br->getSuccessor(0) == BB && Pred1Br->getSuccessor(1) == Pred2)
        {
            IfTrue = Pred1;
            IfFalse = Pred2;
        }
        else if (Pred1Br->getSuccessor(0) == Pred2 && Pred1Br->getSuccessor(1) == BB)
        {
            IfTrue = Pred2;
            IfFalse = Pred1;
        }
        else
        {
            /* We know that one arm of the conditional goes to BB, so the other must
             * go somewhere unrelated, and this must not be an "if statement". */
            return NULL;
        }

        /* The only thing we have to watch out for here is to make sure that Pred2
         * doesn't have incoming edges from other blocks.  If it does, the condition
         * doesn't dominate BB. */
        if (++pred_begin(Pred2) != pred_end(Pred2))
            return NULL;

        return Pred1Br->getCondition();
    }

    /* Ok, if we got here, both predecessors end with an unconditional branch to
     * BB.  Don't panic!  If both blocks only have a single (identical)
     * predecessor, and THAT is a conditional branch, then we're all ok! */
    if (pred_begin(Pred1) == pred_end(Pred1) ||
            ++pred_begin(Pred1) != pred_end(Pred1) ||
            pred_begin(Pred2) == pred_end(Pred2) ||
            ++pred_begin(Pred2) != pred_end(Pred2) ||
            *pred_begin(Pred1) != *pred_begin(Pred2))
        return NULL;

    /* Otherwise, if this is a conditional branch, then we can use it! */
    BasicBlock *CommonPred = *pred_begin(Pred1);
    if (BranchInst *BI = dyn_cast<BranchInst>(CommonPred->getTerminator()))
    {
        if (BI->getSuccessor(0) == Pred1)
        {
            IfTrue = Pred1;
            IfFalse = Pred2;
        }
        else
        {
            IfTrue = Pred2;
            IfFalse = Pred1;
        }
        return BI->getCondition();
    }
    return NULL;
}

/*
 * FoldTwoEntryPHINode()
 *  Given a BB that starts with the specified two-entry PHI node, see if we can
 *  eliminate it.
 */
bool PHItoSelectPass::FoldTwoEntryPHINode(PHINode *PN)
{
    BasicBlock *BB = PN->getParent();
    BasicBlock *IfTrue, *IfFalse;
    Value *IfCond = GetIfCondition(BB, IfTrue, IfFalse);
    if (!IfCond)
        return false;


    /* Okay, we found that we can merge this two-entry phi node into a select.
     * Doing so would require us to fold *all* two entry phi nodes in this block.
     * At some point this becomes non-profitable (particularly if the target
     * doesn't support cmov's).  Only do this transformation if there are two or
     * fewer PHI nodes in this block. */
    unsigned NumPhis = 0;
    for (BasicBlock::iterator I = BB->begin(); isa<PHINode>(I); ++NumPhis, ++I)
    {
        if (NumPhis > 2)
            return false;
    }

    set<Instruction*> AggressiveInsts;

    BasicBlock::iterator AfterPHIIt = BB->begin();
    while (isa<PHINode>(AfterPHIIt))
    {
        PHINode *PN = cast<PHINode>(AfterPHIIt++);
        if (PN->getIncomingValue(0) == PN->getIncomingValue(1))
        {
            if (PN->getIncomingValue(0) != PN)
                PN->replaceAllUsesWith(PN->getIncomingValue(0));
            else
                PN->replaceAllUsesWith(UndefValue::get(PN->getType()));
        }
        else if (!DominatesMergePoint(PN->getIncomingValue(0), BB, &AggressiveInsts) ||
                !DominatesMergePoint(PN->getIncomingValue(1), BB, &AggressiveInsts))
        {
            return false;
        }
    }

    /* If we all PHI nodes are promotable, check to make sure that all
     * instructions in the predecessor blocks can be promoted as well.  If
     * not, we won't be able to get rid of the control flow, so it's not
     * worth promoting to select instructions. */
    BasicBlock *DomBlock = 0, *IfBlock1 = 0, *IfBlock2 = 0;
    PN = cast<PHINode>(BB->begin());
    BasicBlock *Pred = PN->getIncomingBlock(0);
    if (cast<BranchInst>(Pred->getTerminator())->isUnconditional())
    {
        IfBlock1 = Pred;
        DomBlock = *pred_begin(Pred);
        for (BasicBlock::iterator I = Pred->begin(); !isa<TerminatorInst>(I); ++I)
        {
            if (!AggressiveInsts.count(I) && !isa<DbgInfoIntrinsic>(I))
            {
                /* This is not an aggressive instruction that we can promote.
                 * Because of this, we won't be able to get rid of the control
                 * flow, so the xform is not worth it. */
                return false;
            }
        }
    }
    
    Pred = PN->getIncomingBlock(1);
    if (cast<BranchInst>(Pred->getTerminator())->isUnconditional())
    {
        IfBlock2 = Pred;
        DomBlock = *pred_begin(Pred);
        for (BasicBlock::iterator I = Pred->begin(); !isa<TerminatorInst>(I); ++I)
        {
            if (!AggressiveInsts.count(I) && !isa<DbgInfoIntrinsic>(I))
            {
                /* This is not an aggressive instruction that we can promote.
                 * Because of this, we won't be able to get rid of the control
                 * flow, so the xform is not worth it. */
                return false;
            }
        }
    }
    
    /* If we can still promote the PHI nodes after this gauntlet of tests,
     * do all of the PHI's now.
     *
     * Move all 'aggressive' instructions, which are defined in the
     * conditional parts of the if's up to the dominating block. */
    if (IfBlock1)
    {
        DomBlock->getInstList().splice(DomBlock->getTerminator(),
                IfBlock1->getInstList(),
                IfBlock1->begin(),
                IfBlock1->getTerminator());
    }
    if (IfBlock2)
    {
        DomBlock->getInstList().splice(DomBlock->getTerminator(),
                IfBlock2->getInstList(),
                IfBlock2->begin(),
                IfBlock2->getTerminator());
    }
  
    while (PHINode *PN = dyn_cast<PHINode>(BB->begin()))
    {
        /* Change the PHI node into a select instruction. */
        Value *TrueVal = PN->getIncomingValue(PN->getIncomingBlock(0) == IfFalse);
        Value *FalseVal = PN->getIncomingValue(PN->getIncomingBlock(0) == IfTrue);
    
        Value *NV = SelectInst::Create(IfCond, TrueVal, FalseVal, "", AfterPHIIt);
        PN->replaceAllUsesWith(NV);
        NV->takeName(PN);
    
        BB->getInstList().erase(PN);
    }
    return true;
}

bool PHItoSelectPass::runOnFunction(Function &F)
{
    bool Changed = false;

    Function::BasicBlockListType &BBList = F.getBasicBlockList();
    for (Function::BasicBlockListType::iterator I = BBList.begin(), E = BBList.end();
            I != E; I++)
    {
        if (PHINode *PN = dyn_cast<PHINode>(I->begin()))
        {
            if (PN->getNumIncomingValues() == 2)
                Changed |= FoldTwoEntryPHINode(PN);
        }
    }

    return Changed;
}

/*
 * Profile Pass
 */
class ProfilePass : public FunctionPass
{
private:
    IRFactory *IF;
    LLVMContext *Context;
    const TargetData *TD;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *IntPtrTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;

    uint64_t ProfileMode;  /* The profiling level */

    bool ProfileBlock(Function &F);
    bool ProfileTrace(Function &F);

public:
    static char ID;
    explicit ProfilePass() : FunctionPass(ID) {}
    explicit ProfilePass(IRFactory *IRF) : FunctionPass(ID), IF(IRF), Context(&IF->getContext())
    {
        LLVMTranslator *Translator = IF->Translator;
        TD = Translator->getTargetData();

        BasicType &BT = Translator->BT;
        Int8Ty  = BT.Int8Ty;
        Int32Ty = BT.Int32Ty;
        Int64Ty = BT.Int64Ty;
        IntPtrTy  = BT.IntPtrTy;
        Int8PtrTy = BT.Int8PtrTy;
        Int32PtrTy = BT.Int32PtrTy;
        Int64PtrTy = BT.Int64PtrTy;

        ProfileMode = PF->getProfile();
    }
    bool runOnFunction(Function &F);
};

char ProfilePass::ID = 0;
static RegisterPass<ProfilePass> ProfilePassP("profile", "Profile Block/Trace", false, false);
FunctionPass *createProfilePass(IRFactory *IRF) 
{
    return new ProfilePass(IRF);
}

bool ProfilePass::ProfileBlock(Function &F)
{
    return false;
}

bool ProfilePass::ProfileTrace(Function &F)
{
    TraceInfo *Trace = IF->getTrace();
    SmallVector<CallInst *, 16> InlineCalls;
    bool doInline = (HybridMode == HYBRID_SERVER) ? false : true;;
    string FName = (HybridMode == HYBRID_SERVER) ? "helper_counter_inc_client" : "helper_counter_inc";
    Function *CounterFn = IF->getModule()->getFunction(FName);
    Value *Meta[] = { CONST32(0), };

    if (doInline && (ProfileMode & PROFILE_TRACE))
    {
        Trace->Exit = new uint64_t*[MAX_PROFILE_THREADS];
        for (int i = 0; i < MAX_PROFILE_THREADS; i++)
        {
            Trace->Exit[i] = new uint64_t[Trace->NumBlock + 2];
            memset(Trace->Exit[i], 0, (Trace->NumBlock + 2) * sizeof(uint64_t));
        }
    }

    for (Function::iterator FI = F.begin(), FE = F.end(); FI != FE; FI++)
    {
        Instruction *I = static_cast<Instruction*>(FI->getTerminator());
        MDNode *MD = I->getMetadata("exit");
        if (MD == NULL)
            continue;

        int BranchType = static_cast<ConstantInt*>(MD->getOperand(0))->getZExtValue();
        int Level = static_cast<ConstantInt*>(MD->getOperand(1))->getZExtValue();

        Trace->NumExit++;
        if (BranchType == 0)
            Trace->NumIndirectBranch++;

        if (ProfileMode & PROFILE_TRACE)
        {
            Instruction *InsertPos = FI->getFirstNonPHI();
            Value *Exit = (HybridMode == HYBRID_SERVER) ? CONST32(Trace->ID) :
                ConstantExpr::getIntToPtr(CONSTPtr((uintptr_t)Trace->Exit),
                        PointerType::getUnqual(Int8Ty));

            SmallVector<Value *, 4> Params;
            Params.push_back(CONST32(BranchType));
            Params.push_back(Exit);
            Params.push_back(CONST32((BranchType == 2) ? 0 : Level + 2));
            
            CallInst *CI = CreateCall(CounterFn, Params, "", InsertPos);
            CI->setMetadata("flags", CreateMDNode(*Context, Meta, 1));

            if (doInline)
                InlineCalls.push_back(CI);
        }
    }

    while (!InlineCalls.empty())
    {
        InlineFunctionInfo IFI(0, TD);
        InlineFunction(InlineCalls.pop_back_val(), IFI);
    }

    PF->addTrace(Trace);

    return true;
}

bool ProfilePass::runOnFunction(Function &F)
{
    bool Changed = false;
    int Mode = IF->getMode();

    if (ProfileMode == PROFILE_NONE)
        return false;

    if (Mode == LLVM_GEN_BLOCK)
        Changed |= ProfileBlock(F);
    else if (Mode == LLVM_GEN_TRACE || Mode == LLVM_REGEN_TRACE)
        Changed |= ProfileTrace(F);
    else
        DM->Error("%s: invalid mode %d\n", __func__, Mode);

    return Changed;
}

/*
 * OptimizeGuestMemory Pass
 */
struct StripInfo
{
    StripInfo (Instruction *I, long off, int size, int id)
        : Off(off), Size(size), ID(id), Inst(I) {}

    long Off;
    int Size;
    int ID;
    Instruction* Inst;

    static inline bool Cmp(StripInfo *a, StripInfo *b) { return a->Off < b->Off; }
};

class MemoryAnalysis
{
public:
    typedef vector<StripInfo*> StripList;
    typedef map<Value*, StripList> Location;

    Location MemRead;
    Location MemWrite;

    MemoryAnalysis() {}

    inline void insertRead(LoadInst *LI, Value *Base, long Off, int Size, int ID)
    {
        MemRead[Base].push_back(new StripInfo(LI, Off, Size, ID));
    }
    inline void insertWrite(StoreInst *SI, Value *Base, long Off, int Size, int ID)
    {
        MemWrite[Base].push_back(new StripInfo(SI, Off, Size, ID));
    }

    inline Location &getMemRead()  { return MemRead;  }
    inline Location &getMemWrite() { return MemWrite; }

    void sort()
    {
        if (MemRead.empty() && MemWrite.empty())
            return;
        for (Location::iterator I = MemRead.begin(), E = MemRead.end(); I != E; I++)
        {
            std::sort(I->second.begin(), I->second.end(), StripInfo::Cmp);
        }
        for (Location::iterator I = MemWrite.begin(), E = MemWrite.end(); I != E; I++)
        {
            std::sort(I->second.begin(), I->second.end(), StripInfo::Cmp);
        }
    }
    void reset()
    {
        if (MemRead.empty() && MemWrite.empty())
            return;
        for (Location::iterator I = MemRead.begin(), E = MemRead.end(); I != E; I++)
        {
            for (int i = 0, e = I->second.size(); i < e; i++)
                delete I->second[i];
        }
        for (Location::iterator I = MemWrite.begin(), E = MemWrite.end(); I != E; I++)
        {
            for (int i = 0, e = I->second.size(); i < e; i++)
                delete I->second[i];
        }
        MemRead.clear();
        MemWrite.clear();
    }
};

class OptimizeGuestMemoryPass : public FunctionPass
{
private:
    IRFactory *IF;
    LLVMContext *Context;
    const TargetData *TD;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *Int128Ty;
    CONST IntegerType *IntPtrTy;
    CONST Type *FloatTy;
    CONST Type *DoubleTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int16PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;
    CONST PointerType *FloatPtrTy;
    CONST PointerType *DoublePtrTy;

    IVec toErase;

    CONST Type *getEnvelopType(int Size);
    Value *getBasePointer(Value *Base, long Min, long Max, Instruction *InsertPos);
    Value *getBaseWithConstantOffset(Value *Ptr, long &Offset);
    void AnalyzeMemoryOps(Instruction *Start, Instruction *End, MemoryAnalysis &MA);
    void FoldMemoryRead(MemoryAnalysis::StripList &SL, Value *Base, long Min, long Max, int Idx);
    void FoldMemoryWrite(MemoryAnalysis::StripList &SL, Value *Base, long Min, long Max, int Idx);
    bool CombineLoadFP(Function &F);
    bool CombindContiguousMemory(Function &F);

public:
    static char ID;
    explicit OptimizeGuestMemoryPass() : FunctionPass(ID) {}
    explicit OptimizeGuestMemoryPass(IRFactory *IRF) : FunctionPass(ID), IF(IRF), Context(&IF->getContext())
    {
        LLVMTranslator *Translator = IF->Translator;
        TD = Translator->getTargetData();

        BasicType &BT = Translator->BT;
        Int8Ty    = BT.Int8Ty;
        Int8Ty    = BT.Int16Ty;
        Int32Ty   = BT.Int32Ty;
        Int64Ty   = BT.Int64Ty;
        Int128Ty  = BT.Int128Ty;
        IntPtrTy  = BT.IntPtrTy;
        FloatTy   = BT.FloatTy;
        DoubleTy  = BT.DoubleTy;

        Int8PtrTy   = BT.Int8PtrTy;
        Int16PtrTy  = BT.Int16PtrTy;
        Int32PtrTy  = BT.Int32PtrTy;
        Int64PtrTy  = BT.Int64PtrTy;
        FloatPtrTy  = BT.FloatPtrTy;
        DoublePtrTy = BT.DoublePtrTy;
    }

    bool runOnFunction(Function &F);
};

char OptimizeGuestMemoryPass::ID = 0;
static RegisterPass<OptimizeGuestMemoryPass> OptimizeGuestMemoryPassP("opt-guest-memory",
        "Optimize Guest Memory", false, false);
FunctionPass *createOptimizeGuestMemoryPass(IRFactory *IRF) 
{
    return new OptimizeGuestMemoryPass(IRF);
}

/*
 * CombineLoadFP()
 *  Since QEMU only support i32 and i64 type, a memory load to float/double
 *  type is converted to load i32/i64 first and then bitcast to float/double.
 *  Try to transform the load-bitcast pattern back to the load of float and
 *  double type directly.
 */
bool OptimizeGuestMemoryPass::CombineLoadFP(Function &F)
{
    bool Changed = false;
    IVec GuestMemory;

    Function::BasicBlockListType &BBList = F.getBasicBlockList();
    for (Function::BasicBlockListType::iterator I = BBList.begin(), E = BBList.end();
            I != E; I++)
    {
        for (BasicBlock::iterator BI = I->begin(), BE = I->end(); BI != BE; BI++)
        {
            if (LoadInst *LI = dyn_cast<LoadInst>(BI))
            {
                if (LI->isVolatile())
                    GuestMemory.push_back(LI);
            }
        }
    }

    if (GuestMemory.empty())
        return Changed;

    for (int i = 0, e = GuestMemory.size(); i < e; i++)
    {
        LoadInst *LI = static_cast<LoadInst*>(GuestMemory[i]);
        if (LI->hasOneUse() == false)
            continue;

        BitCastInst *I = dyn_cast<BitCastInst>(*LI->use_begin());
        if (I == NULL)
            continue;

        CONST Type *NewTy = I->getType();
        IntToPtrInst *IPI = static_cast<IntToPtrInst*>(LI->getPointerOperand());
        if (NewTy == FloatTy || NewTy == DoubleTy)
        {
            Value *NewBase = new IntToPtrInst(IPI->getOperand(0), PointerType::getUnqual(NewTy), "", LI);
            LoadInst *NewLI = new LoadInst(NewBase, "", true, LI);
            LI->replaceAllUsesWith(NewLI);

            toErase.push_back(LI);
            Changed |= true;
        }
    }

    ProcessErase(toErase);

    return Changed;
}

/*
 * getBaseWithConstantOffset()
 *  Return base address and offset of a memory access pointer.
 */
Value *OptimizeGuestMemoryPass::getBaseWithConstantOffset(Value *Ptr,
        long &Offset)
{
    Operator *PtrOp = dyn_cast<Operator>(Ptr);
    if (PtrOp == 0)
        return Ptr;

    if (PtrOp->getOpcode() == Instruction::BitCast ||
            PtrOp->getOpcode() == Instruction::IntToPtr)
        return getBaseWithConstantOffset(PtrOp->getOperand(0), Offset);

    /* If this is a Add/Sub with constant operand, we can look through it. */
    if (PtrOp->getOpcode() == Instruction::Add)
    {
        ConstantInt *OpC = dyn_cast<ConstantInt>(PtrOp->getOperand(1));
        if (OpC == NULL)
            return Ptr;
        Offset += OpC->getSExtValue();
        return getBaseWithConstantOffset(PtrOp->getOperand(0), Offset);
    }
    else if (PtrOp->getOpcode() == Instruction::Sub)
    {
        ConstantInt *OpC = dyn_cast<ConstantInt>(PtrOp->getOperand(1));
        if (OpC == NULL)
            return Ptr;
        Offset -= OpC->getSExtValue();
        return getBaseWithConstantOffset(PtrOp->getOperand(0), Offset);
    }

    /* If this is a GEP with constant indices, we can look through it. */
    GEPOperator *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (GEP == 0 || !GEP->hasAllConstantIndices())
        return Ptr;

    gep_type_iterator GTI = gep_type_begin(GEP);
    for (User::op_iterator I = GEP->idx_begin(), E = GEP->idx_end(); I != E;
            ++I, ++GTI)
    {
        ConstantInt *OpC = cast<ConstantInt>(*I);
        if (OpC->isZero())
            continue;

        /* Handle a struct and array indices which add their offset to the
         * pointer. */
        if (CONST StructType *STy = dyn_cast<StructType>(*GTI))
            Offset += TD->getStructLayout(STy)->getElementOffset(OpC->getZExtValue());
        else
        {
            size_t Size = TD->getTypeAllocSize(GTI.getIndexedType());
            Offset += OpC->getSExtValue() * Size;
        }
    }

    return getBaseWithConstantOffset(GEP->getPointerOperand(), Offset);
}

void OptimizeGuestMemoryPass::AnalyzeMemoryOps(Instruction *S, Instruction *E,
        MemoryAnalysis &MA)
{
    BasicBlock::iterator Start = ++BasicBlock::iterator(S);
    BasicBlock::iterator End = E;

    /* Clear old records. */
    MA.reset();

    /* Scan for guest memory operations and put the operations of the same base
     * pointer at the same recording slot. */
    int ID = 0;
    for (; Start != End; Start++)
    {
        if (LoadInst *LI = dyn_cast<LoadInst>(Start))
        {
            if (LI->isVolatile() == false)
                continue;

            long Off = 0;
            Value *Base = getBaseWithConstantOffset(LI->getPointerOperand(), Off);
            if (Base)
            {
                size_t Size = TD->getTypeSizeInBits(LI->getType())/8;
                MA.insertRead(LI, Base, Off, Size, ID++);
            }
        }
        else if (StoreInst *SI = dyn_cast<StoreInst>(Start))
        {
            if (SI->isVolatile() == false)
                continue;
            
            long Off = 0;
            Value *Base = getBaseWithConstantOffset(SI->getPointerOperand(), Off);
            if (Base)
            {
                size_t Size = TD->getTypeSizeInBits(SI->getOperand(0)->getType())/8;
                MA.insertWrite(SI, Base, Off, Size, ID++);
            }
        }
    }

    /* Sort the memory operations for each location. */
    MA.sort();
}

bool ComputeEnvelop(MemoryAnalysis::StripList &SL, long &Min, long &Max,
        int &Idx, bool isStore)
{
    int Last = SL.size() - 1;
    int ID = SL[0]->ID;
    int idx = 0;

    for (int i = 0; i < Last; i++)
    {
        StripInfo *S1 = SL[i];
        StripInfo *S2 = SL[i+1];
        if (S1->Off + S1->Size != S2->Off)
            return false;

        if (isStore == false && S2->ID < ID)
        {
            ID = S2->ID;
            idx = i+1;
        }
        if (isStore == true && S2->ID > ID)
        {
            ID = S2->ID;
            idx = i+1;
        }
    }

    if (SL[0]->Off - GUEST_BASE != 0)
        return false;

    Min = SL[0]->Off;
    Max = SL[Last]->Off + SL[Last]->Size;
    Idx = idx;
    return true;
}

CONST Type *OptimizeGuestMemoryPass::getEnvelopType(int Size)
{
    switch (Size)
    {
        case 4:  return Int32Ty;  break;
        case 8:  return Int64Ty;  break;
        case 2:  return Int16Ty;  break;
        case 1:  return Int8Ty;   break;
        case 16: return Int128Ty; break;
        default:
            return Type::getIntNTy(*Context, Size * 8); break;
    }
    return NULL;
}

Value *OptimizeGuestMemoryPass::getBasePointer(Value *Base, long Min,
        long Max, Instruction *InsertPos)
{
    CONST Type *Ty = NULL;
    int Size = Max - Min;

    if (Base->getType()->isPointerTy())
        DM->Error("%s: internal error.\n", __func__);

    switch (Size)
    {
        case 4:  Ty = Int32PtrTy; break;
        case 8:  Ty = Int64PtrTy; break;
        case 2:  Ty = Int16PtrTy; break;
        case 1:  Ty = Int8PtrTy;  break;
        case 16: Ty = PointerType::getUnqual(Int128Ty); break;
        default:
            Ty = Type::getIntNTy(*Context, Size * 8);
            Ty = PointerType::getUnqual(Ty);
            break;
    }

    Value *V = Base;
    if (Min != 0 )
        V = BinaryOperator::Create(Instruction::Add, V, CINT(Min), "", InsertPos);
    return new IntToPtrInst(V, Ty, "", InsertPos);
}

void OptimizeGuestMemoryPass::FoldMemoryRead(MemoryAnalysis::StripList &SL,
        Value *Base, long Min, long Max, int Idx)
{
    Instruction *InsertPos = SL[Idx]->Inst;
    Value *BasePtr = getBasePointer(Base, Min, Max, InsertPos);
    LoadInst *NewLI = new LoadInst(BasePtr, "", true, InsertPos);

    for (int i = 0, e = SL.size(); i < e; i++)
    {
        Value *V = NewLI;
        StripInfo *Strip = SL[i];
        long Off = (Strip->Off - Min) * 8;
        LoadInst *LI = static_cast<LoadInst*>(Strip->Inst);

        /* Discard the lower bits. */
        if (Off != 0)
            V = BinaryOperator::Create(Instruction::LShr, V, CINT(Off), "", LI);
        /* Discard the higher bits. */
        V = new TruncInst(V, LI->getType(), "", LI);
        LI->replaceAllUsesWith(V);

        toErase.push_back(LI);
    }
}

void OptimizeGuestMemoryPass::FoldMemoryWrite(MemoryAnalysis::StripList &SL,
        Value *Base, long Min, long Max, int Idx)
{
    Instruction *InsertPos = SL[Idx]->Inst;
    Value *BasePtr = getBasePointer(Base, Min, Max, InsertPos);
    CONST Type *Ty = getEnvelopType(Max-Min);

    Value *NewV = NULL;
    for (int i = 0, e = SL.size(); i < e; i++)
    {
        StripInfo *Strip = SL[i];
        long Off = (Strip->Off - Min) * 8;
        StoreInst *SI = static_cast<StoreInst*>(Strip->Inst);
        Value *V = new ZExtInst(SI->getOperand(0), Ty, "", InsertPos);
        if (Off != 0)
            V = BinaryOperator::Create(Instruction::Shl, V, CINT(Off), "", InsertPos);

        NewV = (NewV == NULL) ? V : 
            BinaryOperator::Create(Instruction::Or, NewV, V, "", InsertPos);

        toErase.push_back(SI);
    }

    new StoreInst(NewV, BasePtr, true, InsertPos);
}

bool OptimizeGuestMemoryPass::CombindContiguousMemory(Function &F)
{
    IVec &Separator = IF->getSeparator();
    MemoryAnalysis MA;
    int Idx;
    long Min, Max;

    for (int i = 0, e = Separator.size() - 1; i < e; i++)
    {
        /* We only optimize the IRs within the same BasicBlock. */
        if (Separator[i]->getParent() != Separator[i+1]->getParent())
            continue;

        AnalyzeMemoryOps(Separator[i], Separator[i+1], MA);

        MemoryAnalysis::Location &MemRead = MA.getMemRead();
        MemoryAnalysis::Location &MemWrite = MA.getMemWrite();
        if (MemRead.empty() && MemWrite.empty())
            continue;

        for (MemoryAnalysis::Location::iterator I = MemRead.begin(),
                E = MemRead.end(); I != E; I++)
        {
            Value *Base = I->first;
            MemoryAnalysis::StripList &SL = I->second;
            if (SL.size() <= 1)
                continue;

            if (ComputeEnvelop(SL, Min, Max, Idx, false) == true)
                FoldMemoryRead(SL, Base, Min, Max, Idx);
        }
    }

    ProcessErase(toErase);

    return false;
}

bool OptimizeGuestMemoryPass::runOnFunction(Function &F)
{
    bool Changed = false;

    Changed |= CombineLoadFP(F);
    Changed |= CombindContiguousMemory(F);
    ProcessErase(IF->getSeparator());

    return Changed;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

