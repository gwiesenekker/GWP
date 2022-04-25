#ifndef ProfileH
//SCU REVISION 0.589 ma 25 apr 2022  9:43:39 CEST
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

typedef struct timespec counter_t;

typedef struct
{
  counter_t counter_stamp;
  counter_t *counter_pointer;
} profile_global_t;

typedef struct
{
  int block_id[RECURSE_MAX];
  int block_init;
  int block_invocation;
} profile_static_t;

extern profile_global_t profile_global[THREAD_MAX];

int return_pid(int);
void init_block(int [RECURSE_MAX]);
int new_block(int, const char *, int *);
void begin_block(int, int);
void end_block(int);
void init_profile(void);
void clear_profile(void);
void dump_profile(int, int);

#define GET_COUNTER(P) clock_gettime(CLOCK_THREAD_CPUTIME_ID, P)

#define BEGIN_BLOCK(X) \
  {\
    static profile_static_t profile_static[THREAD_MAX];\
    counter_t counter_stamp;\
    GET_COUNTER(&counter_stamp);\
    int pid = PID;\
    PG.counter_stamp = counter_stamp;\
    if (PS.block_init == 0) {init_block(PS.block_id); PS.block_init = 1;}\
    PS.block_invocation++;\
    if (PS.block_id[PS.block_invocation] == PROFILE_INVALID)\
      PS.block_id[PS.block_invocation] = new_block(pid, X, &(PS.block_invocation));\
    begin_block(pid, PS.block_id[PS.block_invocation]);\
    GET_COUNTER(PG.counter_pointer);\
  }
#define END_BLOCK \
  {\
    counter_t counter_stamp;\
    GET_COUNTER(&counter_stamp);\
    int pid = PID;\
    PG.counter_stamp = counter_stamp;\
    end_block(pid);\
    GET_COUNTER(PG.counter_pointer);\
  }

#define INIT_PROFILE init_profile();
#define DUMP_PROFILE(V) dump_profile(PID, V);

#else
#define BEGIN_BLOCK(X)
#define END_BLOCK
#define INIT_PROFILE
#define DUMP_PROFILE(V)
#endif

#endif

