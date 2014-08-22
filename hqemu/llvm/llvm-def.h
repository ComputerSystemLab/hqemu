/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_DEF_H
#define __LLVM_DEF_H

#if defined(LLVM_V28)
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetSelect.h"
#include "llvm/System/Host.h"
#include "llvm/System/DynamicLibrary.h"
typedef llvm::ValueMap<const llvm::Value*, llvm::Value*> ValueToValueMapTy;
#elif defined(LLVM_V29)
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetSelect.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/DynamicLibrary.h"
#elif defined(LLVM_V30)
#include "llvm/Target/TargetData.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/DynamicLibrary.h"
#elif defined(LLVM_V31)
#include "llvm/Target/TargetData.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Analysis/ValueTracking.h"
#elif defined(LLVM_V32svn)
#include "llvm/DataLayout.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Analysis/ValueTracking.h"
#define TargetData DataLayout
#endif

#if defined(LLVM_V28) || defined(LLVM_V29)
#define CONST const
#else
#define CONST
#endif

#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30) || defined(LLVM_V31)
#define FNATTR(x)   Attribute::x
#else
#define FNATTR(x)   Attributes::x
#endif

using namespace llvm;
using namespace std;

/* Return the C++ string of the Value V. */
static inline std::string getNameString(const Value *V)
{
#if defined(LLVM_V28) || defined(LLVM_V29) || defined(LLVM_V30)
    return V->getNameStr();
#else
    return V->getName().str();
#endif
}

/* Return the C string of the Value V. */
static inline const char *getNameCString(const Value *V)
{
    return getNameString(V).c_str();
}

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
