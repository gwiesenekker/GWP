/*
This file is part of GWP 1.2, a profile utility for C/C++ code
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

#include "profile.h"

#ifdef PROFILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#undef PROFILE_REALTIME

#define PROFILE_BUG(X) if (X)\
  {fprintf(stderr, "%s::%ld:%s\n", __FILE__, (long) __LINE__, #X); exit(EXIT_FAILURE);}

#define local static
#define FALSE 0
#define TRUE  1
#define or    ||

#define NAME_MAX  32
#define BLOCK_MAX 100
#define STACK_MAX 100

#define SECS(Y, X) (((double) (Y) - (double) (X)) / (double) frequency)
#define PERC(X) ((X) / time_self_total * 100)

#define PL profile_local[pid]

//call stack
typedef struct
{
  int stack_id;

  double stack_time_self;
  double stack_time_total;

  profile_t stack_count_begin;
  profile_t stack_count_end;
} stack_t;

typedef struct
{
  int child_active;
  long long child_calls;
  double child_time_total;
} child_t;

typedef struct
{
  int parent_active;
  long long parent_calls;
} parent_t;

typedef struct
{
  char block_name[NAME_MAX];
  int block_suspect;
  int block_invocation;

  //needed for end_block
  int *block_invocation_pointer;

  long long block_calls;

  double block_time_self_total;
  double block_time_total;

  long long block_child_calls;
  double block_child_time_total;
  double block_time_error;

  parent_t block_parent[BLOCK_MAX];
  child_t block_child[BLOCK_MAX];

  double block_time_recursive_total;
  long long block_calls_recursive_total;
} block_t;

typedef struct
{
  int nstack;
  stack_t stack[STACK_MAX];

  int nblock;
  block_t block[BLOCK_MAX];

  profile_t counter_overhead_begin;
  profile_t counter_overhead_end;
  double time_total;
} profile_local_t;

profile_global_t profile_global[THREAD_MAX];

local pthread_mutex_t profile_mutex;

local int pid[THREAD_MAX];
local profile_local_t profile_local[THREAD_MAX];

local profile_t frequency;
local profile_t counter_dummy;
local double time_for_loop;
local double time_counter;

int return_pid(int tid)
{
  int result;

  pthread_mutex_lock(&profile_mutex);

  for (result = 0; result < THREAD_MAX; result++)
    if (pid[result] == tid) break;

  if (result >= THREAD_MAX)
  {
    for (result = 0; result < THREAD_MAX; result++)
      if (pid[result] == PROFILE_INVALID) break;

    PROFILE_BUG(result >= THREAD_MAX)

    pid[result] = tid;
  }

  pthread_mutex_unlock(&profile_mutex);

  return(result);
}

void counter(profile_t *counter_pointer)
{
  struct timespec tv;
  
#ifdef PROFILE_REALTIME
  PROFILE_BUG(clock_gettime(CLOCK_REALTIME, &tv) != 0)
#else
  PROFILE_BUG(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv) != 0)
#endif

  *counter_pointer = tv.tv_sec * 1000000000 + tv.tv_nsec;
}

void init_block(int block_id[RECURSE_MAX])
{
  for (int iblock = 0; iblock < RECURSE_MAX; iblock++)
    block_id[iblock] = PROFILE_INVALID;
}

local void clear_block(block_t *with_block)
{
  with_block->block_suspect = FALSE;
  with_block->block_calls = 0;

  with_block->block_time_self_total = 0.0;
  with_block->block_time_total = 0.0;

  for (int iblock = 0; iblock < BLOCK_MAX; iblock++)
  {
    child_t *with_child = with_block->block_child + iblock;

    with_child->child_active = FALSE;
    with_child->child_calls = 0;
    with_child->child_time_total = 0.0;

    parent_t *with_parent = with_block->block_parent + iblock;

    with_parent->parent_active = FALSE;
    with_parent->parent_calls = 0;
  }
}

#define MANGLE_MAX 256

local void mangle(char *dest, const char *source)
{
  char m[MANGLE_MAX];

  if (strlen(source) < NAME_MAX)
    strncpy(dest, source, NAME_MAX);
  else
  {
    PROFILE_BUG(strlen(source) >= MANGLE_MAX)

    strncpy(m, source, MANGLE_MAX);
    
    //remove vowels from the right

    int n = strlen(m);

    while(n >= NAME_MAX)
    {
      //search for a vowel or underscore from the right

      while(n >= 0)
      {
        //first vowels

        if ((m[n] == 'a') || (m[n] == 'o') or (m[n] == 'u') or
            (m[n] == 'i') or (m[n] == 'e')) break;
        
        //then underscores

        if (m[n] == '_') break;

        n--;
      }
      PROFILE_BUG(n < 0)
      
      //remove vowel

      while((m[n] = m[n + 1]) != '\0') n++;
    }
    PROFILE_BUG(strlen(m) >= NAME_MAX)

    strncpy(dest, m, NAME_MAX - 1);
  }
}

int new_block(int pid, const char *name, int *invocation_pointer)
{
  PROFILE_BUG(PL.nblock >= BLOCK_MAX)

  int block_id = PL.nblock;

  block_t *with_block = PL.block + block_id;

  mangle(with_block->block_name, name);

  with_block->block_name[NAME_MAX - 1] = '\0';

  with_block->block_invocation = *invocation_pointer;

  with_block->block_invocation_pointer = invocation_pointer;

  clear_block(with_block);

  PL.nblock++;

  return(block_id);
}

void begin_block(int pid, int block_id)
{
  if (PL.nstack > 0)
  {
    stack_t *with_previous = PL.stack + PL.nstack - 1;

    with_previous->stack_count_end = PG.counter_stamp;

    with_previous->stack_time_self +=
      SECS(with_previous->stack_count_end, with_previous->stack_count_begin);
  }
  else
  {
    counter(&(PL.counter_overhead_begin));
  }
  PROFILE_BUG(PL.nstack >= STACK_MAX)

  stack_t *with_current = PL.stack + PL.nstack;

  with_current->stack_id = block_id;

  with_current->stack_time_self = 0.0;

  with_current->stack_time_total = 0.0;

  PG.counter_pointer = &(with_current->stack_count_begin);

  PL.nstack++;
}

void end_block(int pid)
{
  PL.nstack--;

  PROFILE_BUG(PL.nstack < 0)

  stack_t *with_current = PL.stack + PL.nstack;

  with_current->stack_count_end = PG.counter_stamp;

  with_current->stack_time_self +=
    SECS(with_current->stack_count_end, with_current->stack_count_begin);

  with_current->stack_time_total += with_current->stack_time_self;

  block_t *with_block = PL.block + with_current->stack_id;

  with_block->block_calls++;

  with_block->block_time_self_total += with_current->stack_time_self;

  with_block->block_time_total += with_current->stack_time_total;

  (*with_block->block_invocation_pointer)--;

  PROFILE_BUG(*with_block->block_invocation_pointer < 0);

  PG.counter_pointer = &counter_dummy;

  if (PL.nstack > 0)
  {
    stack_t *with_previous = PL.stack + (PL.nstack - 1);

    with_previous->stack_time_total += with_current->stack_time_total;

    PG.counter_pointer = &(with_previous->stack_count_begin);

    //update parent in child

    parent_t *with_parent = PL.block[with_current->stack_id].block_parent +
                            with_previous->stack_id;

    with_parent->parent_active = TRUE;

    with_parent->parent_calls++;

    //update child in parent

    child_t *with_child = PL.block[with_previous->stack_id].block_child +
                          with_current->stack_id;

    with_child->child_active = TRUE;

    with_child->child_calls++;

    with_child->child_time_total += with_current->stack_time_total;
  }
  else
  {
    counter(&(PL.counter_overhead_end));

    PL.time_total += SECS(PL.counter_overhead_end, PL.counter_overhead_begin);
  }
}

#define NCALL 1000000

void init_profile(void)
{
  profile_t count_begin;
  profile_t count_end;

  PROFILE_BUG(pthread_mutex_init(&profile_mutex, NULL) != 0)

  for (int ithread = 0; ithread < THREAD_MAX; ithread++)
  {
    pid[ithread] = PROFILE_INVALID;

    profile_local_t *with = profile_local + ithread;

    with->nblock = 0;

    with->nstack = 0;

    with->time_total = 0.0;
  }

  (void) remove("profile.txt");
  for (int ithread = 0; ithread < THREAD_MAX; ithread++)
  {
    char name[NAME_MAX];

    snprintf(name, NAME_MAX, "profile-%d.txt", ithread);
    (void) remove(name);
  }

#ifdef PROFILE_REALTIME
  counter(&count_begin);
  sleep(1);
  counter(&count_end);
  frequency = count_end - count_begin;
#else
  frequency = 1000000000;
#endif

  counter(&count_begin);
  for (int icall = 0; icall < NCALL; icall++);
  counter(&count_end);

  time_for_loop = SECS(count_end, count_begin) / NCALL;

  counter(&count_begin);
  for (int icall = 0; icall < NCALL; icall++) counter(&count_end);

  time_counter = SECS(count_end, count_begin) / NCALL - time_for_loop;
}

void dump_profile(int pid, int verbose)
{
  char name[NAME_MAX];
  FILE *f;

  //block_invocation off by one due to 0 initialization

  for (int iblock = 0; iblock < BLOCK_MAX; iblock++)
  {
    block_t *with_block = PL.block + iblock;

    if (with_block->block_invocation > 0) with_block->block_invocation--;
  }

  if (pid == 0)
    strncpy(name, "profile.txt", NAME_MAX);
  else
    snprintf(name, NAME_MAX, "profile-%d.txt", pid - 1);

  PROFILE_BUG((f = fopen(name, "w")) == NULL)

  {
    char stamp[NAME_MAX];
    time_t t = time(NULL);

    (void) strftime(stamp, NAME_MAX, "%H:%M:%S-%d/%m/%Y", localtime(&t));

    fprintf(f, "# Profile dumped at %s\n", stamp);
  }

  fprintf(f, "# Resolution is %llu counts/sec, or %.10f secs/count.\n",
    frequency, 1.0/frequency);

  fprintf(f, "# Needed %.10f secs for dummy loop.\n", time_for_loop);

  fprintf(f, "# Needed %.10f secs for querying counter.\n", time_counter);

  fprintf(f, "# The total number of blocks is %d.\n", PL.nblock);

  if (PL.nstack > 0)
  {
    fprintf(f, "# The following blocks are not properly terminated by an END_BLOCK!\n");

    for (int istack = 0; istack < PL.nstack; istack++)
    {
      block_t *with_block = PL.block + PL.stack[istack].stack_id;

      fprintf(f, "%s (invocation %d)\n",
        with_block->block_name,with_block->block_invocation);
    }
    fprintf(f, "\n");
  }

  //total time

  double time_self_total = 0.0;

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    block_t *with_block = PL.block + iblock;

    with_block->block_child_calls = 0;

    with_block->block_child_time_total = 0.0;

    for (int jblock = 0; jblock < BLOCK_MAX; jblock++)
    {
      child_t *with_child = with_block->block_child + jblock;

      if (!with_child->child_active) continue;

      with_block->block_child_calls += with_child->child_calls;

      with_block->block_child_time_total += with_child->child_time_total;
    }

    with_block->block_time_error = (with_block->block_calls +
      with_block->block_child_calls) * time_counter;

    time_self_total += with_block->block_time_self_total;
  }

  fprintf(f, "# Total run time was %.10f secs.\n", PL.time_total);

  fprintf(f, "# Total self time was %.10f secs.\n", time_self_total);

  fprintf(f, "# Total profile overhead was %.10f secs.\n",
    PL.time_total - time_self_total);

   fprintf(f, "\n");

  //blocks with very short run time or large call overhead

  int found = FALSE;

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    block_t *with_block = PL.block + iblock;

    if (with_block->block_time_self_total <= 4.0 * with_block->block_time_error)
    {
      found = TRUE;

      with_block->block_suspect = TRUE;
    }
  }

  if (found)
  {
    fprintf(f, "# The self times of the following blocks are suspect\n");
    fprintf(f, "# because the self times are of the same order of magnitude\n");
    fprintf(f, "# as the error caused by querying the counter.\n");

    fprintf(f, "%-32s %-10s %16s %10s %10s %s\n", 
      "name", "invocation", "self time", "calls", "children",
      "estimated error");

    for (int iblock = 0; iblock < PL.nblock; iblock++)
    {
      block_t *with_block = PL.block + iblock;

      if (with_block->block_suspect)
      {
        fprintf(f, "%-32s %-10d %16.10f %10lld %10lld %.10f\n",
          with_block->block_name,
          with_block->block_invocation,
          with_block->block_time_self_total,
          with_block->block_calls,
          with_block->block_child_calls,
          with_block->block_time_error);
      }
    }
    fprintf(f, "\n");
  }

  //sort by straight insertion

  int sort[BLOCK_MAX];

  for (int iblock = 0; iblock < PL.nblock; iblock++)
    sort[iblock] = iblock;

  for (int iblock = 0; iblock < PL.nblock - 1; iblock++)
  {
    int kblock = iblock;

    for (int jblock = iblock + 1; jblock < PL.nblock; jblock++)
    {
      if (PL.block[sort[jblock]].block_time_total >
          PL.block[sort[kblock]].block_time_total) kblock = jblock;
    }

    int t = sort[iblock];
    sort[iblock] = sort[kblock];
    sort[kblock] = t;
  }

  fprintf(f, "# Blocks sorted by total time spent in block and children.\n");

  fprintf(f, "# The sum of total times (or the sum of the percentages)\n");

  fprintf(f, "# does not have any meaning, since children will be double counted.\n");

  fprintf(f, "%-32s %-10s %6s %16s %10s %s\n", 
    "name", "invocation", "perc", "total time", "calls", "estimated error");

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    block_t *with_block = PL.block + sort[iblock];

    fprintf(f, "%-32s %-10d %6.2f %16.10f %10lld %.10f\n",
      with_block->block_name, with_block->block_invocation,
      PERC(with_block->block_time_total),
      with_block->block_time_total,
      with_block->block_calls,
      with_block->block_time_error
    );
  }
  fprintf(f, "\n");

  //sort by straight insertion

  for (int iblock = 0; iblock < PL.nblock; iblock++)
    sort[iblock] = iblock;

  for (int iblock = 0; iblock < PL.nblock - 1; iblock++)
  {
    int kblock = iblock;

    for (int jblock = iblock + 1; jblock < PL.nblock; jblock++)
    {
      if (PL.block[sort[jblock]].block_time_self_total >
          PL.block[sort[kblock]].block_time_self_total) kblock = jblock;
    }

    int t = sort[iblock];
    sort[iblock] = sort[kblock];
    sort[kblock] = t;
  }

  fprintf(f, "# Blocks sorted by total time spent in own code.\n");

  fprintf(f, "# The sum of the self times is equal to the total self time.\n");

  fprintf(f, "%-32s %-10s %6s %16s %10s %s\n", 
    "name", "invocation", "perc", "self time", "calls", "estimated error");

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    block_t *with_block = PL.block + sort[iblock];

    fprintf(f, "%-32s %-10d %6.2f %16.10f %10lld %.10f\n",
      with_block->block_name, with_block->block_invocation,
      PERC(with_block->block_time_self_total),
      with_block->block_time_self_total,
      with_block->block_calls,
      with_block->block_time_error);
  }
  fprintf(f, "\n");

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    PL.block[iblock].block_time_recursive_total = 0.0;

    PL.block[iblock].block_calls_recursive_total = 0;

    if (PL.block[iblock].block_invocation == 1)
    {
      PL.block[iblock].block_time_recursive_total =
        PL.block[iblock].block_time_self_total;

      PL.block[iblock].block_calls_recursive_total =
        PL.block[iblock].block_calls;

      for (int jblock = 0; jblock < PL.nblock; jblock++)
      {
        if (PL.block[jblock].block_invocation == 1) continue;

        if (strcmp(PL.block[iblock].block_name,
                   PL.block[jblock].block_name) == 0)
        {
          PL.block[iblock].block_time_recursive_total +=
            PL.block[jblock].block_time_self_total;

          PL.block[iblock].block_calls_recursive_total +=
            PL.block[jblock].block_calls;
        }
      }
    }
  }

  //sort by straight insertion

  for (int iblock = 0; iblock < PL.nblock; iblock++)
    sort[iblock] = iblock;

  for (int iblock = 0; iblock < PL.nblock - 1; iblock++)
  {
    int kblock = iblock;

    for (int jblock = iblock + 1; jblock < PL.nblock; jblock++)
    {
      if (PL.block[sort[jblock]].block_time_recursive_total >
          PL.block[sort[kblock]].block_time_recursive_total) kblock = jblock;
    }

    int t = sort[iblock];
    sort[iblock] = sort[kblock];
    sort[kblock] = t;
  }

  fprintf(f, "# Blocks sorted by self times summed over recursive invocations.\n");

  fprintf(f, "%-32s %6s %16s %10s %16s\n",
    "name", "perc", "self time", "calls", "self time/call");

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    int jblock = sort[iblock];

    if (PL.block[jblock].block_invocation == 1)
    {
      fprintf(f, "%-32s %6.2f %16.10f %10lld %16.10f\n",
        PL.block[jblock].block_name,
        PERC(PL.block[jblock].block_time_recursive_total),
        PL.block[jblock].block_time_recursive_total,
        PL.block[jblock].block_calls_recursive_total,
        PL.block[jblock].block_time_recursive_total / 
        PL.block[jblock].block_calls_recursive_total);
    }
  }
  fprintf(f, "\n");

  if (verbose == 0) goto label_return;

  for (int iblock = 0; iblock < PL.nblock; iblock++)
  {
    block_t *with_block = PL.block + sort[iblock];

    fprintf(f, "# Summary for block %s, invocation %d.\n",
      with_block->block_name, with_block->block_invocation);

    fprintf(f, "Spends %.10f secs in %lld call(s), or %.2f%% of total execution time.\n",
      with_block->block_time_total,
      with_block->block_calls,
      PERC(with_block->block_time_total));

    fprintf(f, "Spends %.10f secs (%.2f%%) in own code, %.10f secs (%.2f%%) in children.\n",
      with_block->block_time_self_total, PERC(with_block->block_time_self_total),
      with_block->block_child_time_total, PERC(with_block->block_child_time_total));
    fprintf(f, "\n");

    found = FALSE;

    for (int jblock = 0; jblock < BLOCK_MAX; jblock++)
    {
      child_t *with_child = with_block->block_child + jblock;

      if (!with_child->child_active) continue;

      found = TRUE;

      fprintf(f, "Spends %.10f secs in %lld call(s) to %s, invocation %d.\n",
        with_child->child_time_total,
        with_child->child_calls,
        PL.block[jblock].block_name,
        PL.block[jblock].block_invocation);
    }

    if (!found) fprintf(f, "No children were found.\n");

    found = FALSE;

    for (int jblock = 0; jblock < BLOCK_MAX; jblock++)
    {
      parent_t *with_parent = with_block->block_parent + jblock;

      if (!with_parent->parent_active) continue;

      found = TRUE;

      fprintf(f, "Is called %lld time(s) from %s, invocation %d.\n",
        with_parent->parent_calls,
        PL.block[jblock].block_name, PL.block[jblock].block_invocation);
    }

    if (!found) fprintf(f, "No parents were found\n");

    fprintf(f, "\n");
  }

  label_return:

  fprintf(f, "# End of profile.\n");

  fclose(f);
}

#endif


