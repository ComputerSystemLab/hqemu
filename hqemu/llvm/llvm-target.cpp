/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include "llvm/InlineAsm.h"
#include "llvm/Operator.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm-profile.h"
#include "llvm.h"
#include "JIT.h"
#include "utils.h"

extern "C"
{
#include "helper.h"
#include "cache-utils.h"
};

typedef DenseMap<uintptr_t, uintptr_t> ConstPoolMap;
typedef DenseMap<uintptr_t, const char *> ESMap;

extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern int HybridMode;
extern CodeGenOpt::Level OptLevel;
extern llvm_lock_t llvm_global_lock;
extern llvm_lock_t llvm_interface_lock;
extern llvm_lock_t llvm_debug_lock;

uint8_t *llvm_tb_ret_addr;
extern uint8_t *ibtc_ret_addr;
extern uint8_t *ibchain_ret_addr;
extern uint8_t *ibtc_helper_entry;
extern uint8_t *tb_ret_addr;

static char include_helper[][64] = {
#include "llvm-helper.h"
};

#define TCG_RUNTIME_HELPER(a)   { #a, (void*)a, }
TCGRuntimeHelper RuntimeHelper[] = {
    TCG_RUNTIME_HELPER(tcg_helper_div_i32),
    TCG_RUNTIME_HELPER(tcg_helper_rem_i32),
    TCG_RUNTIME_HELPER(tcg_helper_divu_i32),
    TCG_RUNTIME_HELPER(tcg_helper_remu_i32),
    TCG_RUNTIME_HELPER(tcg_helper_shl_i64),
    TCG_RUNTIME_HELPER(tcg_helper_shr_i64),
    TCG_RUNTIME_HELPER(tcg_helper_sar_i64),
    TCG_RUNTIME_HELPER(tcg_helper_div_i64),
    TCG_RUNTIME_HELPER(tcg_helper_rem_i64),
    TCG_RUNTIME_HELPER(tcg_helper_divu_i64),
    TCG_RUNTIME_HELPER(tcg_helper_remu_i64),
};
#undef TGC_RUNTIME_HELPER

static unsigned CountCodeReductionForConstant(Value *V, CodeMetrics &Metrics);
static unsigned CountCodeReductionForAlloca(Value *V);


/*
 * MapHelperFunctions()
 *  Keep track of the function pointer and name pair of helper functions in
 *  QEMU for emulating guest architecture. Used for the translation from the
 *  TCG call operation to LLVM IR.
 */
void LLVMEnv::MapHelperFunctions()
{
    /* Runtime helper functions (used for 64-to-32 emulation). */
    for (int i = 0, e = ARRAY_SIZE(RuntimeHelper); i < e; i++)
    {
        const char *fname = RuntimeHelper[i].fname;
        uintptr_t func = (uintptr_t)RuntimeHelper[i].func;
        HelperMap[func] = fname;
    }

    /* General helper functions. */
#define DEF_HELPER_FLAGS_0(name, flags, ret) \
    do {                                     \
        HelperMap[(long)helper_##name] =  "helper_"#name; \
    } while (0);
#define DEF_HELPER_FLAGS_1(name, flags, ret, t1) DEF_HELPER_FLAGS_0(name, flags, ret)
#define DEF_HELPER_FLAGS_2(name, flags, ret, t1, t2) DEF_HELPER_FLAGS_0(name, flags, ret)
#define DEF_HELPER_FLAGS_3(name, flags, ret, t1, t2, t3) DEF_HELPER_FLAGS_0(name, flags, ret)
#define DEF_HELPER_FLAGS_4(name, flags, ret, t1, t2, t3, t4) DEF_HELPER_FLAGS_0(name, flags, ret)

#define GEN_HELPER 3
#include "helper.h"

#undef DEF_HELPER_FLAGS_0
#undef DEF_HELPER_FLAGS_1
#undef DEF_HELPER_FLAGS_2
#undef DEF_HELPER_FLAGS_3
#undef DEF_HELPER_FLAGS_4
#undef GEN_HELPER
}

/*
 * Scalaro to Vector Transformation
 */
VectorFactory::VectorFactory(LLVMContext &CTX, BasicType &BT) : Context(CTX)
{
    Int8Ty  = BT.Int8Ty;
    Int16Ty = BT.Int16Ty;
    Int32Ty = BT.Int32Ty;
    Int64Ty = BT.Int64Ty;

    vector<Constant *> V;
    V.push_back(CONST64(-1));
    V.push_back(CONST64(-1));
    VecMinusOne = getConstantVector(V);
}

/*
 * getConstantVector()
 *  Return a vector of constant values. 
 */
inline Value *VectorFactory::getConstantVector(vector<Constant *> &V)
{
    if (V.size() == 0)
        DM->Error("%s: fatal error.\n", __func__);

#if defined(LLVM_V28) || defined(LLVM_V29)
    VectorType *VectorTy = VectorType::get(V.front()->getType(), V.size());
    return ConstantVector::get(VectorTy, V);
#else
    return ConstantVector::get(V);
#endif
}

/*
 * Init()
 *  Initialize the scalar the vector transformation.
 */
void VectorFactory::Init(Function *F, CONST Type *ElementType, int NumElements)
{
    if (F->arg_size() != 2)
        DM->Error("%s: invalid vector function.\n", __func__);

    F->deleteBody();
    LastInst = ReturnInst::Create(Context, BasicBlock::Create(Context, "entry", F));
    VectorTy = PointerType::getUnqual(VectorType::get(ElementType, NumElements));

    Function::arg_iterator args = F->arg_begin();
    In1 = CAST(args++, VectorTy);
    In2 = CAST(args++, VectorTy);
    Vec1 = new LoadInst(In1, "", false, 16, LastInst);
    Vec2 = new LoadInst(In2, "", false, 16, LastInst);
}

/*
 * setConstantVec2()
 *  Make a constant vector as the second operand.
 */
void VectorFactory::setConstantVec2(CONST Type *Ty, int NumElements, int Val)
{
    Constant *ConstVal;
    vector<Constant *> V;

    if (Ty == Int8Ty)       ConstVal = CONST8(Val);
    else if (Ty == Int16Ty) ConstVal = CONST16(Val);
    else if (Ty == Int32Ty) ConstVal = CONST32(Val);
    else if (Ty == Int64Ty) ConstVal = CONST64(Val);
    else
        DM->Error("%s: internal error.\n", __func__);

    for (int i = 0; i < NumElements; i++)
        V.push_back(ConstVal);

    Vec2 = getConstantVector(V);
}

/*
 * addOp()
 *  Return the LLVM Vector operation.
 */
void VectorFactory::addOp(int Op)
{
    if (Op == OP_ADD)       Vec1 = ADD(Vec1, Vec2);
    else if (Op == OP_SUB)  Vec1 = SUB(Vec1, Vec2);
    else if (Op == OP_MUL)  Vec1 = MUL(Vec1, Vec2);
    else if (Op == OP_OR)   Vec1 = OR(Vec1, Vec2);
    else if (Op == OP_XOR)  Vec1 = XOR(Vec1, Vec2);
    else if (Op == OP_AND)  Vec1 = AND(Vec1, Vec2);
    else if (Op == OP_ANDN)
    {
        Value *Tmp = XOR(Vec1, VecMinusOne);
        Vec1 = AND(Tmp, Vec2);
    }
    else if (Op == OP_LSHR) Vec1 = LSHR(Vec1, Vec2);
    else if (Op == OP_FADD) Vec1 = FADD(Vec1, Vec2);
    else if (Op == OP_FSUB) Vec1 = FSUB(Vec1, Vec2);
    else if (Op == OP_FMUL) Vec1 = FMUL(Vec1, Vec2);
    else if (Op == OP_FDIV) Vec1 = FDIV(Vec1, Vec2);
}

/*
 * Create()
 *  Perform the scalar to vector transformation.
 */
void VectorFactory::Create(Function *F, int Op, CONST Type *ElementType,
        int NumElements)
{
    Init(F, ElementType, NumElements);
    addOp(Op);
    Finalize();
}

void VectorFactory::Create(Function *F, int Op, CONST Type *ElementType,
        int NumElements, int Idx)
{
    Value *OutData;

    Init(F, ElementType, NumElements);
    OutData = Vec1;
    Vec1 = ExtractElementInst::Create(Vec1, CONST32(Idx), "", LastInst);
    Vec2 = ExtractElementInst::Create(Vec2, CONST32(Idx), "", LastInst);
    addOp(Op);
    Vec1 = InsertElementInst::Create(OutData, Vec1, CONST32(Idx), "", LastInst);
    Finalize();
}

inline void VectorFactory::Finalize()
{
    new StoreInst(Vec1, In1, false, 16, LastInst);
}

/*
 * InitHelperFunctions()
 *  Materialize helper functions and compute inline costs.
 */
void LLVMTranslator::InitHelperFunctions(TCGContext *s)
{
    VF = new VectorFactory(Context, BT);

    UpdateDependentFunctions();

    /* Initialize specific helper functions that are allowed to be inlined. */
    for (int i = 0, e = ARRAY_SIZE(include_helper); i < e; i++)
    {
        string ErrInfo;
        HelperFuncs[include_helper[i]] = new HelperInfo;
        Function *Func = Mod->getFunction(include_helper[i]);
        if (Func && Func->isMaterializable())
        {
            Func->Materialize(&ErrInfo);
            OptimizeSIMDFunc(Func);
        }
    }

    for (int i = 0; i < s->nb_helpers; i++)
    {
        TCGHelperInfo *th = &s->helpers[i];
        string FName = string("helper_").append(th->name);
        string FNameNoInline = string("helper_").append(th->name).append("_noinline");

        if (HelperFuncs.count(FName))
        {
            HelperInfo *Helper = HelperFuncs[FName];
            HelperFuncs[FNameNoInline] = Helper;

            Function *Func = Mod->getFunction(FName);

            Mod->getOrInsertFunction(FNameNoInline, Func->getFunctionType());
            EE->updateGlobalMapping(Mod->getNamedValue(FNameNoInline),
                    (void*)th->func);

            Helper->Func = Func;
            Helper->FuncNoInline = Mod->getFunction(FNameNoInline);
        }
    }

    /* Analyze the inline cost for each helper function and make a non-inlined
     * counterpart object in LLVM Module. For the non-inlined function, just 
     * remap the function address in LLVM module which causes the JIT to emit a
     * call instruction to the function address. */
    for (int i = 0; i < s->nb_helpers; i++)
    {
        TCGHelperInfo *th = &s->helpers[i];
        string FName = string("helper_").append(th->name);

        if (HelperFuncs.count(FName))
        {
            HelperInfo *Helper = HelperFuncs[FName];

            Function *Func = Mod->getFunction(FName);
            int ret = OptimizeHelperFunc(Func);
            if (ret < 0)
            {
                /* If the helper function consists of loops, it is not suitable
                 * to be inlined because it conflicts to the state mapping
                 * pass. */
                HelperFuncs.erase(FName);
                goto skip;
            }

            CodeMetrics *Metrics = &Helper->Metrics;
            vector<ArgInfo> *Weight = &Helper->ArgumentWeights;

            Metrics->analyzeFunction(Func);
            Weight->reserve(Func->arg_size());

            for (Function::arg_iterator I = Func->arg_begin(),
                    E = Func->arg_end(); I != E; I++)
            {
                Weight->push_back(ArgInfo(
                            CountCodeReductionForConstant(I, *Metrics),
                            CountCodeReductionForAlloca(I)));
            }

            continue;
        }
skip:
        /* Bind the function pointer to avoid inlining this helper function. */
        EE->updateGlobalMapping(Mod->getNamedValue(FName), (void*)th->func);
    }

#if defined(CONFIG_USER_ONLY)
    /* Force the JIT to resolve the address of the system libraries and global
     * variables which are used by the helper functions before we do any JIT
     * compilation. This is required for the clinet/server model because it
     * does not support lazy address relocation. */
    resolveLibcall();
    resolveGlobalVariable();
#endif
}

/*
 * UpdateFPUFunctions()
 *  Bind addresses to the nested functions which are used by the softfloat
 *  helper functions.
 */
void LLVMTranslator::UpdateFPUFunctions()
{
#define UpdateMapping(a) EE->updateGlobalMapping(Mod->getNamedValue(#a), (void*)a)
#if defined(CONFIG_SOFTFLOAT)
    UpdateMapping(float32_add);
    UpdateMapping(float32_sub);
    UpdateMapping(float32_mul);
    UpdateMapping(float32_div);
    UpdateMapping(float32_sqrt);
    UpdateMapping(float32_scalbn);
    UpdateMapping(float32_compare);
    UpdateMapping(float32_compare_quiet);
    UpdateMapping(float64_add);
    UpdateMapping(float64_sub);
    UpdateMapping(float64_mul);
    UpdateMapping(float64_div);
    UpdateMapping(float64_sqrt);
    UpdateMapping(float64_scalbn);
    UpdateMapping(float64_compare);
    UpdateMapping(float64_compare_quiet);
    UpdateMapping(float16_to_float32);
    UpdateMapping(float32_to_float16);
    UpdateMapping(float32_to_float64);
    UpdateMapping(float32_to_int32);
    UpdateMapping(float32_to_int64);
    UpdateMapping(float32_to_uint32);
    UpdateMapping(float64_to_float32);
    UpdateMapping(float64_to_int32);
    UpdateMapping(float64_to_int64);
    UpdateMapping(float64_to_uint32);
    UpdateMapping(int32_to_float32);
    UpdateMapping(int32_to_float64);
    UpdateMapping(int64_to_float32);
    UpdateMapping(uint32_to_float32);
    UpdateMapping(uint32_to_float64);
    UpdateMapping(float32_to_int32_round_to_zero);
#endif
#undef UpdateMapping
}

/*
 * UpdateDependentFunctions()
 *  Bind addresses to the nested functions which are used by the helper
 *  fucntions.
 */
#if defined(CONFIG_USER_ONLY)
void LLVMTranslator::UpdateDependentFunctions()
{
    /* Set external symbol address for LLVM runtime. */
    EE->updateGlobalMapping(Mod->getNamedValue("cpu_loop_exit"), (void*)cpu_loop_exit);
    EE->updateGlobalMapping(Mod->getNamedValue("guest_base"), (void*)&guest_base);
    EE->updateGlobalMapping(Mod->getNamedValue("patch_jmp"), (void*)patch_jmp);
    EE->updateGlobalMapping(Mod->getNamedValue("ibtc_ret_addr"), (void*)ibtc_ret_addr);
    EE->updateGlobalMapping(Mod->getNamedValue("ibtc_helper_entry"), (void*)ibtc_helper_entry);
    EE->updateGlobalMapping(Mod->getNamedValue("logfile"), (void*)&logfile);
    EE->updateGlobalMapping(Mod->getNamedValue("loglevel"), (void*)&loglevel);

#if defined(TARGET_I386)
    EE->updateGlobalMapping(Mod->getNamedValue("raise_exception"), (void*)raise_exception);
    EE->updateGlobalMapping(Mod->getNamedValue("raise_exception_err"), (void*)raise_exception_err);
#elif defined(TARGET_ARM)
    EE->updateGlobalMapping(Mod->getNamedValue("raise_exception"), (void*)raise_exception);
#endif

#if defined(TCG_TARGET_ARM)
    EE->updateGlobalMapping(Mod->getNamedValue("helper_counter_inc"), (void*)helper_counter_inc);
#endif
#if defined(CONFIG_NET)
    EE->updateGlobalMapping(Mod->getNamedValue("helper_counter_inc_client"), (void*)helper_counter_inc_client);
#endif

    UpdateFPUFunctions();
}

#else
void LLVMTranslator::UpdateDependentFunctions()
{
}
#endif

/*
 * PopulateCallTable()
 *  Initialize CallTable which is used to reserve space in the code cache for
 *  trace chaining. (Only used by the ARM host)
 */
void LLVMTranslator::PopulateCallTable()
{
#if defined(TCG_TARGET_ARM)
    if (sizeof(tcg_target_ulong) != 4)
        DM->Error("%s: fatal error.\n", __func__);
    
    int NumCallEntry = 256;
    vector<tcg_target_ulong> &CallTable = Env->getCallTable();

    /* If the CallTable is empty, initialize it. */
    if (CallTable.empty())
    {
        CallTable.push_back(0xa9999999);
        for (int i = 1; i < NumCallEntry; i++)
            CallTable.push_back(0x99999900 | (uint8_t)i);
    }

    for (int i = 0; i < NumCallEntry; i++)
    {
        char Name[32];
        sprintf(Name, "%x", CallTable[i]);
        FunctionType *FT = FunctionType::get(VoidTy, false);
        Mod->getOrInsertFunction(Name, FT);
        EE->updateGlobalMapping(Mod->getNamedValue(Name), (void*)CallTable[i]);
    }
#endif
}

/*
 * UpdateMapping()
 *  Bind an address to a function.
 */
void LLVMTranslator::UpdateMapping(string Name, uintptr_t Addr)
{
    EE->updateGlobalMapping(Mod->getNamedValue(Name), (void*)Addr);
}

/*
 * resolveLibcall()
 *  Get the address of external library symbols.
 */
void LLVMTranslator::resolveLibcall()
{
    char LibcallNames[][128] = {
        "__ashlhi3", "__ashlsi3", "__ashldi3", "__ashlti3",
        "__lshrhi3", "__lshrsi3", "__lshrdi3", "__lshrti3",
        "__ashrhi3", "__ashrsi3", "__ashrdi3", "__ashrti3",
        "__mulqi3",  "__mulhi3",  "__mulsi3",  "__muldi3", "__multi3",
        "__divqi3",  "__divhi3",  "__divsi3",  "__divdi3", "__divti3",
        "__udivqi3", "__udivhi3", "__udivsi3", "__udivdi3", "__udivti3",
        "__modqi3",  "__modhi3",  "__modsi3",  "__moddi3", "__modti3",
        "__umodqi3", "__umodhi3", "__umodsi3", "__umoddi3", "__umodti3",
        "__negsi2",  "__negdi2",  "__addsf3",  "__adddf3",  "__addxf3",
        "__gcc_qadd", "__gcc_qsub", "__gcc_qmul", "__gcc_qdiv",
        "__subsf3",  "__subdf3",  "__subxf3",
        "__mulsf3",  "__muldf3",  "__mulxf3",
        "__divsf3",  "__divdf3",  "__divxf3",
        "fmodf", "fmod", "fmodl", "fmodl",
        "__powisf2", "__powidf2", "__powixf2", "__powitf2",
        "sqrtf", "sqrt", "sqrtl", "sqrtl",
        "logf", "log", "logl", "logl",
        "log2f", "log2", "log2l", "log2l",
        "log10f", "log10", "log10l", "log10l",
        "expf", "exp", "expl", "expl",
        "exp2f", "exp2", "exp2l", "exp2l",
        "sinf", "sin", "sinl", "sinl",
        "cosf", "cos", "cosl", "cosl",
        "powf", "pow", "powl", "powl",
        "ceilf", "ceil", "ceill", "ceill",
        "truncf", "trunc", "truncl", "truncl",
        "rintf", "rint", "rintl", "rintl",
        "nearbyintf", "nearbyint", "nearbyintl", "nearbyintl",
        "floorf", "floor", "floorl", "floorl",
        "copysignf", "copysign", "copysignl", "copysignl",
        "__extendsfdf2", "__gnu_h2f_ieee", "__gnu_f2h_ieee",
        "__truncdfsf2", "__truncxfsf2", "__trunctfsf2", "__truncxfdf2", "__trunctfdf2",
        "__fixsfqi", "__fixsfhi", "__fixsfsi", "__fixsfdi", "__fixsfti", 
        "__fixdfqi", "__fixdfhi", "__fixdfsi", "__fixdfdi", "__fixdfti",
        "__fixxfsi", "__fixxfdi", "__fixxfti",
        "__fixtfsi", "__fixtfdi", "__fixtfti",
        "__fixunssfqi", "__fixunssfhi", "__fixunssfsi", "__fixunssfdi", "__fixunssfti",
        "__fixunsdfqi", "__fixunsdfhi", "__fixunsdfsi", "__fixunsdfdi", "__fixunsdfti",
        "__fixunsxfsi", "__fixunsxfdi", "__fixunsxfti",
        "__fixunstfsi", "__fixunstfdi", "__fixunstfti",
        "__floatsisf", "__floatsidf", "__floatsixf", "__floatsitf",
        "__floatdisf", "__floatdidf", "__floatdixf", "__floatditf",
        "__floattisf", "__floattidf", "__floattixf", "__floattitf",
        "__floatunsisf", "__floatunsidf", "__floatunsixf", "__floatunsitf",
        "__floatundisf", "__floatundidf", "__floatundixf", "__floatunditf",
        "__floatuntisf", "__floatuntidf", "__floatuntixf", "__floatuntitf",
        "__eqsf2", "__eqdf2", "__nesf2", "__nedf2",
        "__gesf2", "__gedf2", "__ltsf2", "__ltdf2",
        "__lesf2", "__ledf2", "__gtsf2", "__gtdf2",
        "__unordsf2", "__unorddf2", "__unordsf2", "__unorddf2",
        "memcpy", "memmove", "memset",
        "_Unwind_Resume",
        "__sync_val_compare_and_swap_1",
        "__sync_val_compare_and_swap_2",
        "__sync_val_compare_and_swap_4",
        "__sync_val_compare_and_swap_8",
        "__sync_lock_test_and_set_1",
        "__sync_lock_test_and_set_2",
        "__sync_lock_test_and_set_4",
        "__sync_lock_test_and_set_8",
        "__sync_fetch_and_add_1",
        "__sync_fetch_and_add_2",
        "__sync_fetch_and_add_4",
        "__sync_fetch_and_add_8",
        "__sync_fetch_and_sub_1",
        "__sync_fetch_and_sub_2",
        "__sync_fetch_and_sub_4",
        "__sync_fetch_and_sub_8",
        "__sync_fetch_and_and_1",
        "__sync_fetch_and_and_2",
        "__sync_fetch_and_and_4",
        "__sync_fetch_and_and_8",
        "__sync_fetch_and_or_1",
        "__sync_fetch_and_or_2",
        "__sync_fetch_and_or_4",
        "__sync_fetch_and_or_8",
        "__sync_fetch_and_xor_1",
        "__sync_fetch_and_xor_2",
        "__sync_fetch_and-xor_4",
        "__sync_fetch_and_xor_8",
        "__sync_fetch_and_nand_1",
        "__sync_fetch_and_nand_2",
        "__sync_fetch_and_nand_4",
        "__sync_fetch_and_nand_8",
    };

    for (int i = 0, e = ARRAY_SIZE(LibcallNames); i < e; i++)
    {
        JSData.ExternalSymbolIdx[LibcallNames[i]] = i;
        if (HybridMode == HYBRID_CLIENT)
        {
            void *Address = ((JIT*)EE)->getPointerToNamedFunction(
                    LibcallNames[i], false);
            JSData.ExternalSymbol.push_back((tcg_target_long)(long)Address);
        }
    }
}

/*
 * MoveToReg()
 *  Generate the inline assembly of moving the value V to the register Reg.
 */
void LLVMTranslator::MoveToReg(Value *V, string Reg, Instruction *InsertPos)
{
    static char AsmString[64] = "\0";
    vector<CONST Type *> Types;
    SmallVector<Value *, 4> Params;

    Types.push_back(V->getType());
    Params.push_back(V);

    sprintf(AsmString, "{%s}", Reg.c_str());
    InlineAsm *IA = InlineAsm::get(FunctionType::get(VoidTy, Types, false), "",
            AsmString, true);
    CreateCall(IA, Params, "", InsertPos);
}

/*
 * resolveGlobalVariable()
 *  Get the address of the global variables used by the helper functions.
 */
void LLVMTranslator::resolveGlobalVariable()
{
    if (HybridMode == HYBRID_SERVER)
        return;

    /* Retrieve name and address of global variables. */
    FunctionType *FT = FunctionType::get(VoidTy, false);
    Function *GVFunc = FUNC(FT, "GlobalVariableFunc");
    GVFunc->setCallingConv(CallingConv::C);
    GVFunc->addFnAttr(FNATTR(NoReturn));
    GVFunc->addFnAttr(FNATTR(NoUnwind));
    GVFunc->addFnAttr(FNATTR(Naked));

    BasicBlock *GVEntry = BasicBlock::Create(Context, "entry", GVFunc);
    ReturnInst *RI = ReturnInst::Create(Context, NULL, GVEntry);
    
    for (Module::global_iterator I = Mod->global_begin(), E = Mod->global_end();
            I != E; I++)
    {
        if (I->isThreadLocal() || I->hasPrivateLinkage() || I->getName() == "basetype")
            continue;
        MoveToReg(I, BaseRegStr, RI);
    }
 
    if (DM->getDebug() & DEBUG_IR)
    {
        DM->Debug(DEBUG_LLVM, "%s: populate global variables.\n", __func__);
        GVFunc->dump();
    }

    EE->runJITOnFunction(GVFunc);
    
    GlobalMap &GlobalVariable = JSData.GlobalVariable;
    for (Module::global_iterator I = Mod->global_begin(), E = Mod->global_end();
            I != E; I++)
    {
        uintptr_t Addr = (uintptr_t)EE->getPointerToGlobalIfAvailable(I);
        if (Addr == 0)
            continue;

        if (GlobalVariable.count(Addr) == 0)
            GlobalVariable[Addr] = I->getName();
    }
}

/*
 * isLibcall()
 *  Determine if the function name is a system library.
 */
static bool isLibcall(string &Name)
{
    if (Name == "fmodf" || Name == "fmod" || Name == "fmodl"
            || Name == "abs" || Name == "labs" || Name == "llabs"
            || Name == "fabs" || Name == "fabsf" || Name == "fabsl"
            || Name == "sqrtf" || Name == "sqrt" || Name == "sqrtl"
            || Name == "logf" || Name == "log" || Name == "logl"
            || Name == "log2f" || Name == "log2" || Name == "log2l"
            || Name == "log10f" || Name == "log10" || Name == "log10l"
            || Name == "expf" || Name == "exp" || Name == "expl"
            || Name == "exp2f" || Name == "exp2" || Name == "exp2l"
            || Name == "ldexpf" || Name == "ldexp" || Name == "ldexpl"
            || Name == "sinf" || Name == "sin" || Name == "sinl"
            || Name == "cosf" || Name == "cos" || Name == "cosl"
            || Name == "tanf" || Name == "tan" || Name == "tanl"
            || Name == "atanf" || Name == "atan" || Name == "atanl"
            || Name == "atanf2" || Name == "atan2" || Name == "atanl2"
            || Name == "powf" || Name == "pow" || Name == "powl"
            || Name == "ceilf" || Name == "ceil" || Name == "ceill"
            || Name == "truncf" || Name == "trunc" || Name == "truncl"
            || Name == "rintf" || Name == "rint" || Name == "rintl"
            || Name == "lrintf" || Name == "lrint" || Name == "lrintl"
            || Name == "nearbyintf" || Name == "nearbyint" || Name == "nearbyintl"
            || Name == "floorf" || Name == "floor" || Name == "floorl"
            || Name == "copysignf" || Name == "copysign" || Name == "copysignl"
            || Name == "memcpy" || Name == "memmove" || Name == "memset"
            || Name == "fegetround" || Name == "fesetround"
            || Name == "__isinfl" || Name == "__isnanl")
    {
        return true;
    }

    return false;
}

/*
 * isSoftFPcall()
 *  Determine if the function name is a softfloat helper function.
 */
static bool isSoftFPcall(string &Name)
{
    static char SoftFPName[][128] = {
        "float16_to_float32",
        "float32_add",
        "float32_compare",
        "float32_compare_quiet",
        "float32_div",
        "float32_mul",
        "float32_scalbn",
        "float32_sqrt",
        "float32_sub",
        "float32_to_float16",
        "float32_to_float64",
        "float32_to_int32",
        "float32_to_int64",
        "float32_to_uint32",
        "float64_add",
        "float64_compare",
        "float64_compare_quiet",
        "float64_div",
        "float64_mul",
        "float64_scalbn",
        "float64_sqrt",
        "float64_sub",
        "float64_to_float32",
        "float64_to_int32",
        "float64_to_int64",
        "float64_to_uint32",
        "int32_to_float32",
        "int32_to_float64",
        "int64_to_float32",
        "normalizeRoundAndPackFloat128",
        "propagateFloat128NaN",
        "propagateFloatx80NaN",
        "roundAndPackFloat128",
        "roundAndPackFloat32",
        "roundAndPackFloat64",
        "roundAndPackFloatx80",
        "set_float_rounding_mode",
        "subFloat128Sigs",
        "subFloat32Sigs",
        "subFloat64Sigs",
        "subFloatx80Sigs",
        "uint32_to_float32",
        "uint32_to_float64",
#if 0
        /* FIXME: this function causes LLVM JIT error:
           LLVM ERROR: Error reading function 'set_float_exception_flags' from bitcode file: Malformed block record */
        "set_float_exception_flags",
#endif
        "addFloat32Sigs",
        "addFloat64Sigs",
    };

    for (int i = 0, e = ARRAY_SIZE(SoftFPName); i < e; i++)
    {
        if (Name == SoftFPName[i])
            return true;
    }

    return false;
}

/*
 * isBaseArgument()
 *  Determine if the function argument and Ptr are alias.
 */
static bool isBaseArgument(Function *F, Value *Ptr)
{
    Operator *PtrOp = dyn_cast<Operator>(Ptr);
    if (PtrOp == NULL)
    {
        for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
                I != E; I++)
        {
            if (Ptr == I)
                return true;
        }
        return false;
    }

    if (PtrOp->getOpcode() == Instruction::BitCast
            || PtrOp->getOpcode() == Instruction::IntToPtr
            || PtrOp->getOpcode() == Instruction::GetElementPtr)
        return isBaseArgument(F, PtrOp->getOperand(0));

    return false;
}

/*
 * OptimizeSelection()
 *  Optimize the min/max helper functions by converting them to use the LLVM
 *  SelectInst instruction.
 */
int LLVMTranslator::OptimizeSelection(string Name, Value *I, Value *Base)
{
    LoadInst *LI = dyn_cast<LoadInst>(I);
    SelectInst *SI = dyn_cast<SelectInst>(Base);
    if (LI == NULL || SI == NULL)
        return 0;

    if (Name != "helper_minps" && Name != "helper_minss"
            && Name != "helper_minpd" && Name != "helper_minsd"
            && Name != "helper_maxps" && Name != "helper_maxss"
            && Name != "helper_maxpd" && Name != "helper_maxsd")
        return 0;

    CmpInst *CI = dyn_cast<CmpInst>(SI->getCondition());
    if (CI == NULL)
        DM->Error("%s: fatal error.\n", __func__);

    SelectInst *NewSI = SelectInst::Create(CI, CI->getOperand(0), CI->getOperand(1),
            "", LI);
    LI->replaceAllUsesWith(NewSI);

    toErase.push_back(LI);

    return 1;
}

/*
 * OptimizeSIMDFunc()
 *  Transform SIMD helper functions from scalar style to vector.
 *  Do not do transformation with (1) guest and host with different endians;
 *  (2) hosts not supporting vector instructions.
 */
int LLVMTranslator::OptimizeSIMDFunc(Function *F)
{
#if defined(NEET_BSWAP) || !defined(TCG_TARGET_I386)
    return 0;
#endif

#if defined(TARGET_I386)
    uint64_t xmm_offset = offsetof(CPUState, xmm_regs[0]);
    if (xmm_offset % 16 != 0)
        DM->Error("%s: fatal error on unaligned XMM register (%ld)\n",
                __func__, xmm_offset);

    string fname = getNameString(F);
    if (fname == "helper_paddb_xmm")       VF->Create(F, OP_ADD, Int8Ty, 16);
    else if (fname == "helper_paddw_xmm")  VF->Create(F, OP_ADD, Int16Ty, 8);
    else if (fname == "helper_paddl_xmm")  VF->Create(F, OP_ADD, Int32Ty, 4);
    else if (fname == "helper_paddq_xmm")  VF->Create(F, OP_ADD, Int64Ty, 2);
    else if (fname == "helper_psubb_xmm")  VF->Create(F, OP_SUB, Int8Ty, 16);
    else if (fname == "helper_psubw_xmm")  VF->Create(F, OP_SUB, Int16Ty, 8);
    else if (fname == "helper_psubl_xmm")  VF->Create(F, OP_SUB, Int32Ty, 4);
    else if (fname == "helper_psubq_xmm")  VF->Create(F, OP_SUB, Int64Ty, 2);
    else if (fname == "helper_pmullw_xmm") VF->Create(F, OP_MUL, Int16Ty, 8);
    else if (fname == "helper_pmulhuw_xmm")
    {
        VF->Init(F, Int16Ty, 8);
        VF->addOp(OP_MUL);
        VF->setConstantVec2(Int16Ty, 8, 16);
        VF->addOp(OP_LSHR);
        VF->Finalize();
    }
    else if (fname == "helper_por_xmm")    VF->Create(F, OP_OR, Int64Ty, 2);
    else if (fname == "helper_pxor_xmm")   VF->Create(F, OP_XOR, Int64Ty, 2);
    else if (fname == "helper_pand_xmm")   VF->Create(F, OP_AND, Int64Ty, 2);
    else if (fname == "helper_pandn_xmm")  VF->Create(F, OP_ANDN, Int64Ty, 2);
    else if (fname == "helper_pavgb_xmm")
    {
        VF->Init(F, Int8Ty, 16);
        VF->addOp(OP_ADD);
        VF->setConstantVec2(Int8Ty, 16, 1);
        VF->addOp(OP_ADD);
        VF->addOp(OP_LSHR);
        VF->Finalize();
    }
    else if (fname == "helper_pavgw_xmm")
    {
        VF->Init(F, Int16Ty, 8);
        VF->addOp(OP_ADD);
        VF->setConstantVec2(Int16Ty, 8, 1);
        VF->addOp(OP_ADD);
        VF->addOp(OP_LSHR);
        VF->Finalize();
    }
    else if (fname == "helper_addss")    VF->Create(F, OP_FADD, FloatTy, 1);
    else if (fname == "helper_addps")    VF->Create(F, OP_FADD, FloatTy, 4);
    else if (fname == "helper_addsd")    VF->Create(F, OP_FADD, DoubleTy, 1);
    else if (fname == "helper_addpd")    VF->Create(F, OP_FADD, DoubleTy, 2);
    else if (fname == "helper_subss")    VF->Create(F, OP_FSUB, FloatTy, 1);
    else if (fname == "helper_subps")    VF->Create(F, OP_FSUB, FloatTy, 4);
    else if (fname == "helper_subsd")    VF->Create(F, OP_FSUB, DoubleTy, 1);
    else if (fname == "helper_subpd")    VF->Create(F, OP_FSUB, DoubleTy, 2);
    else if (fname == "helper_mulss")    VF->Create(F, OP_FMUL, FloatTy, 1);
    else if (fname == "helper_mulps")    VF->Create(F, OP_FMUL, FloatTy, 4);
    else if (fname == "helper_mulsd")    VF->Create(F, OP_FMUL, DoubleTy, 1);
    else if (fname == "helper_mulpd")    VF->Create(F, OP_FMUL, DoubleTy, 2);
    else if (fname == "helper_divss")    VF->Create(F, OP_FDIV, FloatTy, 1);
    else if (fname == "helper_divps")    VF->Create(F, OP_FDIV, FloatTy, 4);
    else if (fname == "helper_divsd")    VF->Create(F, OP_FDIV, DoubleTy, 1);
    else if (fname == "helper_divpd")    VF->Create(F, OP_FDIV, DoubleTy, 2);
#endif

    return 1;
}

int LLVMTranslator::SplitState(Instruction *I, size_t Off, size_t Size)
{
    Function *F = I->getParent()->getParent();
    HelperInfo *Helper = HelperFuncs[F->getName()];
    StateTypeMap::iterator TI = --StateType.upper_bound(Off);

    bool Clobber = dyn_cast<StoreInst>(I);
    int Count = 0;
    size_t NextOff = Off;
    vector<size_t> AccessOff;
    vector<size_t> AccessSize;
    while(Size != 0)
    {
        size_t StateStart = TI->first;
        size_t StateSize = TI->second.second;
        size_t Remain = StateSize - (NextOff - StateStart);
        Size -= Remain;
        if (Size < 0)
            DM->Error("%s: fixme.\n", __func__);
        AccessOff.push_back(NextOff - Off);
        AccessSize.push_back(Remain);
        NextOff += Remain;
        Count++;
        ++TI;

        Helper->insertState(StateStart, Clobber);
    }

    if (LoadInst *LI = dyn_cast<LoadInst>(I))
    {
        Value *Addr = LI->getPointerOperand();
        Value *V = NULL;
        if (LI->getType()->getTypeID() != Type::IntegerTyID)
            DM->Error("%s: invalid load state.\n", __func__);

        for (int i = 0; i < Count; i++)
        {
            Off = AccessOff[i];
            Size = AccessSize[i];
            CONST Type *NewTy = IntegerType::get(Context, Size*8);
            Value *NewV;

            Value *NewAddr = new BitCastInst(Addr, Int8PtrTy, "", LI);
            NewAddr = GetElementPtrInst::Create(NewAddr, CONST32(Off), "", LI);
            NewAddr = new BitCastInst(NewAddr, PointerType::getUnqual(NewTy), "", LI);

            NewV = new LoadInst(NewAddr, "", false, LI);
            NewV = new ZExtInst(NewV, LI->getType(), "", LI);
            if (Off != 0)
                NewV = BinaryOperator::Create(Instruction::Shl, NewV, ConstantInt::get(LI->getType(), Off*8), "", LI);
            V = (V == NULL) ? NewV : BinaryOperator::Create(Instruction::Or, V, NewV, "", LI);
        }

        LI->replaceAllUsesWith(V);
    }
    else if (StoreInst *SI = dyn_cast<StoreInst>(I))
    {
        Value *Addr = SI->getPointerOperand();
        Value *V = SI->getOperand(0);
        if (V->getType()->getTypeID() != Type::IntegerTyID)
            DM->Error("%s: invalid store state.\n", __func__);

        for (int i = 0; i < Count; i++)
        {
            Off = AccessOff[i];
            Size = AccessSize[i];
            CONST Type *NewTy = IntegerType::get(Context, Size*8);
            Value *NewV = V;

            Value *NewAddr = new BitCastInst(Addr, Int8PtrTy, "", SI);
            NewAddr = GetElementPtrInst::Create(NewAddr, CONST32(Off), "", SI);
            NewAddr = new BitCastInst(NewAddr, PointerType::getUnqual(NewTy), "", SI);

            if (Off != 0)
                NewV = BinaryOperator::Create(Instruction::LShr, NewV, ConstantInt::get(V->getType(), Off*8), "", SI);
            NewV = new TruncInst(NewV, NewTy, "", SI);
            new StoreInst(NewV, NewAddr, false, SI);
        }
    }

    toErase.push_back(I);

    return 1;
}

/*
 * OptimizeHelperFunc()
 *  Eliminate redundant load/store within each 'basic block'.
 */
int LLVMTranslator::OptimizeHelperFunc(Function *F)
{
    HelperInfo *Helper = HelperFuncs[F->getName()];
    Helper->isInitialized = true;

    /* We don't want to inline helper function that contains loop, so take 
       a check of it. */
    bool hasLoop = HelperPasses->run(*F);
    if (hasLoop)
        Helper->mayConflict = true;

    for (Function::iterator FI = F->begin(), FE = F->end(); FI != FE; FI++)
    {
        for (BasicBlock::iterator BI = FI->begin(), BE = FI->end(); BI != BE; BI++)
        {
            if (LoadInst *LI = dyn_cast<LoadInst>(BI))
            {
                size_t Off = 0;
                Value *Base = getBaseWithConstantOffset(TD, LI->getPointerOperand(), Off);
                if (Base->getType() == BaseRegType)
                {
                    size_t Size = TD->getTypeSizeInBits(LI->getType())/8;
                    StateTypeMap::iterator TI = --StateType.upper_bound(Off);
                    size_t StateStart = TI->first;
                    size_t StateEnd = StateStart + TI->second.second;
                    if (Off + Size > StateEnd)
                        SplitState(LI, Off, Size);
                    else
                        Helper->insertState(StateStart, false);

#if defined(TARGET_I386)
                    if (Off == offsetof(CPUState, fpstt))
                        Helper->hasFP = true;
#endif
                }
                else if (dyn_cast<SelectInst>(Base))
                {
                    int ret = OptimizeSelection(F->getName(), LI, Base);
                    if (ret == 0)
                        Helper->mayConflict = true;
                }
                else if (isBaseArgument(F, Base))
                    Helper->mayConflictArg = true;
            }
            else if (StoreInst *SI = dyn_cast<StoreInst>(BI))
            {
                size_t Off = 0;
                Value *Base = getBaseWithConstantOffset(TD, SI->getPointerOperand(), Off);
                if (Base->getType() == BaseRegType)
                {
                    if (isStoreEIP(Off) == true)
                        DM->Error("%s: cannot handle store eip in helper function.\n", __func__);

                    size_t Size = TD->getTypeSizeInBits(SI->getOperand(0)->getType())/8;
                    StateTypeMap::iterator TI = --StateType.upper_bound(Off);
                    size_t StateStart = TI->first;
                    size_t StateEnd = StateStart + TI->second.second;
                    if (Off + Size > StateEnd)
                        SplitState(SI, Off, Size);
                    else
                        Helper->insertState(StateStart, true);
                }
                else if (dyn_cast<SelectInst>(Base))
                    Helper->mayConflict = true;
                else if (isBaseArgument(F, Base))
                    Helper->mayConflictArg = true;
            }
            else if (CallInst *CI = dyn_cast<CallInst>(BI))
            {
                if (CI->isInlineAsm())
                {
#if defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
                    char basereg_str[16] = {'\0'};
                    sprintf(basereg_str, "={%s}", BaseRegStr);
                    Value *AsmToReplace = InlineAsm::get(FunctionType::get(BaseRegType, false),
                            "", basereg_str, true);
                    CI->setCalledFunction(AsmToReplace);
#endif
                    continue;
                }

                Function *CalledFunc = CI->getCalledFunction();
                string Fname = CalledFunc->getName();
                if (CalledFunc->isIntrinsic())
                {
                    /* Remove dbg derived from `-g' compiler flag. */
                    if (Fname == "llvm.dbg.value")
                    {
                        toErase.push_back(CI);
                        continue;
                    }
                    
                    if (HybridMode == HYBRID_SERVER)
                        Helper->mayConflict = true;

                    if (HybridMode == HYBRID_CLIENT &&
                            EE->getPointerToGlobalIfAvailable(CalledFunc) == false)
                        return -1;
                }
                else if (isLibcall(Fname) == true)
                {
                    if (HybridMode == HYBRID_SERVER)
                        Helper->mayConflict = true;

                    /* Libcalls are always const function, mark it. */
                    Value *Meta[] = { };
                    CI->setMetadata("flags", CreateMDNode(Context, Meta, 0));
                }
                else if (isSoftFPcall(Fname) == true)
                {
                    Helper->mayConflict = true;
                    continue;

                    /* SoftFPcalls are always const function, mark it. */
                    Value *Meta[] = { };
                    CI->setMetadata("flags", CreateMDNode(Context, Meta, 0));
                }
#if defined(CONFIG_NET)
                else if (EE->getPointerToGlobalIfAvailable(CalledFunc) == false)
                    return -1;
#endif
                else
                {
                    Helper->hasNestedCall = true;
                    if (HelperFuncs.count(Fname.c_str()))
                    {
                        Value *NewCalledFunc = Mod->getNamedValue(Fname + "_noinline");
                        CI->setCalledFunction(NewCalledFunc);

                        HelperInfo *NestedHelper = HelperFuncs[Fname];
                        if (NestedHelper != Helper && NestedHelper->isInitialized)
                        {
                            Helper->insertState(NestedHelper);
                            Helper->hasNestedCall = false;
                        }
                    }
                    else if (EE->getPointerToGlobalIfAvailable(CalledFunc) == false)
                        return -1;
                }

                if (HybridMode == HYBRID_CLIENT)
                {
                    uintptr_t Addr = (uintptr_t)EE->getPointerToFunction(CalledFunc);
                    if (JSData.NestedFunc.count(Addr) == 0)
                        JSData.NestedFunc[Addr] =  Fname;
                }
            }
        }
    }

    ProcessErase(toErase);
    return 0;
}

/* 
 * CountCodeReductionForConstant()
 *  Figure out an approximation for how many instructions will be constant 
 *  folded if the specified value is constant.
 */
static unsigned CountCodeReductionForConstant(Value *V, CodeMetrics &Metrics)
{
    unsigned IndirectCallBonus;
#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30)
    IndirectCallBonus = InlineConstants::IndirectCallBonus;
#else
    IndirectCallBonus = -InlineConstants::IndirectCallThreshold;
#endif

    unsigned Reduction = 0;
    for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E;++UI)
    {
        User *U = *UI;
        if (isa<BranchInst>(U) || isa<SwitchInst>(U))
        {
            /* We will be able to eliminate all but one of the successors. */
            const TerminatorInst &TI = cast<TerminatorInst>(*U);
            const unsigned NumSucc = TI.getNumSuccessors();
            unsigned Instrs = 0;
            for (unsigned I = 0; I != NumSucc; ++I)
                Instrs += Metrics.NumBBInsts[TI.getSuccessor(I)];
            /* We don't know which blocks will be eliminated, so use the average size. */
            Reduction += InlineConstants::InstrCost*Instrs*(NumSucc-1)/NumSucc;
        }
        else if (CallInst *CI = dyn_cast<CallInst>(U))
        {
            /* Turning an indirect call into a direct call is a BIG win */
            if (CI->getCalledValue() == V)
                Reduction += IndirectCallBonus;
        }
        else if (InvokeInst *II = dyn_cast<InvokeInst>(U))
        {
            /* Turning an indirect call into a direct call is a BIG win */
            if (II->getCalledValue() == V)
                Reduction += IndirectCallBonus;
        }
        else
        {
            Instruction &Inst = cast<Instruction>(*U);
            
            if (Inst.mayReadFromMemory() || Inst.mayHaveSideEffects() ||
                    isa<AllocaInst>(Inst))
                continue;
            
            bool AllOperandsConstant = true;
            for (unsigned i = 0, e = Inst.getNumOperands(); i != e; ++i)
                if (!isa<Constant>(Inst.getOperand(i)) && Inst.getOperand(i) != V)
                {
                    AllOperandsConstant = false;
                    break;
                }
            
            if (AllOperandsConstant)
            {
                /* We will get to remove this instruction... */
                Reduction += InlineConstants::InstrCost;
                Reduction += CountCodeReductionForConstant(&Inst, Metrics);
            }
        }
    }
    return Reduction;
}

/* 
 * CountCodeReductionForAlloca()
 *  Figure out an approximation of how much smaller the function will be if 
 *  it is inlined into a context where an argument becomes an alloca.
 */
static unsigned CountCodeReductionForAlloca(Value *V) 
{
    if (!V->getType()->isPointerTy()) return 0; 

    unsigned Reduction = 0;
    for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E;++UI)
    {
        Instruction *I = cast<Instruction>(*UI);
        if (isa<LoadInst>(I) || isa<StoreInst>(I))
            Reduction += InlineConstants::InstrCost;
        else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I))
        {
            /* If the GEP has variable indices, we won't be able to do much with it. */
            if (GEP->hasAllConstantIndices())
                Reduction += CountCodeReductionForAlloca(GEP);
        }
        else if (BitCastInst *BCI = dyn_cast<BitCastInst>(I))
        {
            /* Track pointer through bitcasts. */
            Reduction += CountCodeReductionForAlloca(BCI);
        }
        else
            return 0;
    }
    
    return Reduction;
}

/*
 * SkipStoreEIP()
 *  Return true if the given offset is accociated to EIP.
 */
bool isStoreEIP(size_t Off)
{
    size_t EIPOffset;

#if defined(TARGET_ALPHA)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_ARM)
    EIPOffset = offsetof(CPUState, regs[15]);
#elif defined(TARGET_CRIS)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_I386)
    EIPOffset = offsetof(CPUState, eip);
#elif defined(TARGET_M68K)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_MICROBLAZE)
    EIPOffset = offsetof(CPUState, sregs[0]);
#elif defined(TARGET_MIPS)
    EIPOffset = offsetof(CPUState, active_tc.PC);
#elif defined(TARGET_PPC)
    EIPOffset = offsetof(CPUState, nip);
#elif defined(TARGET_SH4)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_SPARC)
    size_t EIPOffset2;
    EIPOffset = offsetof(CPUState, pc);
    EIPOffset2 = offsetof(CPUState, npc);
#else
#error "unsupported processor type"
#endif

#if defined(TARGET_ALPHA) || defined(TARGET_ARM) || defined(TARGET_CRIS) \
    || defined(TARGET_I386) || defined(TARGET_M68K) || defined(TARGET_MICROBLAZE) \
    || defined(TARGET_MIPS) || defined(TARGET_PPC) || defined(TARGET_SH4)
    return (Off >= EIPOffset && Off < EIPOffset + sizeof(target_ulong));
#elif defined(TARGET_SPARC)
    return ((Off >= EIPOffset && Off < EIPOffset + sizeof(target_ulong)) ||
            (Off >= EIPOffset2 && Off < EIPOffset2 + sizeof(target_ulong)));
#endif
}

/*
 * SaveTraceEIP()
 *  Trace building requires store EIP instruction to link basic blocks. 
 *  But in some archirecture, EIP is promoted to register and we need to 
 *  regenerate the store EIP instruction.
 */
void IRFactory::SaveTraceEIP(Instruction *InsertBefore)
{
#if defined(TARGET_ALPHA)
#define EIP_NAME    "pc"
#elif defined(TARGET_ARM)
#define EIP_NAME    "pc"
#elif defined(TARGET_CRIS)
#define EIP_NAME    "pc"
#elif defined(TARGET_I386)
#elif defined(TARGET_M68K)
#define EIP_NAME    "PC"
#elif defined(TARGET_MICROBLAZE)
#define EIP_NAME    "rpc"
#elif defined(TARGET_MIPS)
#define EIP_NAME    "PC"
#elif defined(TARGET_PPC)
#define EIP_NAME    "nip"
#elif defined(TARGET_SH4)
#define EIP_NAME    "PC"
#elif defined(TARGET_SPARC)
#define EIP_NAME    "pc"
#define EIP_NAME2   "npc"
#else
#error "unsupported processor type"
#endif

#if defined(TARGET_ALPHA) || defined(TARGET_ARM) || defined(TARGET_CRIS) \
    || defined(TARGET_M68K) || defined(TARGET_MICROBLAZE) || defined(TARGET_MIPS) \
    || defined(TARGET_PPC)  || defined(TARGET_SH4)
    for (int i = 0; i < nb_globals; i++)
    {
        Register *reg = &Reg[i];
        if (reg->isReg() && !strcmp(reg->Name.c_str(), EIP_NAME))
        {
            if (reg->isDirty())
            {
                uint64_t size = TD->getTypeSizeInBits(
                        reg->getData()->getType());
                
                Value *Addr = LookupAddr(reg, getPointerTy(size));
                new StoreInst(reg->getData(), Addr, false, InsertBefore);
            }
            reg->Demote();
        }
    }
#elif defined(TARGET_SPARC)
    for (int i = 0; i < nb_globals; i++)
    {
        Register *reg = &Reg[i];
        if (reg->isReg() && (!strcmp(reg->Name, EIP_NAME) ||
                    !strcmp(reg->Name, EIP_NAME2)))
        {
            if (reg->isDirty())
            {
                uint64_t size = TD->getTypeSizeInBits(
                        reg->getData()->getType());
                
                Value *Addr = LookupAddr(reg, getPointerTy(size));
                new StoreInst(reg->getData(), Addr, false, InsertBefore);
            }
            reg->Demote();
        }
    }
#endif
}

/*
 * TraceCheckStoreEIP()
 *  Check if store EIP instruction is storing a legal EIP.
 */
void IRFactory::TraceCheckStoreEIP(StoreInst *SI)
{
    size_t Off = 0;
    size_t EIPOffset;

#if defined(TARGET_ALPHA)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_ARM)
    EIPOffset = offsetof(CPUState, regs[15]);
#elif defined(TARGET_CRIS)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_I386)
    EIPOffset = offsetof(CPUState, eip);
#elif defined(TARGET_M68K)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_MICROBLAZE)
    EIPOffset = offsetof(CPUState, sregs[0]);
#elif defined(TARGET_MIPS)
    EIPOffset = offsetof(CPUState, active_tc.PC);
#elif defined(TARGET_PPC)
    EIPOffset = offsetof(CPUState, nip);
#elif defined(TARGET_SH4)
    EIPOffset = offsetof(CPUState, pc);
#elif defined(TARGET_SPARC)
    size_t EIPOffset2;
    EIPOffset = offsetof(CPUState, pc);
    EIPOffset2 = offsetof(CPUState, npc);
#else
#error "unsupported processor type"
#endif

    getBaseWithConstantOffset(TD, SI->getPointerOperand(), Off);

#if defined(TARGET_ALPHA) || defined(TARGET_ARM) || defined(TARGET_CRIS) \
    || defined(TARGET_I386) || defined(TARGET_M68K) || defined(TARGET_MICROBLAZE) \
    || defined(TARGET_MIPS) || defined(TARGET_PPC) || defined(TARGET_SH4)
    if (Off != EIPOffset)
    {
        Func->dump();
        DM->Error("%s: invalid store offset %ld (%ld).\n", __func__, Off,
                EIPOffset);
    }
#elif defined(TARGET_SPARC)
    if (Off != EIPOffset && Off != EIPOffset2)
    {
        Func->dump();
        DM->Error("%s: invalid store offset %ld (%ld, %ld).\n", __func__, Off,
                EIPOffset, EIPOffset2);
    }
#endif
}

/*
 * DeleteDeadInstructions()
 *  Remove an instruction from a basic block. Also delete any instrution
 *  used by this instruction if it is no longer being used.
 */
void DeleteDeadInstructions(Instruction *I)
{
    IVec NowDeadInsts;

    NowDeadInsts.push_back(I);
    while (!NowDeadInsts.empty())
    {
        Instruction *DeadInst = NowDeadInsts.back();
        NowDeadInsts.pop_back();
        
        for (unsigned op = 0, e = DeadInst->getNumOperands(); op != e; ++op)
        {
            Value *Op = DeadInst->getOperand(op);
            DeadInst->setOperand(op, 0);
            
            if (!Op->use_empty()) continue;
            
            /* If this operand just became dead, add it to the dead list. */
            if (Instruction *OpI = dyn_cast<Instruction>(Op))
#if 0
                if (isInstructionTriviallyDead(OpI))
#endif
                    NowDeadInsts.push_back(OpI);
        }
        
        DeadInst->eraseFromParent();
    }
}

/*
 * getBaseWithConstantOffset()
 *  Return base address and offset of a memory access pointer.
 */
Value *getBaseWithConstantOffset(const TargetData *TD, Value *Ptr,
        size_t &Offset)
{
    Operator *PtrOp = dyn_cast<Operator>(Ptr);
    if (PtrOp == 0)
        return Ptr;
    
    if (PtrOp->getOpcode() == Instruction::BitCast ||
            PtrOp->getOpcode() == Instruction::IntToPtr)
        return getBaseWithConstantOffset(TD, PtrOp->getOperand(0), Offset);
    
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
    
    return getBaseWithConstantOffset(TD, GEP->getPointerOperand(), Offset);
}

/*
 * getBaseWithConstantOffset()
 *  Return base address and offset of a memory access pointer.
 */
Value *getBaseWithPossibleOffset(const TargetData *TD, Value *Ptr,
        size_t &Offset, Value **LastPtr)
{
    Operator *PtrOp = dyn_cast<Operator>(Ptr);
    if (PtrOp == 0)
        return Ptr;
    
    if (PtrOp->getOpcode() == Instruction::BitCast ||
            PtrOp->getOpcode() == Instruction::IntToPtr)
        return getBaseWithPossibleOffset(TD, PtrOp->getOperand(0), Offset, LastPtr);
    
    GEPOperator *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (GEP == 0)
        return Ptr;
    
    gep_type_iterator GTI = gep_type_begin(GEP);
    for (User::op_iterator I = GEP->idx_begin(), E = GEP->idx_end(); I != E;
            ++I, ++GTI)
    {
        /* If this is a GEP with non-constant indices, stop here. */
        ConstantInt *OpC = dyn_cast<ConstantInt>(*I);
        if (OpC == NULL)
        {
            *LastPtr = *I;
            return PtrOp->getOperand(0);
        }
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
    
    return getBaseWithPossibleOffset(TD, GEP->getPointerOperand(), Offset, LastPtr);
}

/*
 * ProcessErase()
 *  Perform instruction removal from the parent container.
 */
void ProcessErase(IVec &toErase)
{
    for (IVec::iterator I = toErase.begin(), E = toErase.end(); I != E; ++I)
        DeleteDeadInstructions(*I);
    
    toErase.clear();
}

/*
 * ResolvePC()
 *  Get the real PC by combining PC:CS.
 */
void ResolvePC(target_ulong &PC, target_ulong &CS)
{
    PC += CS;
}

/*
 * llvm_out32()
 *  Emit one 32-bit value in the code cache.
 */
static inline void llvm_out32(TCGContext *s, uint32_t v)
{
    *(uint32_t *)s->code_ptr = v;
    s->code_ptr += 4;
}

/*
 * gen_target_stub()
 *  Generate a trampoline, which jumps to the epilog, at the start of the
 *  optimized code cache.
 */
int gen_target_stub(void *Stub)
{
    int StubSize = 0;

#if defined(TCG_TARGET_I386)
    llvm_tb_ret_addr = tb_ret_addr;
    StubSize = 0;
#elif defined(TCG_TARGET_ARM)
    tcg_ctx_env->code_ptr = (uint8_t*)Stub;
    llvm_tb_ret_addr = tcg_ctx_env->code_ptr;

    /* mov pc, tb_ret_addr */
    llvm_out32(tcg_ctx_env, (COND_AL << 28) | 0x05100000 |
            (TCG_REG_PC << 16) | (TCG_REG_PC << 12) | ((4) & 0xfff));
    llvm_out32(tcg_ctx_env, (uintptr_t)tb_ret_addr);
    StubSize = 8;
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    llvm_tb_ret_addr = (uint8_t*)Stub;
    patch_jmp((unsigned long)Stub, (unsigned long)tb_ret_addr);
    StubSize = 28;
#endif

#if 0
    if (StubSize > 0)
    {
        fprintf(stderr, "%s: create jump stub [size=%d]\n", __func__, StubSize);
        disas(stderr, (uint8_t *)Stub, StubSize);
        fprintf(stderr, "\n");
    }
#endif
    if (StubSize)
        flush_icache_range((unsigned long)Stub, (unsigned long)Stub + StubSize);

    return StubSize;
}

/*
 * ResetPatchJmp()
 *  Replace jump patching region with NOP and return aligned patching address.
 */
uintptr_t ResetPatchJmp(void *Code, size_t Size, uintptr_t Addr, MDNode *MD,
        int &JmpOff, int &NextOff, int Type)
{
#if defined(TCG_TARGET_I386)
    int i = 0;
    uint8_t *p = (uint8_t *)Addr;
    int count = 8; /* 4 trap instruciotns (ud2) */

    while (i < count) p[i++] = 0x90;
    JmpOff = 0;
    NextOff = 5;

    /* Arrange the operand part of the jump instruction to 4-byte aligned
     * because we must make sure the patching is atomic. */
    while ((Addr + 1) % 4)
        Addr++;
#elif defined(TCG_TARGET_ARM)
    if (Type == PATCH_DIRECT_JUMP)
        NextOff = 8;
    else 
    {
        vector<tcg_target_ulong> &CallTable = Env->getCallTable();
        int CallIdx = static_cast<ConstantInt*>(MD->getOperand(2))->getZExtValue();
        tcg_target_ulong arg = CallTable[CallIdx];

        bool flag = false;
        uint8_t *CPoolAddr = NULL;
        for (int i = 0; i < (int)Size; i += 4)
        {
            CPoolAddr = (uint8_t *)Code + i;
            if (*(uint32_t *)CPoolAddr == arg)
            {
                flag = true;
                break;
            }
        }

        if (flag == false)
        {
            fprintf(stderr, "OUT: [size=%d]\n", (int)Size);
            disas(stderr, (uint8_t *)Code, Size);
            fprintf(stderr, "\n");
            DM->Error("%s: cannot find constant pool. 0x%x arg=%x idx=%d\n",
                    __func__, Addr, arg, CallIdx);
        }

        flag = false;
        uint8_t *OpAddr = NULL;
        uint32_t Op = 0;
        for (int i = 0; i < (int)Size; i += 4)
        {
            OpAddr = (uint8_t *)Code + i;
            Op = *(uint32_t *)(OpAddr);
            uint32_t Opcode = Op >> 24 & 0xf;
            uint32_t Rn = Op >> 16 & 0xf;
            int32_t Off = Op & 0xfff;
            if ((Op & 0x00800000) == 0)
                Off = -Off;
            Off += 8;
            if (Opcode == 5 && Rn == 15 && OpAddr + Off == CPoolAddr)
            {
                flag = true;
                break;
            }
        }

        if (flag == false)
            DM->Error("%s: cannot find load operation.\n", __func__);

        tcg_ctx_env->code_ptr = OpAddr;
        llvm_out32(tcg_ctx_env, (Op & 0xf0000000) | 0x01a00000);

        Op = *(uint32_t *)(OpAddr + 4);
        llvm_out32(tcg_ctx_env, (Op & 0xf0000000) | 0x01a00000);

        *(uint32_t *)CPoolAddr = Addr + 4;
        JmpOff = (intptr_t)CPoolAddr - (intptr_t)Addr;
        NextOff = 4;
    }
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    JmpOff = 0;
    NextOff = 16;

    vector<uint32_t> Value;
    uint32_t *p = (uint32_t *)Addr;
    int last = -1;
    uint32_t mask1 = ~(31U << 21);
    uint32_t mask2 = ~((1 << 8)-1);
    for (int i = 0; ; i++)
    {
        if ((p[i] & mask1 & mask2) == 0x80000000)
            continue;
        if ((p[i] >> 26) == 0x24)
        {
            last = i;
            break;
        }
        else
            Value.push_back(p[i]);
    }
    for (int i = 0, e = Value.size(); i < e; i++)
        p[i] = Value[i];
    for (int i = Value.size(); i < last; i++)
        p[i] = 0x0;
    Addr += 4 * Value.size();
#else
#error "unsupported processor type"
#endif
    
    return Addr;
}

/*
 * ResetPatchReturn()
 *  Replace jump patching region with NOP and return aligned patching address.
 */
uintptr_t ResetPatchReturn(uintptr_t Addr, unsigned long RetVal)
{
#if defined(TCG_TARGET_I386)
    int i = 0;
    uint8_t *p = (uint8_t *)Addr;

    if (OptLevel == CodeGenOpt::None)
    {
        p--;
        if (*p != 0xc3)
            DM->Error("%s: invalid opcode %d\n", __func__, *p);
        while (i < 5) p[i++] = 0x90;
        Addr = (uintptr_t)p;
    }
    else
    {
        while (i < 10) p[i++] = 0x90;
        tcg_ctx_env->code_ptr = (uint8_t*)Addr;
        if (RetVal != (uint32_t)RetVal)
            DM->Error("%s: invalid return value size %d\n", __func__,
                    sizeof(unsigned long));
        
        tcg_out_movi(tcg_ctx_env, TCG_TYPE_PTR, TCG_REG_EAX, RetVal);
        Addr = (uintptr_t)tcg_ctx_env->code_ptr;
    }
#elif defined(TCG_TARGET_ARM)
    uint32_t Op = *(uint32_t *)Addr & 0x0ffffff0;
    if (Op != 0x012fff10)
    {
        /* handle patch point that points to 'mov r0, rn'
         *   mov      r0, r4
         *   bx       lr
         */
        Addr += 4;
        Op = *(uint32_t *)Addr & 0x0ffffff0;
        if (Op != 0x012fff10)
            DM->Error("%s: cannot find patch point.\n", __func__);
    }
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    uint32_t *p = (uint32_t *)Addr;
    if (*p != 0x4e800020)
    {
        p++;
        if (*p != 0x4e800020)
            DM->Error("%s: cannot find patch point.\n", __func__);
        Addr += 4;
    }
#endif
    
    return Addr;
}

/*
 * ResetPatchIBChain()
 *  Patch an IB chain point.
 */
uintptr_t ResetPatchIBChain(uintptr_t Addr, int &JmpOff, IBChainInfo *ChainInfo)
{
#if defined(TCG_TARGET_I386)
    JmpOff = 0;
    Addr -= 9;
    uint8_t *p = (uint8_t *)Addr;
    if (p[0] != 0xff || p[2] != 0x8a)
        DM->Error("%s: cannot find IB chain patch point.\n", __func__);

    int i = 0, count = 9;
    while (i < count) p[i++] = 0x90;
    if (Addr % 2)
        Addr++;

    ChainInfo->setPatchAddr(Addr);
#elif defined(TCG_TARGET_ARM)
    uint8_t *OpAddr = (uint8_t *)Addr;
    uint32_t Op = *(uint32_t *)(OpAddr);
    int32_t Off = Op & 0xfff;
    if ((Op & 0x00800000) == 0)
        Off = -Off;
    Off += 8;

    JmpOff = Off;

    ChainInfo->setPatchAddr(Addr+JmpOff);
#endif
    return Addr;
}

/*
 * RecordDirectLink()
 *  Record the offset to the emitted host code region for block chaining.
 *
 */
void RecordDirectLink(TranslationBlock *TB, int Idx, uintptr_t Disp)
{
#if defined(TCG_TARGET_I386)
    TB->tb_jmp_offset[Idx] = Disp + 1;
    TB->tb_next_offset[Idx] = TB->tb_jmp_offset[Idx] + 4;
#elif defined(TCG_TARGET_ARM)
    TB->tb_jmp_offset[Idx] = Disp;
    TB->tb_next_offset[Idx] = TB->tb_jmp_offset[Idx] + 4;
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    TB->tb_jmp_offset[Idx] = Disp;
    TB->tb_next_offset[Idx] = TB->tb_jmp_offset[Idx] + 16;
#else
#error "unsupported processor type"
#endif
}

/*
 * PatchExternalSymbol()
 *  Patch the address of external symbols (used only for Client/Server model).
 */
void PatchExternalSymbol(uintptr_t Code, JITSyncData &JSData,
        vector< pair<uint32_t,int> > &ExternalSymbol)
{
    for (int i = 0, e = ExternalSymbol.size(); i < e; i++)
    {
        int idx = ExternalSymbol[i].second;

        tcg_target_long Addr = JSData.ExternalSymbol[idx];
        if (Addr == 0)
            DM->Error("%s: internal error.\n", __func__);

        tcg_ctx_env->code_ptr = (uint8_t *)Code + ExternalSymbol[i].first;
        llvm_out32(tcg_ctx_env, Addr);
    }
}

/*
 * PatchMovConstPool()
 *  Patch the address of constant pools (used only for Clinet/Server model).
 */
void PatchMovConstPool(uintptr_t Code, int Off, int CPoolOff)
{
#if defined(TCG_TARGET_I386)
    intptr_t CPoolAddr = (intptr_t)Code + CPoolOff;
    tcg_ctx_env->code_ptr = (uint8_t *)Code + Off + 2;  /* plus 2 to skip opcode */
    llvm_out32(tcg_ctx_env, CPoolAddr);
    llvm_out32(tcg_ctx_env, CPoolAddr >> 31 >> 1);
#elif defined(TCG_TARGET_ARM)
    DM->Error("%s: fixme.\n", __func__);
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    DM->Error("%s: fixme.\n", __func__);
#else
#error "unsupported processor type"
#endif
}

/*
 * PatchCodeShort()
 *  Patch a jump.
 */
void PatchCodeShort(TargetJITInfo &TJI, uintptr_t Old, uintptr_t New)
{
#if defined(TCG_TARGET_I386)
#ifdef ASSERT
    int64_t disp = (New > Old) ? New - Old : Old - New;
    if (disp != (int32_t)disp)
        DM->Error("%s: jump address is too long.\n", __func__);
#endif
    TJI.replaceMachineCodeForFunction((void *)Old, (void *)New);
#elif defined(TCG_TARGET_ARM)
    tcg_ctx_env->code_ptr = (uint8_t *)Old;
    int32_t offset = New - Old;
    if (!(offset - 8 < 0x01fffffd && offset - 8 > -0x01fffffd))
        DM->Error("%s: jump address is too long for short jump.\n", __func__);
    
    uint32_t Cond = *(uint32_t *)Old & 0xf0000000;
    llvm_out32(tcg_ctx_env, Cond | 0x0a000000 |
            (((offset - 8) >> 2) & 0x00ffffff));
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    TJI.replaceMachineCodeForFunction((void *)Old, (void *)New);
#else
#error "unsupported processor type"
#endif
}

/*
 * PatchCode()
 *  Patch a jump.
 */
void PatchCode(TargetJITInfo &TJI, uintptr_t Old, uintptr_t New, int JmpOff)
{
#if defined(TCG_TARGET_I386)
#ifdef ASSERT
    int64_t disp = (New > Old) ? New - Old : Old - New;
    if (disp != (int32_t)disp)
        DM->Error("%s: jump address is too long.\n", __func__);
#endif
    TJI.replaceMachineCodeForFunction((void *)Old, (void *)New);
#elif defined(TCG_TARGET_ARM)
    uint32_t Op = *(uint32_t *)Old;
    uint32_t Cond = Op & 0xf0000000;
    int32_t Off = JmpOff - 8;
    
    tcg_ctx_env->code_ptr = (uint8_t *)Old;
    if (Off >= 0)
        llvm_out32(tcg_ctx_env, Cond | 0x05900000 |
                (TCG_REG_PC << 16) | (TCG_REG_PC << 12) | (Off & 0xfff));
    else
        llvm_out32(tcg_ctx_env, Cond | 0x05100000 |
                (TCG_REG_PC << 16) | (TCG_REG_PC << 12) | ((-Off) & 0xfff));
    
    tcg_ctx_env->code_ptr = (uint8_t *)Old + JmpOff;
    llvm_out32(tcg_ctx_env, New);
#elif defined(TCG_TARGET_PPC) || defined(TCG_TARGET_PPC64)
    TJI.replaceMachineCodeForFunction((void *)Old, (void *)New);
#else
#error "unsupported processor type"
#endif
}

/*
 * JIT Event
 */
EventListener::EventListener(TargetJITInfo &tji, CallBackInfo &callback) : TJI(tji), CallBack(callback),
    JSData(*CallBack.JSData)
{
}

/*
 * HandleEmittedCodeInfo()
 *  Set patch rules for client/server mode.
 */
void EventListener::HandleEmittedCodeInfo(void *Code, const EmittedCodeInfo &ECInfo)
{
    PatchInfo &Patch = Env->getPatch();

    const ESMap &ExternalSymbol = ECInfo.ExternalSymbol;
    for (ESMap::const_iterator I = ExternalSymbol.begin(), E = ExternalSymbol.end();
            I != E; I++)
    {
        if (JSData.ExternalSymbolIdx.count(I->second) == 0)
            DM->Error("%s: fatal error.\n", __func__);

        int idx = JSData.ExternalSymbolIdx[I->second];
        Patch.ExternalSymbol.push_back(make_pair(I->first, idx));
    }

    const ConstPoolMap &ConstPool = ECInfo.ConstantPoolEntry;
    if (ConstPool.size() == 0)
        return;

    int ConstPoolSize = 0;
    for (ConstPoolMap::const_iterator I = ConstPool.begin(), E = ConstPool.end();
            I != E; I++)
    {
        if (I->second >= (uintptr_t)Code)
            DM->Error("%s: internal error.\n", __func__);

        uint32_t Off = I->first - (uintptr_t)Code;
        int ConstPoolOff = (intptr_t)I->second - (intptr_t)Code;
        if (ConstPoolOff > 0)
            DM->Error("%s: fix me.\n", __func__);

        Patch.ConstPool.push_back(make_pair(Off, ConstPoolOff));
        if (abs(ConstPoolOff) > ConstPoolSize)
            ConstPoolSize = abs(ConstPoolOff);
    }
    Patch.ConstPoolSize = ConstPoolSize;
}

void EventListener::NotifyFunctionEmitted(const Function &F,
        void *Code, size_t Size,
        const EmittedFunctionDetails &Details)
{
    int JmpOff, NextOff;
    int NumDebugLoc = Details.LineStarts.size();
    TraceChainMap &TraceChain = Env->getTraceChain();
    ChainVec &ChainPoint = Env->getChainPoint();
    PatchInfo &Patch = Env->getPatch();
    
    if (CallBack.Func == NULL)
        return;

    if (Code == NULL || Size == 0)
        DM->Error("%s: JIT fatal error at pc 0x%s\n", __func__, getNameCString(&F));

    CallBack.Code = (uint8_t *)Code;
    CallBack.Size = Size;

    if (HybridMode == HYBRID_SERVER)
    {
        Patch.toExit.clear();
        Patch.toExec.clear();
        Patch.ConstPool.clear();
        Patch.ConstPoolSize = 0;
    }

    for (int i = 0; i < NumDebugLoc; i++)
    {
        uintptr_t Addr = Details.LineStarts[i].Address;
        MDNode *MD = Details.LineStarts[i].Loc.getScope(F.getContext());
        int PatchType = static_cast<ConstantInt*>(MD->getOperand(1))->getZExtValue();
        
        if (PatchType == PATCH_DUMMY) ;
        else if (PatchType == PATCH_EXIT_TB)
        {
            unsigned long RetVal = static_cast<ConstantInt*>(MD->getOperand(2))->getZExtValue();
            Addr = ResetPatchReturn(Addr, RetVal);

            if (HybridMode == HYBRID_SERVER)
                Patch.toExit.push_back(Addr - (uintptr_t)Code);
            else
                PatchCodeShort(TJI, Addr, (uintptr_t)llvm_tb_ret_addr);
        }
        else if (PatchType == PATCH_DIRECT_JUMP)
        {
            int jmp_idx = static_cast<ConstantInt*>(MD->getOperand(3))->getZExtValue();
            Addr = ResetPatchJmp(Code, Size, Addr, MD, JmpOff, NextOff, PATCH_DIRECT_JUMP);
            RecordDirectLink(CallBack.TB, jmp_idx, Addr - (uintptr_t)Code);
            PatchCodeShort(TJI, Addr, Addr + NextOff);
        }
        else if (PatchType == PATCH_TRACE_BLOCK_CHAINING)
        {
            unsigned long Key = static_cast<ConstantInt*>(MD->getOperand(3))->getZExtValue();
            target_ulong PC = static_cast<ConstantInt*>(MD->getOperand(4))->getZExtValue();
            Addr = ResetPatchJmp(Code, Size, Addr, MD, JmpOff, NextOff, PATCH_TRACE_BLOCK_CHAINING);

            if (Env->is_multithreaded()) llvm_spin_lock(&llvm_global_lock);
            TraceChain[PC].push_back(Addr + JmpOff);
            if (Env->is_multithreaded()) llvm_spin_unlock(&llvm_global_lock);

            llvm_spin_lock(&llvm_interface_lock);
            ChainPoint[Key] = Addr + JmpOff;
            llvm_spin_unlock(&llvm_interface_lock);

            if (HybridMode == HYBRID_SERVER)
                Patch.toExec.push_back(ChainInfo(Addr - (uintptr_t)Code, NextOff, JmpOff, Key, PC));
            else
                PatchCode(TJI, Addr, Addr + NextOff, JmpOff);
        }
        else if (PatchType == PATCH_IBCHAIN)
        {
            IBChainInfo *ChainInfo = (IBChainInfo*)static_cast<ConstantInt*>(MD->getOperand(2))->getZExtValue();
            Addr = ResetPatchIBChain(Addr, JmpOff, ChainInfo);
            PatchCode(TJI, Addr, (uintptr_t)ibchain_ret_addr, JmpOff);
        }
        else if (PatchType == PATCH_CALL)
        {
            int idx = static_cast<ConstantInt*>(MD->getOperand(2))->getZExtValue();
            map<long,int> &CallPos = CallBack.CI->getCallPos();
            CallPos[Addr-(uintptr_t)Code] = idx;
        }
#if 0
        else
            DM->Error("%s: invalid patch type.\n", __func__);
#endif
    }

    if (HybridMode == HYBRID_SERVER)
        HandleEmittedCodeInfo(Code, Details.MF->ECInfo);

    /* Flush instruction cache */
    flush_icache_range((unsigned long)Code, (unsigned long)Code + Size);

    if (DM->getDebug() & DEBUG_OUTASM)
    {
        llvm_spin_lock(&llvm_debug_lock);
        fprintf(stderr, "OUT: [size=%d]\n", (int)Size);
        disas(stderr, Code, (int)Size);
        fprintf(stderr, "\n");
        llvm_spin_unlock(&llvm_debug_lock);
    }
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
