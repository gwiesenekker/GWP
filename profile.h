/*
This file is part of GWP 1.2, a high-resolution code profiler for C/C++ code
Copyright (C) 1998-2018 Gijsbert Wiesenekker <gijsbert.wiesenekker@gmail.com>

GWP is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GWP is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GWP.  If not, see <https://www.gnu.org/licenses/>.
*/
#ifndef ProfileH
#define ProfileH

#ifdef PROFILE

#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>  

#define PROFILE_INVALID (-1)
#define THREAD_MAX      16
#define RECURSE_MAX     100

#define PID return_pid(syscall(SYS_gettid))

#define PG profile_global[pid]
#define PS profile_static[pid]
#define PL profile_local[pid]

typedef unsigned long long profile_t;

typedef struct
{
  profile_t counter_stamp;
  profile_t *counter_pointer;
} profile_global_t;

typedef struct
{
  int block_id[RECURSE_MAX];
  int block_invocation;
} profile_static_t;

extern profile_global_t profile_global[THREAD_MAX];

int return_pid(int);
void counter(profile_t *);
void init_block(int [RECURSE_MAX]);
int new_block(int, const char *, int *);
void begin_block(int, int);
void end_block(int);
void init_profile(void);
void clear_profile(void);
void dump_profile(int, int);

#define BEGIN_BLOCK(X) \
  {\
    static profile_static_t profile_static[THREAD_MAX];\
    int pid = PID;\
    counter(&(PG.counter_stamp));\
    if (PS.block_invocation == 0) {init_block(PS.block_id); PS.block_invocation = 1;}\
    PS.block_invocation++;\
    if (PS.block_id[PS.block_invocation] == PROFILE_INVALID)\
      PS.block_id[PS.block_invocation] = new_block(pid, X, &(PS.block_invocation));\
    begin_block(pid, PS.block_id[PS.block_invocation]);\
    counter(PG.counter_pointer);\
  }
#define END_BLOCK \
  {\
    int pid = PID;\
    counter(&(PG.counter_stamp));\
    end_block(pid);\
    counter(PG.counter_pointer);\
  }

#define INIT_PROFILE init_profile();
#define CLEAR_PROFILE clear_profile();
#define DUMP_PROFILE(V) dump_profile(PID, V);

#else
#define BEGIN_BLOCK(X)
#define END_BLOCK
#define INIT_PROFILE
#define DUMP_PROFILE(V)
#endif

#endif

