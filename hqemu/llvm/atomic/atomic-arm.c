/*
 * Copyright (C) 2010 Parallel Processing Institute (PPI), Fudan Univ.
 *  <http://ppi.fudan.edu.cn/system_research_group>
 *
 * Authors:
 *  Zhaoguo Wang    <zgwang@fudan.edu.cn>
 *  Yufei Chen      <chenyufei@fudan.edu.cn>
 *  Ran Liu         <naruilone@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* We include this file in op_helper.c */

#include <stdlib.h>
#include <pthread.h>
#include "coremu-atomic.h"

__thread uint64_t cm_exclusive_val;
__thread uint32_t cm_exclusive_addr = -1;

#define GEN_LOAD_EXCLUSIVE(type, TYPE) \
void HELPER(load_exclusive##type)(uint32_t reg, uint32_t addr)        \
{                                                                     \
    unsigned long q_addr = 0;                                         \
    DATA_##type val = 0;                                              \
                                                                      \
    cm_exclusive_addr = addr;                                         \
    CM_GET_QEMU_ADDR(q_addr,addr);                                    \
    val = *(DATA_##type *)q_addr;                                     \
    cm_exclusive_val = val;                                           \
    env->regs[reg] = val;                                             \
}

GEN_LOAD_EXCLUSIVE(b, B);
GEN_LOAD_EXCLUSIVE(w, W);
GEN_LOAD_EXCLUSIVE(l, L);
//GEN_LOAD_EXCLUSIVE(q, Q);

#define GEN_STORE_EXCLUSIVE(type, TYPE) \
void HELPER(store_exclusive##type)(uint32_t res, uint32_t reg, uint32_t addr) \
{                                                                             \
    unsigned long q_addr = 0;                                                    \
    DATA_##type val = 0;                                                      \
    DATA_##type r = 0;                                                        \
                                                                              \
    if(addr != cm_exclusive_addr)                                             \
        goto fail;                                                            \
                                                                              \
    CM_GET_QEMU_ADDR(q_addr,addr);                                            \
    val = (DATA_##type)env->regs[reg];                                        \
                                                                              \
    r = atomic_compare_exchange##type((DATA_##type *)q_addr,                  \
                                    (DATA_##type)cm_exclusive_val, val);      \
                                                                              \
    if(r == (DATA_##type)cm_exclusive_val) {                                  \
        env->regs[res] = 0;                                                   \
        goto done;                                                            \
    } else {                                                                  \
        goto fail;                                                            \
    }                                                                         \
                                                                              \
fail:                                                                         \
    env->regs[res] = 1;                                                       \
                                                                              \
done:                                                                         \
    cm_exclusive_addr = -1;                                                   \
    return;                                                                   \
}

GEN_STORE_EXCLUSIVE(b, B);
GEN_STORE_EXCLUSIVE(w, W);
GEN_STORE_EXCLUSIVE(l, L);
//GEN_STORE_EXCLUSIVE(q, Q);

void HELPER(load_exclusiveq)(uint32_t reg, uint32_t addr)
{
   unsigned long q_addr = 0;
   uint64_t val = 0;

   cm_exclusive_addr = addr;
   CM_GET_QEMU_ADDR(q_addr,addr);
   val = *(uint64_t *)q_addr;
   cm_exclusive_val = val;
   env->regs[reg] = (uint32_t)val;
   env->regs[reg + 1] = (uint32_t)(val>>32);
}

void HELPER(store_exclusiveq)(uint32_t res, uint32_t reg, uint32_t addr)
{
   unsigned long q_addr = 0;
   uint64_t val = 0;
   uint64_t r = 0;

   if(addr != cm_exclusive_addr)
        goto fail;

   CM_GET_QEMU_ADDR(q_addr,addr);
   val = (uint32_t)env->regs[reg];
   val |= ((uint64_t)env->regs[reg + 1]) << 32;

   r = atomic_compare_exchangeq((uint64_t *)q_addr,
                                    (uint64_t)cm_exclusive_val, val);

   if(r == (uint64_t)cm_exclusive_val) {
        env->regs[res] = 0;
        goto done;
   } else {
        goto fail;
   }

fail:
    env->regs[res] = 1;

done:
    cm_exclusive_addr = -1;
    return;
}

void HELPER(clear_exclusive)(void)
{
    cm_exclusive_addr = -1;
}

void HELPER(swpb)(uint32_t dst, uint32_t src, uint32_t addr)
{
    uint8_t old, val;
    unsigned long q_addr;
    CM_GET_QEMU_ADDR(q_addr,env->regs[addr]);
    val = (uint8_t)env->regs[src];
    old = atomic_exchangeb((uint8_t *)q_addr, (uint8_t)val);
    env->regs[dst] = old;
    //printf("SWPB\n");
}

void HELPER(swp)(uint32_t dst, uint32_t src, uint32_t addr)
{
    uint32_t old, val;
    unsigned long q_addr;
    CM_GET_QEMU_ADDR(q_addr,env->regs[addr]);
    val = env->regs[src];
    old = atomic_exchangel((uint32_t *)q_addr, val);
    env->regs[dst] = old;
    //printf("SWP\n");
}
