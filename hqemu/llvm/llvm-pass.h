/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_PASS_H
#define __LLVM_PASS_H

#include "llvm/LLVMContext.h"

using namespace llvm;

class IRFactory;

FunctionPass *createHelperPass();
FunctionPass *createStateMappingPass(IRFactory *IF);
FunctionPass *createProfilePass(IRFactory *IF);
FunctionPass *createOptimizeGuestMemoryPass(IRFactory *IF);
FunctionPass *createHoistCommonCodePass();
FunctionPass *createPHItoSelectPass();

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
