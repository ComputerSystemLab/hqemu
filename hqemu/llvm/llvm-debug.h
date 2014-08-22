/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_DEBUG_H
#define __LLVM_DEBUG_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include "utils.h"

#define DEBUG_NONE      (uint64_t)0
#define DEBUG_LLVM      ((uint64_t)1 << 0)
#define DEBUG_INASM     ((uint64_t)1 << 1)
#define DEBUG_OP        ((uint64_t)1 << 2)
#define DEBUG_OUTASM    ((uint64_t)1 << 3)
#define DEBUG_IR        ((uint64_t)1 << 4)
#define DEBUG_ENTRY     ((uint64_t)1 << 5)
#define DEBUG_DBO       ((uint64_t)1 << 6)
#define DEBUG_VERIFY    ((uint64_t)1 << 7)
#define DEBUG_ASM       (DEBUG_INASM | DEBUG_OP | DEBUG_OUTASM)
#define DEBUG_DEBUG     (DEBUG_LLVM | DEBUG_IR | DEBUG_OUTASM)
#define DEBUG_ALL       (DEBUG_LLVM | DEBUG_INASM | DEBUG_OP | DEBUG_OUTASM \
                            | DEBUG_IR | DEBUG_ENTRY | DEBUG_DBO | DEBUG_VERIFY)
#define DEBUG_NUM       12

/*
 * LLVMDebug provides the facilities to debug the results of the LLVM
 * translation based on the debug level.
 */
class LLVMDebug
{
private:
    uint64_t Mode;        /* The debug level */
    FILE *FP;             /* The handle for the output */
    char Buf[32];         /* The buffer containing the timestamp */
    struct timeval Start; /* The startup time of the DBT */
    llvm_lock_t Lock;     /* The lock for writting out the output */

    /* Print - Write the output including the elapse time to FP. */
    void Print(char *str)
    {
        struct timeval tv;
        
        gettimeofday(&tv, 0);
        timersub(&tv, &Start, &tv);
        strftime(Buf, 32, "[%H:%M:%S", gmtime(&tv.tv_sec));
        sprintf(Buf + 9, ".%06ld]", tv.tv_usec);
        
        fprintf(FP, "%s %s", Buf, str);
    }

    /* ParseDebugMode - Parse the debug mode from the environment variable
     * LLVM_DEBUG. */
    void ParseDebugMode()
    {
        static char debug_str[DEBUG_NUM][65] = {
            "none", "llvm", "in_asm", "op", "out_asm", "ir", "entry", 
            "dbo", "verify", "asm", "debug", "all",
        };
        static uint64_t debug_enum[DEBUG_NUM] = {
            DEBUG_NONE, DEBUG_LLVM, DEBUG_INASM, DEBUG_OP, DEBUG_OUTASM, 
            DEBUG_IR, DEBUG_ENTRY, DEBUG_DBO, DEBUG_VERIFY, DEBUG_ASM,
            DEBUG_DEBUG, DEBUG_ALL,
        };

        char *s, *p, *e, buf[65];

        p = getenv("LLVM_DEBUG");
        if (p == NULL || strlen(p) == 0)
            return;

        e = p + strlen(p);
        s = buf;
        while (p <= e)
        {
            if (*p == ',' || p == e)
            {
                *s = '\0';
                for (int i = 0; i < DEBUG_NUM; i++)
                {
                    if (!strcmp(buf, debug_str[i]))
                    {
                        Mode |= debug_enum[i];
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

public:
    LLVMDebug(uint64_t mode=DEBUG_NONE) : Mode(DEBUG_NONE), FP(stderr)
    {
        llvm_lock_init(&Lock);

        /* If the debug mode is not specified internally, parse it from the
         * environment variable. */
        if (mode == DEBUG_NONE)
            ParseDebugMode();
        else
            setDebug(mode);

        gettimeofday(&Start, NULL); 
    }

    ~LLVMDebug() 
    {
        Mode = DEBUG_NONE;
    }

    uint64_t getDebug()           { return Mode;       }
    void setDebug(uint64_t mode)  { Mode = mode;       }
    void resetDebug()             { Mode = DEBUG_NONE; }

    /* Error - Display the error message and terminate. */
    void Error(const char *fmt, ...)
    {
        static char str[256] = "Error: ";
        va_list ap;
        
        llvm_spin_lock(&Lock);
        va_start(ap, fmt);
        vsprintf(str + 7, fmt, ap);
        va_end(ap);
        Print(str);
        exit(1);
        llvm_spin_unlock(&Lock);
    }

    /* Warning - Display a warning message. */
    void Warning(const char *fmt, ...)
    {
        static char str[256] = "Warning: ";
        va_list ap;
        
        llvm_spin_lock(&Lock);
        va_start(ap, fmt);
        vsprintf(str + 9, fmt, ap);
        va_end(ap);
        Print(str);
        llvm_spin_unlock(&Lock);
    }

    /* Debug - Display the debug message according to the debug level. */
    void Debug(uint64_t label, const char *fmt, ...)
    {
        if ((Mode & label) == 0)
            return;

        static char str[256];
        va_list ap;

        llvm_spin_lock(&Lock);
        va_start(ap, fmt);
        vsprintf(str, fmt, ap);
        va_end(ap);
        Print(str);
        llvm_spin_unlock(&Lock);
    }
};

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
