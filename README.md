# GWP a high-resolution code profiler for Linux C/C++ source code

Statistical code profilers like gprof or perf never worked well for my programs. Yes, they provided clues but never clearly showed where the bottlenecks were. 20 years ago I developed my own code profiler for C/C++ (version 1.1) and I now added thread support to it (version 1.2). In order to use it you have to add the file profile.c to your build and add the following header and macro's to your program you want to profile:
```
#include "profile.h"

 //INIT_PROFILE should be called once by the main program BEFORE any threads are started
INIT_PROFILE
..
 //Bracket each block you want to profile with calls to the macros BEGIN_BLOCK and END_BLOCK
 //BEGIN_BLOCK has the name of the block as the argument
 //BEGIN_BLOCK and END_BLOCK typically bracket functions but can be used to bracket anything like for loops etc.
 //You can use __func__ for "block name" in functions
 //The main program should bracket main with a block called "main"
 //Optionally the main program and the threads should bracket the part of the main program/the part of the thread 
 //where most of the time is spent with a block called "main-thread". The self-time percentages will be calculated against 
 //main-thread or main in that order.
 
 BEGIN_BLOCK("main")
 
 BGIN_BLOCK("main-thread")
 
 BEGIN_BLOCK("block name")
..
//DO NOT USE MULTIPLE RETURNS AS BEGIN_BLOCK and END_BLOCK have to match
//return(result);
result = .. ;
goto end_block;
..
end_block:
 //Call END_BLOCK at the end of the block or just before the exit or return of the function
END_BLOCK
..
return(result);
..
//main-thread
END_BLOCK
//main
END_BLOCK

//DUMP_PROFILE should be called once by each thread at the end of the thread 
//VERBOSE can be 0 or 1. Creates files profile-<thread-sequence-number>.txt for each thread.

DUMP_PROFILE(VERBOSE) 
```
BLOCKS can be nested and recursion is supported.

The macro's expand to code that collect the profile information when you compile your program with -DPROFILE. Obviously BEGIN_BLOCK/END_BLOCK macro's have to match, so multiple returns within procedures and functions should be avoided.

Currently GWP uses hard-coded limits for the number of blocks (BLOCK_MAX), the number of recursive invocations (RECURSE_MAX), the call chain (STACK_MAX) and the number of threads (THREAD_MAX). Increase these as needed if you hit any of these limits.

## Method

GWP needs to collect some information (the time spent, the number of calls, which blocks call which blocks etc.) so BEGIN_BLOCK creates static variables in a code block to store this information AND to avoid name-clashes with your current code. Because these variables reside in a code block, these are not available outside of this block, but END_BLOCK and DUMP_PROFILE need some way to access them. BEGIN_BLOCK links these static variables to global arrays and pointers so that END_BLOCK and DUMP_PROFILE can update and use that information.

GWP uses the following method to clearly separate the time spent in your code and the time needed to collect the profile information (the profile overhead): when BEGIN_BLOCK/END_BLOCK are entered the time is recorded (you could say that the stopwatch for your code is stopped and the stopwatch for the profiler is started), when BEGIN_BLOCK/END_BLOCK are exited the time is recorded again (the stopwatch for the profiler is stopped and the stopwatch for your code is started again).

The call stack records the start and stop times AND the blocks being started and stopped. If there are no nested blocks the pseudo-code looks like:
```
//BEGIN_BLOCK
Record the time t1
  Do some administration (the profile overhead):
    Store t1 (confusingly called counter_overhead_begin)
    Link the block to the stack frame
    Setup a pointer that will record the start time of the block t2 in stack_counter_begin
    Record the time t2 and store it in that pointer

//END_BLOCK
Record the time t3
  Do some administration (the profile overhead):
    //Self time and total time are the same if there are no nested blocks
    Increment block_self_time_total with t3 - t2
    Increment block_time_total with t3 - t2
    Record the time t4
    Store t4 (confusingly called counter_overhead_end)
    Increment time_total with t4 - t1

The profile overhead will be total-time - the self-time of the block
```
For nested blocks the pseudo-code looks like:
```
//BEGIN_BLOCK
Record the time t1
  Do some administration (the profile overhead):
    //Stop the clock in the previous stack frame and update the self time
    Previous stack_counter_end = t1
    Increment previous stack_time_self = t1 - previous stack_counter_begin
    Link the block to the stack frame
    Setup a pointer that will record the time t2 in stack_counter_begin
    Record the time t2 and store it in that pointer

//END_BLOCK
Record the time t3
  Do some administration (the profile overhead):
    Increment block_self_time_total with t3 - t2
    Increment block_time_total with t3 - t2
    Increment previous stack_time_total with current stack_time_total
    Setup a pointer that will record the time t4 in previous stack_counter_begin
    Record the time t4
    Store t4 in that pointer
```

## Intrinsic profile overhead

So the minimum unavoidable or intrinsic profile overhead is the sequence:
```
Record the time t1 in a variable
Store the time t2 in a pointer
```
Ideally t2 - t1 should be zero if there is no code executed between t1 and t2. Recently I started to notice some strange results when profiling small functions (less than 100 ticks) that were called many times, so I decided to take a look at how large the intrinsic profile overhead is using the following program. You can compile it standalone if you are interested in what the intrinsic profile overhead looks like on your system.
```
#include <stdio.h>
//SCU REVISION 0.588 za 23 apr 2022 14:29:57 CEST
#include <stdlib.h>
#include <time.h>
#include <math.h>

typedef struct timespec counter_t;

#define GET_COUNTER(P) clock_gettime(CLOCK_THREAD_CPUTIME_ID, P)

#define TICKS(TV) (TV.tv_sec * 1000000000 + TV.tv_nsec)

void update_mean_sigma(long long n, long long x,
  long long *xmax, double *mn, double *sn)
{
  double mnm1 = *mn;
  double snm1 = *sn;

  if (xmax != NULL)
  {
    if (x > *xmax) *xmax = x;
  }

  *mn = mnm1 + (x - mnm1) / n;
  
  *sn = snm1 + (x - mnm1) * (x - *mn);
}

#define NCALL 1000000LL

int main(int argc, char **argv)
{
  counter_t counter_dummy;
  counter_t *counter_pointer;

  counter_pointer = &counter_dummy;
  
  for (int j = 0; j < 10; j++)
  {
    double mn = 0.0;
    double sn = 0.0;
  
    for (long long n = 1; n <= NCALL; ++n)
    {
      counter_t counter_stamp;
    
      GET_COUNTER(&counter_stamp);
      GET_COUNTER(counter_pointer);
    
      update_mean_sigma(n, TICKS(counter_dummy) - TICKS(counter_stamp),
        NULL, &mn, &sn);
    }

    //note that the distribution is very skewed, so the distribution
    //is not normal:
    //deviations LESS than the mean (or the mean - sigma) hardly ever occur
    //but deviations LARGER than (mean + sigma) do occur more often
 
    //arbitrarily set the standard deviation to one-third of the mean
    //to check for large positive deviations instead of
    //round(sqrt(sigma / NCALL));

    long long sigma = round(mn / 3.0);
    long long mean = round(mn);

    long long nlarge = 0;
    long long largest = 0;

    for (long long n = 1; n <= NCALL; ++n)
    {
      counter_t counter_stamp;
    
      GET_COUNTER(&counter_stamp);
      GET_COUNTER(counter_pointer);
    
      long long delta = TICKS(counter_dummy) - TICKS(counter_stamp);

      if (delta > largest) largest = delta;
      if (delta > (mean + 3 * sigma)) nlarge++;
    }

    printf("NCALL=%lld mean=%lld nlarge=%lld largest=%lld\n",
      NCALL, mean, nlarge, largest);
  }
}
```
The program returns the mean value of t2 - t1, the number of deviations larger than twice the mean and the largest value of t2 - t1. On my AMD 1950X the results are:
```
$ a.out
NCALL=1000000 mean=188 nlarge=118 largest=4389
NCALL=1000000 mean=186 nlarge=122 largest=4698
NCALL=1000000 mean=188 nlarge=152 largest=6062
NCALL=1000000 mean=187 nlarge=109 largest=6041
NCALL=1000000 mean=188 nlarge=110 largest=6202
NCALL=1000000 mean=187 nlarge=111 largest=6061
NCALL=1000000 mean=187 nlarge=110 largest=5761
NCALL=1000000 mean=189 nlarge=120 largest=9649
NCALL=1000000 mean=191 nlarge=110 largest=5680
NCALL=1000000 mean=189 nlarge=139 largest=5400
```
So the intrinsic profile overhead is 188 ticks on average which is far from negligible when profiling small functions, so we have to correct for it. But as you can see from the results you cannot get the correction perfectly right as the mean is not constant and sometimes (100 times out of 1M samples) a very large value is returned. I do not know why, it could be related to the performance counters on an AMD 1950X, it could be related to the Linux kernel or to the general problem of returning a meaningful high-resolution thread-specific timer on modern processors. Even if you execute the program on one CPU using taskset the results are not constant:
```
$ taskset --cpu-list 0 a.out
NCALL=1000000 mean=186 nlarge=217 largest=22142
NCALL=1000000 mean=190 nlarge=182 largest=14697
NCALL=1000000 mean=191 nlarge=174 largest=5370
NCALL=1000000 mean=192 nlarge=182 largest=7253
NCALL=1000000 mean=191 nlarge=139 largest=24786
NCALL=1000000 mean=189 nlarge=160 largest=8927
NCALL=1000000 mean=190 nlarge=139 largest=5761
NCALL=1000000 mean=188 nlarge=153 largest=7364
NCALL=1000000 mean=190 nlarge=148 largest=7374
$ taskset --cpu-list 7 a.out
NCALL=1000000 mean=190 nlarge=144 largest=64312
NCALL=1000000 mean=191 nlarge=170 largest=64401
NCALL=1000000 mean=190 nlarge=151 largest=68719
NCALL=1000000 mean=192 nlarge=173 largest=68389
NCALL=1000000 mean=189 nlarge=141 largest=64181
NCALL=1000000 mean=190 nlarge=177 largest=65152
NCALL=1000000 mean=195 nlarge=208 largest=63920
NCALL=1000000 mean=189 nlarge=152 largest=65162
NCALL=1000000 mean=189 nlarge=151 largest=64120
NCALL=1000000 mean=189 nlarge=139 largest=64410
```
Oddly, for one CPU (7) my AMD 1950x always returns very large largest values (and I cannot help noticing the largest values are around 65536). So how should we correct for the intrinsic profile overhead? GWP uses the following method: after calculating t2 - t1 or t3 - t2, GWP takes two samples of the intrinsic profile overhead and subtracts it from t2 - t1 or t3 - t2. The idea of sampling instead of using a fixed value for the intrinsic profile overhead is that it adjusts for the intrinsic profile overhead at that point in time (perhaps the counter is 'slow'), but as you cannot know which value of the intrinsic profile overhead will be returned it will always be an approximation. It can also happen that t2 - t1 or t3 - t2 minus the intrinsic profile overhead is less than zero, especially if a large value for the intrinsic profile overhead is returned. In that case GWP uses zero for t2 - t1 or t3 - t2.

So how well does the correction work? The self-times of all the following blocks should be 0 ticks:
```
  for (long long n = 1; n <= NVALIDATE; ++n)
  {
    BEGIN_BLOCK("profile-0-0")
    END_BLOCK
  
    BEGIN_BLOCK("profile-1-1")
      BEGIN_BLOCK("profile-1-1-0")
      END_BLOCK
    END_BLOCK
  
    BEGIN_BLOCK("profile-2-2")
      BEGIN_BLOCK("profile-2-2-1-0")
      END_BLOCK
      BEGIN_BLOCK("profile-2-2-2-0")
      END_BLOCK
    END_BLOCK
  
    BEGIN_BLOCK("profile-3-1")
      BEGIN_BLOCK("profile-3-3-1-3")
        BEGIN_BLOCK("profile-3-3-1-3-1-0")
        END_BLOCK
        BEGIN_BLOCK("profile-3-3-1-3-2-0")
        END_BLOCK
        BEGIN_BLOCK("profile-3-3-1-3-3-0")
        END_BLOCK
      END_BLOCK
    END_BLOCK
  }
```
The naming convention of the blocks is such that the last number reflects the number of children of the block. The results are:
```
# Blocks sorted by self times summed over recursive invocations.
name                               perc  %main        self time      calls   self time/call ticks/call
profile-3-3-1-3                   18.76  18.76     0.0006423540     100000     0.0000000064          6
main                              17.21  17.21     0.0005893700          1     0.0005893700     589370
profile-2-2                       13.95  13.95     0.0004776800     100000     0.0000000048          5
profile-1-1                        9.07   9.07     0.0003106530     100000     0.0000000031          3
profile-3-1                        8.58   8.58     0.0002937990     100000     0.0000000029          3
profile-2-2-1-0                    5.30   5.30     0.0001816500     100000     0.0000000018          2
profile-3-3-1-3-1-0                5.02   5.02     0.0001718610     100000     0.0000000017          2
profile-3-3-1-3-3-0                4.83   4.83     0.0001654110     100000     0.0000000017          2
profile-0-0                        4.54   4.54     0.0001555210     100000     0.0000000016          2
profile-2-2-2-0                    4.40   4.40     0.0001506720     100000     0.0000000015          2
profile-3-3-1-3-2-0                4.30   4.30     0.0001473570     100000     0.0000000015          1
profile-1-1-0                      4.03   4.03     0.0001380820     100000     0.0000000014          1
```
As you can see the correction works really well. Ideally the ticks/call should be 0 in this case, but it is close. What you can also see is an intrinsic build-up of the error depending on the number of children, going from 1-2 (0 children) to 6 (three children). This is unavoidable, as the counters in the parent have to be stopped and started again (with the corresponding small error) each time a child is started, otherwise you cannot correct for the intrinsic profile overhead.

## Recursion

Profiling recursive procedures and functions is not easy. GWP solves this problem by profiling each invocation separately. DUMP_PROFILE shows both the time spent in each invocation and summed over invocations.

## Example

Here are the results for a 30 second run of my Draughts program GWD. DUMP_PROFILE will shorten large block names by removing vowels and underscores from the right until they are smaller than 32 characters.
```
# Profile dumped at 09:51:10-25/04/2022
# The frequency is 1000000000 ticks, or 0.0000000010 secs/tick.
# The intrinsic profile overhead is 190 ticks on average.
# 129 out of 1000000 samples of the intrinsic profile overhead
# ..are larger than twice the mean, the largest value is 5110.
# The total number of blocks is 57.
# The total run time was 35.0390780820 secs.
# The total self time was 8.1687480770 secs.
# The total profile overhead was 26.8703300050 secs.

# Blocks sorted by total time spent in block and children.
# The sum of total times (or the sum of the percentages)
# does not have any meaning, since children will be double counted.
name                             invocation   perc       total time      calls
main-thread                      1           39.58     3.2330612150          1
solve_problems                   1           39.58     3.2330520290          1
white_search                     1           39.57     3.2326368230          1
black_alpha_beta                 1           39.54     3.2302730730        142
white_alpha_beta                 1           39.54     3.2300232070        449
black_alpha_beta                 2           39.53     3.2294950100       1391
white_alpha_beta                 2           39.52     3.2280448340       2717
black_alpha_beta                 3           39.48     3.2254132280       6968
white_alpha_beta                 3           39.40     3.2186641640      12925
black_alpha_beta                 4           39.25     3.2061154810      29398
white_alpha_beta                 4           38.92     3.1795478960      36926
black_alpha_beta                 5           38.34     3.1321089190      87456
white_alpha_beta                 5           37.31     3.0475992410      92132
black_alpha_beta                 6           35.69     2.9158019750     165196
white_alpha_beta                 6           33.40     2.7285928930     186570
main-final                       1           32.15     2.6261303780          1
black_alpha_beta                 7           29.94     2.4454498040     213416
main-init                        1           27.08     2.2121751540          1
white_alpha_beta                 7           26.39     2.1558449140     219140
black_alpha_beta                 8           21.18     1.7299278230     183189
white_alpha_beta                 8           17.53     1.4317831730     153564
black_alpha_beta                 9           13.27     1.0841965870     106278
white_alpha_beta                 9           11.01     0.8990524720      77577
black_alpha_beta                 10           8.70     0.7103911640      58926
white_alpha_beta                 10           7.58     0.6189297170      61324
black_alpha_beta                 11           6.36     0.5192336120      58512
white_alpha_beta                 11           5.64     0.4603652020      67198
black_alpha_beta                 12           4.38     0.3577006620      79230
white_alpha_beta                 12           3.34     0.2725755970      84244
gen_black_moves                  1            2.29     0.1873295590     942900
return_pattern_score_double      1            2.24     0.1833768430     180732
black_alpha_beta                 13           1.93     0.1576494080      73205
gen_white_moves                  1            1.75     0.1427854250     918313
main-tests                       1            1.19     0.0973813300          1
white_alpha_beta                 13           1.19     0.0969595380      47124
read_endgame                     1            0.54     0.0443166520    2156188
black_alpha_beta                 14           0.53     0.0433614680      27892
white_alpha_beta                 14           0.25     0.0207433770      12445
undo_black_move                  1            0.18     0.0145089880    1056122
do_black_move                    1            0.18     0.0143103530    1056122
test-neural                      1            0.17     0.0139798400          1
return_neural                    1            0.17     0.0137939380      10000
do_white_move                    1            0.17     0.0137150880    1099301
undo_white_move                  1            0.16     0.0133215390    1099301
black_alpha_beta                 15           0.10     0.0083478310       6931
Ax+b-and-activation-first-layer  1            0.09     0.0070615200      10000
Ax+b-and-activation-other-layrs  1            0.08     0.0065809040      20000
white_alpha_beta                 15           0.04     0.0033847740       1885
black_alpha_beta                 16           0.02     0.0014237250       1408
return_my_timer                  1            0.02     0.0012255290       4342
white_alpha_beta                 16           0.00     0.0003790970        186
black_alpha_beta                 17           0.00     0.0000792530         50
white_alpha_beta                 17           0.00     0.0000132710          6
return_black_mcts_best_move      1            0.00     0.0000089200       1117
check_white_moves                1            0.00     0.0000082900          1
return_white_mcts_best_move      1            0.00     0.0000079280       1123
update_crc32                     1            0.00     0.0000000710          1

# Blocks sorted by total time spent in own code.
# The sum of the self times is equal to the total self time.
name                             invocation   perc        self time      calls
main-final                       1           32.15     2.6261179450          1
main-init                        1           27.08     2.2121751540          1
white_alpha_beta                 7            4.48     0.3661916710     219140
white_alpha_beta                 8            3.71     0.3032134870     153564
black_alpha_beta                 8            2.95     0.2413565070     183189
white_alpha_beta                 6            2.93     0.2391644960     186570
black_alpha_beta                 7            2.81     0.2295304000     213416
gen_black_moves                  1            2.29     0.1873295590     942900
return_pattern_score_double      1            2.24     0.1833768430     180732
white_alpha_beta                 9            1.98     0.1620656130      77577
black_alpha_beta                 9            1.83     0.1498701440     106278
black_alpha_beta                 6            1.78     0.1455017210     165196
gen_white_moves                  1            1.75     0.1427854250     918313
white_alpha_beta                 5            1.31     0.1068350240      92132
white_alpha_beta                 12           1.05     0.0856674220      84244
main-tests                       1            1.02     0.0834005580          1
white_alpha_beta                 10           1.00     0.0816857360      61324
white_alpha_beta                 11           0.91     0.0740875910      67198
black_alpha_beta                 10           0.88     0.0716554880      58926
black_alpha_beta                 5            0.79     0.0641563150      87456
black_alpha_beta                 12           0.66     0.0537188950      79230
white_alpha_beta                 13           0.56     0.0458512920      47124
read_endgame                     1            0.54     0.0443166520    2156188
black_alpha_beta                 13           0.54     0.0441290510      73205
black_alpha_beta                 11           0.52     0.0425423620      58512
white_alpha_beta                 4            0.45     0.0371510010      36926
black_alpha_beta                 4            0.24     0.0194354530      29398
black_alpha_beta                 14           0.21     0.0170796020      27892
undo_black_move                  1            0.18     0.0145089880    1056122
do_black_move                    1            0.18     0.0143103530    1056122
do_white_move                    1            0.17     0.0137150880    1099301
undo_white_move                  1            0.16     0.0133215390    1099301
white_alpha_beta                 14           0.13     0.0108412100      12445
white_alpha_beta                 3            0.11     0.0093120580      12925
Ax+b-and-activation-first-layer  1            0.09     0.0070615200      10000
Ax+b-and-activation-other-layrs  1            0.08     0.0065809040      20000
black_alpha_beta                 3            0.06     0.0045703560       6968
black_alpha_beta                 15           0.04     0.0036580380       6931
white_search                     1            0.03     0.0022349000          1
white_alpha_beta                 2            0.02     0.0018429710       2717
white_alpha_beta                 15           0.02     0.0017519060       1885
return_my_timer                  1            0.02     0.0012255290       4342
black_alpha_beta                 2            0.01     0.0009325020       1391
black_alpha_beta                 16           0.01     0.0008091860       1408
solve_problems                   1            0.00     0.0004065000          1
white_alpha_beta                 1            0.00     0.0003748740        449
white_alpha_beta                 16           0.00     0.0002855490        186
test-neural                      1            0.00     0.0001859020          1
black_alpha_beta                 1            0.00     0.0001718940        142
return_neural                    1            0.00     0.0001515140      10000
black_alpha_beta                 17           0.00     0.0000562730         50
white_alpha_beta                 17           0.00     0.0000130200          6
main-thread                      1            0.00     0.0000091860          1
return_black_mcts_best_move      1            0.00     0.0000089200       1117
check_white_moves                1            0.00     0.0000079910          1
return_white_mcts_best_move      1            0.00     0.0000079280       1123
update_crc32                     1            0.00     0.0000000710          1

# Blocks sorted by self times summed over recursive invocations.
name                               perc  %main        self time      calls   self time/call ticks/call
main-final                        32.15  81.23     2.6261179450          1     2.6261179450         -1
main-init                         27.08  68.42     2.2121751540          1     2.2121751540         -1
white_alpha_beta                  18.69  47.21     1.5263349210    1056412     0.0000014448       1445
black_alpha_beta                  13.33  33.69     1.0891741870    1099588     0.0000009905        991
gen_black_moves                    2.29   5.79     0.1873295590     942900     0.0000001987        199
return_pattern_score_double        2.24   5.67     0.1833768430     180732     0.0000010146       1015
gen_white_moves                    1.75   4.42     0.1427854250     918313     0.0000001555        155
main-tests                         1.02   2.58     0.0834005580          1     0.0834005580   83400558
read_endgame                       0.54   1.37     0.0443166520    2156188     0.0000000206         21
undo_black_move                    0.18   0.45     0.0145089880    1056122     0.0000000137         14
do_black_move                      0.18   0.44     0.0143103530    1056122     0.0000000135         14
do_white_move                      0.17   0.42     0.0137150880    1099301     0.0000000125         12
undo_white_move                    0.16   0.41     0.0133215390    1099301     0.0000000121         12
Ax+b-and-activation-first-layer    0.09   0.22     0.0070615200      10000     0.0000007062        706
Ax+b-and-activation-other-layrs    0.08   0.20     0.0065809040      20000     0.0000003290        329
white_search                       0.03   0.07     0.0022349000          1     0.0022349000    2234900
return_my_timer                    0.02   0.04     0.0012255290       4342     0.0000002822        282
solve_problems                     0.00   0.01     0.0004065000          1     0.0004065000     406500
test-neural                        0.00   0.01     0.0001859020          1     0.0001859020     185902
return_neural                      0.00   0.00     0.0001515140      10000     0.0000000152         15
main-thread                        0.00   0.00     0.0000091860          1     0.0000091860       9186
return_black_mcts_best_move        0.00   0.00     0.0000089200       1117     0.0000000080          8
check_white_moves                  0.00   0.00     0.0000079910          1     0.0000079910       7991
return_white_mcts_best_move        0.00   0.00     0.0000079280       1123     0.0000000071          7
update_crc32                       0.00   0.00     0.0000000710          1     0.0000000710         71

# Summary for block main-final, invocation 1.
Spends 2.6261303780 secs in 1 call(s), or 32.15% of total execution time.
Spends 2.6261179450 secs (32.15%) in own code, 0.0000124330 secs (0.00%) in children.

Spends 0.0000124330 secs in 3 call(s) to return_my_timer, invocation 1.
No parents were found

# Summary for block main-init, invocation 1.
Spends 2.2121751540 secs in 1 call(s), or 27.08% of total execution time.
Spends 2.2121751540 secs (27.08%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
No parents were found

# Summary for block white_alpha_beta, invocation 1.
Spends 3.2300232070 secs in 449 call(s), or 39.54% of total execution time.
Spends 0.0003748740 secs (0.00%) in own code, 3.2296483330 secs (39.54%) in children.

Spends 0.0000963390 secs in 420 call(s) to gen_white_moves, invocation 1.
Spends 0.0000161500 secs in 1382 call(s) to do_white_move, invocation 1.
Spends 0.0000317100 secs in 1382 call(s) to undo_white_move, invocation 1.
Spends 0.0000073500 secs in 6 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000015510 secs in 449 call(s) to read_endgame, invocation 1.
Spends 0.0000002230 secs in 33 call(s) to return_white_mcts_best_move, invocation 1.
Spends 3.2294950100 secs in 1391 call(s) to black_alpha_beta, invocation 2.
Is called 449 time(s) from black_alpha_beta, invocation 1.

# Summary for block black_alpha_beta, invocation 1.
Spends 3.2302730730 secs in 142 call(s), or 39.54% of total execution time.
Spends 0.0001718940 secs (0.00%) in own code, 3.2301011790 secs (39.54%) in children.

Spends 0.0000100010 secs in 6 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000004620 secs in 142 call(s) to read_endgame, invocation 1.
Spends 0.0000384450 secs in 142 call(s) to gen_black_moves, invocation 1.
Spends 0.0000057240 secs in 435 call(s) to do_black_move, invocation 1.
Spends 3.2300232070 secs in 449 call(s) to white_alpha_beta, invocation 1.
Spends 0.0000230190 secs in 435 call(s) to undo_black_move, invocation 1.
Spends 0.0000003210 secs in 20 call(s) to return_black_mcts_best_move, invocation 1.
Is called 142 time(s) from white_search, invocation 1.

# Summary for block gen_black_moves, invocation 1.
Spends 0.1873295590 secs in 942900 call(s), or 2.29% of total execution time.
Spends 0.1873295590 secs (2.29%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 102 time(s) from white_search, invocation 1.
Is called 142 time(s) from black_alpha_beta, invocation 1.
Is called 1233 time(s) from black_alpha_beta, invocation 2.
Is called 5825 time(s) from black_alpha_beta, invocation 3.
Is called 20635 time(s) from black_alpha_beta, invocation 4.
Is called 54912 time(s) from black_alpha_beta, invocation 5.
Is called 117107 time(s) from black_alpha_beta, invocation 6.
Is called 172569 time(s) from black_alpha_beta, invocation 7.
Is called 168329 time(s) from black_alpha_beta, invocation 8.
Is called 101415 time(s) from black_alpha_beta, invocation 9.
Is called 56225 time(s) from black_alpha_beta, invocation 10.
Is called 57393 time(s) from black_alpha_beta, invocation 11.
Is called 78730 time(s) from black_alpha_beta, invocation 12.
Is called 72889 time(s) from black_alpha_beta, invocation 13.
Is called 27542 time(s) from black_alpha_beta, invocation 14.
Is called 6637 time(s) from black_alpha_beta, invocation 15.
Is called 1172 time(s) from black_alpha_beta, invocation 16.
Is called 43 time(s) from black_alpha_beta, invocation 17.

# Summary for block return_pattern_score_double, invocation 1.
Spends 0.1833768430 secs in 180732 call(s), or 2.24% of total execution time.
Spends 0.1833768430 secs (2.24%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 26 time(s) from white_search, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 1.
Is called 6 time(s) from white_alpha_beta, invocation 1.
Is called 91 time(s) from black_alpha_beta, invocation 2.
Is called 98 time(s) from white_alpha_beta, invocation 2.
Is called 330 time(s) from black_alpha_beta, invocation 3.
Is called 486 time(s) from white_alpha_beta, invocation 3.
Is called 1060 time(s) from black_alpha_beta, invocation 4.
Is called 2168 time(s) from white_alpha_beta, invocation 4.
Is called 3635 time(s) from black_alpha_beta, invocation 5.
Is called 5938 time(s) from white_alpha_beta, invocation 5.
Is called 10258 time(s) from black_alpha_beta, invocation 6.
Is called 13321 time(s) from white_alpha_beta, invocation 6.
Is called 16497 time(s) from black_alpha_beta, invocation 7.
Is called 21992 time(s) from white_alpha_beta, invocation 7.
Is called 18093 time(s) from black_alpha_beta, invocation 8.
Is called 20923 time(s) from white_alpha_beta, invocation 8.
Is called 13666 time(s) from black_alpha_beta, invocation 9.
Is called 15155 time(s) from white_alpha_beta, invocation 9.
Is called 7808 time(s) from black_alpha_beta, invocation 10.
Is called 8216 time(s) from white_alpha_beta, invocation 10.
Is called 4083 time(s) from black_alpha_beta, invocation 11.
Is called 4716 time(s) from white_alpha_beta, invocation 11.
Is called 3718 time(s) from black_alpha_beta, invocation 12.
Is called 4188 time(s) from white_alpha_beta, invocation 12.
Is called 1521 time(s) from black_alpha_beta, invocation 13.
Is called 1788 time(s) from white_alpha_beta, invocation 13.
Is called 635 time(s) from black_alpha_beta, invocation 14.
Is called 267 time(s) from white_alpha_beta, invocation 14.
Is called 40 time(s) from black_alpha_beta, invocation 15.
Is called 3 time(s) from white_alpha_beta, invocation 15.

# Summary for block gen_white_moves, invocation 1.
Spends 0.1427854250 secs in 918313 call(s), or 1.75% of total execution time.
Spends 0.1427854250 secs (1.75%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 1 time(s) from main-tests, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.
Is called 86 time(s) from white_search, invocation 1.
Is called 420 time(s) from white_alpha_beta, invocation 1.
Is called 2342 time(s) from white_alpha_beta, invocation 2.
Is called 9802 time(s) from white_alpha_beta, invocation 3.
Is called 30834 time(s) from white_alpha_beta, invocation 4.
Is called 74991 time(s) from white_alpha_beta, invocation 5.
Is called 140334 time(s) from white_alpha_beta, invocation 6.
Is called 179514 time(s) from white_alpha_beta, invocation 7.
Is called 138563 time(s) from white_alpha_beta, invocation 8.
Is called 72587 time(s) from white_alpha_beta, invocation 9.
Is called 58635 time(s) from white_alpha_beta, invocation 10.
Is called 65628 time(s) from white_alpha_beta, invocation 11.
Is called 83400 time(s) from white_alpha_beta, invocation 12.
Is called 46769 time(s) from white_alpha_beta, invocation 13.
Is called 12342 time(s) from white_alpha_beta, invocation 14.
Is called 1872 time(s) from white_alpha_beta, invocation 15.
Is called 186 time(s) from white_alpha_beta, invocation 16.
Is called 6 time(s) from white_alpha_beta, invocation 17.

# Summary for block main-tests, invocation 1.
Spends 0.0973813300 secs in 1 call(s), or 1.19% of total execution time.
Spends 0.0834005580 secs (1.02%) in own code, 0.0139807720 secs (0.17%) in children.

Spends 0.0000008610 secs in 1 call(s) to gen_white_moves, invocation 1.
Spends 0.0000000710 secs in 1 call(s) to update_crc32, invocation 1.
Spends 0.0139798400 secs in 1 call(s) to test-neural, invocation 1.
No parents were found

# Summary for block read_endgame, invocation 1.
Spends 0.0443166520 secs in 2156188 call(s), or 0.54% of total execution time.
Spends 0.0443166520 secs (0.54%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 189 time(s) from white_search, invocation 1.
Is called 142 time(s) from black_alpha_beta, invocation 1.
Is called 449 time(s) from white_alpha_beta, invocation 1.
Is called 1391 time(s) from black_alpha_beta, invocation 2.
Is called 2717 time(s) from white_alpha_beta, invocation 2.
Is called 6968 time(s) from black_alpha_beta, invocation 3.
Is called 12925 time(s) from white_alpha_beta, invocation 3.
Is called 29398 time(s) from black_alpha_beta, invocation 4.
Is called 36926 time(s) from white_alpha_beta, invocation 4.
Is called 87456 time(s) from black_alpha_beta, invocation 5.
Is called 92132 time(s) from white_alpha_beta, invocation 5.
Is called 165196 time(s) from black_alpha_beta, invocation 6.
Is called 186570 time(s) from white_alpha_beta, invocation 6.
Is called 213416 time(s) from black_alpha_beta, invocation 7.
Is called 219140 time(s) from white_alpha_beta, invocation 7.
Is called 183188 time(s) from black_alpha_beta, invocation 8.
Is called 153564 time(s) from white_alpha_beta, invocation 8.
Is called 106278 time(s) from black_alpha_beta, invocation 9.
Is called 77577 time(s) from white_alpha_beta, invocation 9.
Is called 58926 time(s) from black_alpha_beta, invocation 10.
Is called 61324 time(s) from white_alpha_beta, invocation 10.
Is called 58512 time(s) from black_alpha_beta, invocation 11.
Is called 67198 time(s) from white_alpha_beta, invocation 11.
Is called 79230 time(s) from black_alpha_beta, invocation 12.
Is called 84244 time(s) from white_alpha_beta, invocation 12.
Is called 73205 time(s) from black_alpha_beta, invocation 13.
Is called 47124 time(s) from white_alpha_beta, invocation 13.
Is called 27892 time(s) from black_alpha_beta, invocation 14.
Is called 12445 time(s) from white_alpha_beta, invocation 14.
Is called 6931 time(s) from black_alpha_beta, invocation 15.
Is called 1885 time(s) from white_alpha_beta, invocation 15.
Is called 1408 time(s) from black_alpha_beta, invocation 16.
Is called 186 time(s) from white_alpha_beta, invocation 16.
Is called 50 time(s) from black_alpha_beta, invocation 17.
Is called 6 time(s) from white_alpha_beta, invocation 17.

# Summary for block undo_black_move, invocation 1.
Spends 0.0145089880 secs in 1056122 call(s), or 0.18% of total execution time.
Spends 0.0145089880 secs (0.18%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 86 time(s) from white_search, invocation 1.
Is called 435 time(s) from black_alpha_beta, invocation 1.
Is called 2699 time(s) from black_alpha_beta, invocation 2.
Is called 12888 time(s) from black_alpha_beta, invocation 3.
Is called 36882 time(s) from black_alpha_beta, invocation 4.
Is called 92048 time(s) from black_alpha_beta, invocation 5.
Is called 186537 time(s) from black_alpha_beta, invocation 6.
Is called 219099 time(s) from black_alpha_beta, invocation 7.
Is called 153525 time(s) from black_alpha_beta, invocation 8.
Is called 77558 time(s) from black_alpha_beta, invocation 9.
Is called 61296 time(s) from black_alpha_beta, invocation 10.
Is called 67180 time(s) from black_alpha_beta, invocation 11.
Is called 84243 time(s) from black_alpha_beta, invocation 12.
Is called 47124 time(s) from black_alpha_beta, invocation 13.
Is called 12445 time(s) from black_alpha_beta, invocation 14.
Is called 1885 time(s) from black_alpha_beta, invocation 15.
Is called 186 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block do_black_move, invocation 1.
Spends 0.0143103530 secs in 1056122 call(s), or 0.18% of total execution time.
Spends 0.0143103530 secs (0.18%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 86 time(s) from white_search, invocation 1.
Is called 435 time(s) from black_alpha_beta, invocation 1.
Is called 2699 time(s) from black_alpha_beta, invocation 2.
Is called 12888 time(s) from black_alpha_beta, invocation 3.
Is called 36882 time(s) from black_alpha_beta, invocation 4.
Is called 92048 time(s) from black_alpha_beta, invocation 5.
Is called 186537 time(s) from black_alpha_beta, invocation 6.
Is called 219099 time(s) from black_alpha_beta, invocation 7.
Is called 153525 time(s) from black_alpha_beta, invocation 8.
Is called 77558 time(s) from black_alpha_beta, invocation 9.
Is called 61296 time(s) from black_alpha_beta, invocation 10.
Is called 67180 time(s) from black_alpha_beta, invocation 11.
Is called 84243 time(s) from black_alpha_beta, invocation 12.
Is called 47124 time(s) from black_alpha_beta, invocation 13.
Is called 12445 time(s) from black_alpha_beta, invocation 14.
Is called 1885 time(s) from black_alpha_beta, invocation 15.
Is called 186 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block do_white_move, invocation 1.
Spends 0.0137150880 secs in 1099301 call(s), or 0.17% of total execution time.
Spends 0.0137150880 secs (0.17%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 9 time(s) from check_white_moves, invocation 1.
Is called 244 time(s) from white_search, invocation 1.
Is called 1382 time(s) from white_alpha_beta, invocation 1.
Is called 6948 time(s) from white_alpha_beta, invocation 2.
Is called 29365 time(s) from white_alpha_beta, invocation 3.
Is called 87373 time(s) from white_alpha_beta, invocation 4.
Is called 165133 time(s) from white_alpha_beta, invocation 5.
Is called 213373 time(s) from white_alpha_beta, invocation 6.
Is called 183149 time(s) from white_alpha_beta, invocation 7.
Is called 106236 time(s) from white_alpha_beta, invocation 8.
Is called 58887 time(s) from white_alpha_beta, invocation 9.
Is called 58486 time(s) from white_alpha_beta, invocation 10.
Is called 79230 time(s) from white_alpha_beta, invocation 11.
Is called 73205 time(s) from white_alpha_beta, invocation 12.
Is called 27892 time(s) from white_alpha_beta, invocation 13.
Is called 6931 time(s) from white_alpha_beta, invocation 14.
Is called 1408 time(s) from white_alpha_beta, invocation 15.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block undo_white_move, invocation 1.
Spends 0.0133215390 secs in 1099301 call(s), or 0.16% of total execution time.
Spends 0.0133215390 secs (0.16%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 9 time(s) from check_white_moves, invocation 1.
Is called 244 time(s) from white_search, invocation 1.
Is called 1382 time(s) from white_alpha_beta, invocation 1.
Is called 6948 time(s) from white_alpha_beta, invocation 2.
Is called 29365 time(s) from white_alpha_beta, invocation 3.
Is called 87373 time(s) from white_alpha_beta, invocation 4.
Is called 165133 time(s) from white_alpha_beta, invocation 5.
Is called 213373 time(s) from white_alpha_beta, invocation 6.
Is called 183149 time(s) from white_alpha_beta, invocation 7.
Is called 106236 time(s) from white_alpha_beta, invocation 8.
Is called 58887 time(s) from white_alpha_beta, invocation 9.
Is called 58486 time(s) from white_alpha_beta, invocation 10.
Is called 79230 time(s) from white_alpha_beta, invocation 11.
Is called 73205 time(s) from white_alpha_beta, invocation 12.
Is called 27892 time(s) from white_alpha_beta, invocation 13.
Is called 6931 time(s) from white_alpha_beta, invocation 14.
Is called 1408 time(s) from white_alpha_beta, invocation 15.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block Ax+b-and-activation-first-layer, invocation 1.
Spends 0.0070615200 secs in 10000 call(s), or 0.09% of total execution time.
Spends 0.0070615200 secs (0.09%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 10000 time(s) from return_neural, invocation 1.

# Summary for block Ax+b-and-activation-other-layrs, invocation 1.
Spends 0.0065809040 secs in 20000 call(s), or 0.08% of total execution time.
Spends 0.0065809040 secs (0.08%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 20000 time(s) from return_neural, invocation 1.

# Summary for block white_search, invocation 1.
Spends 3.2326368230 secs in 1 call(s), or 39.57% of total execution time.
Spends 0.0022349000 secs (0.03%) in own code, 3.2304019230 secs (39.55%) in children.

Spends 0.0000196770 secs in 86 call(s) to gen_white_moves, invocation 1.
Spends 0.0000056050 secs in 244 call(s) to do_white_move, invocation 1.
Spends 0.0000164320 secs in 244 call(s) to undo_white_move, invocation 1.
Spends 0.0000488590 secs in 26 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000005510 secs in 189 call(s) to read_endgame, invocation 1.
Spends 0.0000002660 secs in 16 call(s) to return_white_mcts_best_move, invocation 1.
Spends 3.2302730730 secs in 142 call(s) to black_alpha_beta, invocation 1.
Spends 0.0000275220 secs in 102 call(s) to gen_black_moves, invocation 1.
Spends 0.0000081580 secs in 27 call(s) to return_my_timer, invocation 1.
Spends 0.0000008260 secs in 86 call(s) to do_black_move, invocation 1.
Spends 0.0000009540 secs in 86 call(s) to undo_black_move, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.

# Summary for block return_my_timer, invocation 1.
Spends 0.0012255290 secs in 4342 call(s), or 0.02% of total execution time.
Spends 0.0012255290 secs (0.02%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 27 time(s) from white_search, invocation 1.
Is called 2 time(s) from black_alpha_beta, invocation 2.
Is called 4 time(s) from white_alpha_beta, invocation 2.
Is called 10 time(s) from black_alpha_beta, invocation 3.
Is called 24 time(s) from white_alpha_beta, invocation 3.
Is called 50 time(s) from black_alpha_beta, invocation 4.
Is called 70 time(s) from white_alpha_beta, invocation 4.
Is called 190 time(s) from black_alpha_beta, invocation 5.
Is called 168 time(s) from white_alpha_beta, invocation 5.
Is called 348 time(s) from black_alpha_beta, invocation 6.
Is called 398 time(s) from white_alpha_beta, invocation 6.
Is called 418 time(s) from black_alpha_beta, invocation 7.
Is called 432 time(s) from white_alpha_beta, invocation 7.
Is called 360 time(s) from black_alpha_beta, invocation 8.
Is called 314 time(s) from white_alpha_beta, invocation 8.
Is called 194 time(s) from black_alpha_beta, invocation 9.
Is called 132 time(s) from white_alpha_beta, invocation 9.
Is called 134 time(s) from black_alpha_beta, invocation 10.
Is called 118 time(s) from white_alpha_beta, invocation 10.
Is called 96 time(s) from black_alpha_beta, invocation 11.
Is called 136 time(s) from white_alpha_beta, invocation 11.
Is called 158 time(s) from black_alpha_beta, invocation 12.
Is called 202 time(s) from white_alpha_beta, invocation 12.
Is called 144 time(s) from black_alpha_beta, invocation 13.
Is called 96 time(s) from white_alpha_beta, invocation 13.
Is called 64 time(s) from black_alpha_beta, invocation 14.
Is called 28 time(s) from white_alpha_beta, invocation 14.
Is called 18 time(s) from black_alpha_beta, invocation 15.
Is called 2 time(s) from white_alpha_beta, invocation 15.
Is called 2 time(s) from black_alpha_beta, invocation 16.
Is called 3 time(s) from main-final, invocation 1.

# Summary for block solve_problems, invocation 1.
Spends 3.2330520290 secs in 1 call(s), or 39.58% of total execution time.
Spends 0.0004065000 secs (0.00%) in own code, 3.2326455290 secs (39.57%) in children.

Spends 0.0000004160 secs in 1 call(s) to gen_white_moves, invocation 1.
Spends 0.0000082900 secs in 1 call(s) to check_white_moves, invocation 1.
Spends 3.2326368230 secs in 1 call(s) to white_search, invocation 1.
Is called 1 time(s) from main-thread, invocation 1.

# Summary for block test-neural, invocation 1.
Spends 0.0139798400 secs in 1 call(s), or 0.17% of total execution time.
Spends 0.0001859020 secs (0.00%) in own code, 0.0137939380 secs (0.17%) in children.

Spends 0.0137939380 secs in 10000 call(s) to return_neural, invocation 1.
Is called 1 time(s) from main-tests, invocation 1.

# Summary for block return_neural, invocation 1.
Spends 0.0137939380 secs in 10000 call(s), or 0.17% of total execution time.
Spends 0.0001515140 secs (0.00%) in own code, 0.0136424240 secs (0.17%) in children.

Spends 0.0070615200 secs in 10000 call(s) to Ax+b-and-activation-first-layer, invocation 1.
Spends 0.0065809040 secs in 20000 call(s) to Ax+b-and-activation-other-layrs, invocation 1.
Is called 10000 time(s) from test-neural, invocation 1.

# Summary for block main-thread, invocation 1.
Spends 3.2330612150 secs in 1 call(s), or 39.58% of total execution time.
Spends 0.0000091860 secs (0.00%) in own code, 3.2330520290 secs (39.58%) in children.

Spends 3.2330520290 secs in 1 call(s) to solve_problems, invocation 1.
No parents were found

# Summary for block return_black_mcts_best_move, invocation 1.
Spends 0.0000089200 secs in 1117 call(s), or 0.00% of total execution time.
Spends 0.0000089200 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 20 time(s) from black_alpha_beta, invocation 1.
Is called 35 time(s) from black_alpha_beta, invocation 2.
Is called 55 time(s) from black_alpha_beta, invocation 3.
Is called 93 time(s) from black_alpha_beta, invocation 4.
Is called 198 time(s) from black_alpha_beta, invocation 5.
Is called 192 time(s) from black_alpha_beta, invocation 6.
Is called 145 time(s) from black_alpha_beta, invocation 7.
Is called 125 time(s) from black_alpha_beta, invocation 8.
Is called 101 time(s) from black_alpha_beta, invocation 9.
Is called 66 time(s) from black_alpha_beta, invocation 10.
Is called 57 time(s) from black_alpha_beta, invocation 11.
Is called 26 time(s) from black_alpha_beta, invocation 12.
Is called 4 time(s) from black_alpha_beta, invocation 13.

# Summary for block check_white_moves, invocation 1.
Spends 0.0000082900 secs in 1 call(s), or 0.00% of total execution time.
Spends 0.0000079910 secs (0.00%) in own code, 0.0000002990 secs (0.00%) in children.

Spends 0.0000002480 secs in 9 call(s) to do_white_move, invocation 1.
Spends 0.0000000510 secs in 9 call(s) to undo_white_move, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.

# Summary for block return_white_mcts_best_move, invocation 1.
Spends 0.0000079280 secs in 1123 call(s), or 0.00% of total execution time.
Spends 0.0000079280 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 16 time(s) from white_search, invocation 1.
Is called 33 time(s) from white_alpha_beta, invocation 1.
Is called 50 time(s) from white_alpha_beta, invocation 2.
Is called 76 time(s) from white_alpha_beta, invocation 3.
Is called 122 time(s) from white_alpha_beta, invocation 4.
Is called 200 time(s) from white_alpha_beta, invocation 5.
Is called 163 time(s) from white_alpha_beta, invocation 6.
Is called 136 time(s) from white_alpha_beta, invocation 7.
Is called 134 time(s) from white_alpha_beta, invocation 8.
Is called 86 time(s) from white_alpha_beta, invocation 9.
Is called 62 time(s) from white_alpha_beta, invocation 10.
Is called 32 time(s) from white_alpha_beta, invocation 11.
Is called 13 time(s) from white_alpha_beta, invocation 12.

# Summary for block update_crc32, invocation 1.
Spends 0.0000000710 secs in 1 call(s), or 0.00% of total execution time.
Spends 0.0000000710 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 1 time(s) from main-tests, invocation 1.

# Summary for block white_alpha_beta, invocation 2.
Spends 3.2280448340 secs in 2717 call(s), or 39.52% of total execution time.
Spends 0.0018429710 secs (0.02%) in own code, 3.2262018630 secs (39.49%) in children.

Spends 0.0004888030 secs in 2342 call(s) to gen_white_moves, invocation 1.
Spends 0.0000740530 secs in 6948 call(s) to do_white_move, invocation 1.
Spends 0.0001150240 secs in 6948 call(s) to undo_white_move, invocation 1.
Spends 0.0001007830 secs in 98 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000086670 secs in 2717 call(s) to read_endgame, invocation 1.
Spends 0.0000002350 secs in 50 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000010700 secs in 4 call(s) to return_my_timer, invocation 1.
Spends 3.2254132280 secs in 6968 call(s) to black_alpha_beta, invocation 3.
Is called 2717 time(s) from black_alpha_beta, invocation 2.

# Summary for block black_alpha_beta, invocation 3.
Spends 3.2254132280 secs in 6968 call(s), or 39.48% of total execution time.
Spends 0.0045703560 secs (0.06%) in own code, 3.2208428720 secs (39.43%) in children.

Spends 0.0003485760 secs in 330 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000210820 secs in 6968 call(s) to read_endgame, invocation 1.
Spends 0.0014430350 secs in 5825 call(s) to gen_black_moves, invocation 1.
Spends 0.0000031470 secs in 10 call(s) to return_my_timer, invocation 1.
Spends 0.0001485160 secs in 12888 call(s) to do_black_move, invocation 1.
Spends 0.0002138820 secs in 12888 call(s) to undo_black_move, invocation 1.
Spends 0.0000004700 secs in 55 call(s) to return_black_mcts_best_move, invocation 1.
Spends 3.2186641640 secs in 12925 call(s) to white_alpha_beta, invocation 3.
Is called 6968 time(s) from white_alpha_beta, invocation 2.

# Summary for block white_alpha_beta, invocation 3.
Spends 3.2186641640 secs in 12925 call(s), or 39.40% of total execution time.
Spends 0.0093120580 secs (0.11%) in own code, 3.2093521060 secs (39.29%) in children.

Spends 0.0019686750 secs in 9802 call(s) to gen_white_moves, invocation 1.
Spends 0.0003051750 secs in 29365 call(s) to do_white_move, invocation 1.
Spends 0.0003924150 secs in 29365 call(s) to undo_white_move, invocation 1.
Spends 0.0005131430 secs in 486 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000494730 secs in 12925 call(s) to read_endgame, invocation 1.
Spends 0.0000002830 secs in 76 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000074610 secs in 24 call(s) to return_my_timer, invocation 1.
Spends 3.2061154810 secs in 29398 call(s) to black_alpha_beta, invocation 4.
Is called 12925 time(s) from black_alpha_beta, invocation 3.

# Summary for block black_alpha_beta, invocation 4.
Spends 3.2061154810 secs in 29398 call(s), or 39.25% of total execution time.
Spends 0.0194354530 secs (0.24%) in own code, 3.1866800280 secs (39.01%) in children.

Spends 0.0011375780 secs in 1060 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000960060 secs in 29398 call(s) to read_endgame, invocation 1.
Spends 0.0049145420 secs in 20635 call(s) to gen_black_moves, invocation 1.
Spends 0.0000145600 secs in 50 call(s) to return_my_timer, invocation 1.
Spends 0.0004661850 secs in 36882 call(s) to do_black_move, invocation 1.
Spends 0.0005021870 secs in 36882 call(s) to undo_black_move, invocation 1.
Spends 0.0000010740 secs in 93 call(s) to return_black_mcts_best_move, invocation 1.
Spends 3.1795478960 secs in 36926 call(s) to white_alpha_beta, invocation 4.
Is called 29398 time(s) from white_alpha_beta, invocation 3.

# Summary for block white_alpha_beta, invocation 4.
Spends 3.1795478960 secs in 36926 call(s), or 38.92% of total execution time.
Spends 0.0371510010 secs (0.45%) in own code, 3.1423968950 secs (38.47%) in children.

Spends 0.0059376690 secs in 30834 call(s) to gen_white_moves, invocation 1.
Spends 0.0008801350 secs in 87373 call(s) to do_white_move, invocation 1.
Spends 0.0009325750 secs in 87373 call(s) to undo_white_move, invocation 1.
Spends 0.0023972520 secs in 2168 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001207650 secs in 36926 call(s) to read_endgame, invocation 1.
Spends 0.0000005000 secs in 122 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000190800 secs in 70 call(s) to return_my_timer, invocation 1.
Spends 3.1321089190 secs in 87456 call(s) to black_alpha_beta, invocation 5.
Is called 36926 time(s) from black_alpha_beta, invocation 4.

# Summary for block black_alpha_beta, invocation 5.
Spends 3.1321089190 secs in 87456 call(s), or 38.34% of total execution time.
Spends 0.0641563150 secs (0.79%) in own code, 3.0679526040 secs (37.56%) in children.

Spends 0.0039736490 secs in 3635 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002763300 secs in 87456 call(s) to read_endgame, invocation 1.
Spends 0.0126311680 secs in 54912 call(s) to gen_black_moves, invocation 1.
Spends 0.0000536760 secs in 190 call(s) to return_my_timer, invocation 1.
Spends 0.0022113680 secs in 92048 call(s) to do_black_move, invocation 1.
Spends 0.0012051180 secs in 92048 call(s) to undo_black_move, invocation 1.
Spends 0.0000020540 secs in 198 call(s) to return_black_mcts_best_move, invocation 1.
Spends 3.0475992410 secs in 92132 call(s) to white_alpha_beta, invocation 5.
Is called 87456 time(s) from white_alpha_beta, invocation 4.

# Summary for block white_alpha_beta, invocation 5.
Spends 3.0475992410 secs in 92132 call(s), or 37.31% of total execution time.
Spends 0.1068350240 secs (1.31%) in own code, 2.9407642170 secs (36.00%) in children.

Spends 0.0137180900 secs in 74991 call(s) to gen_white_moves, invocation 1.
Spends 0.0026292590 secs in 165133 call(s) to do_white_move, invocation 1.
Spends 0.0017863580 secs in 165133 call(s) to undo_white_move, invocation 1.
Spends 0.0064476320 secs in 5938 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003317500 secs in 92132 call(s) to read_endgame, invocation 1.
Spends 0.0000008680 secs in 200 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000482850 secs in 168 call(s) to return_my_timer, invocation 1.
Spends 2.9158019750 secs in 165196 call(s) to black_alpha_beta, invocation 6.
Is called 92132 time(s) from black_alpha_beta, invocation 5.

# Summary for block black_alpha_beta, invocation 6.
Spends 2.9158019750 secs in 165196 call(s), or 35.69% of total execution time.
Spends 0.1455017210 secs (1.78%) in own code, 2.7703002540 secs (33.91%) in children.

Spends 0.0108519700 secs in 10258 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0005424700 secs in 165196 call(s) to read_endgame, invocation 1.
Spends 0.0257860210 secs in 117107 call(s) to gen_black_moves, invocation 1.
Spends 0.0000972420 secs in 348 call(s) to return_my_timer, invocation 1.
Spends 0.0021825620 secs in 186537 call(s) to do_black_move, invocation 1.
Spends 0.0022458690 secs in 186537 call(s) to undo_black_move, invocation 1.
Spends 0.0000012270 secs in 192 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.7285928930 secs in 186570 call(s) to white_alpha_beta, invocation 6.
Is called 165196 time(s) from white_alpha_beta, invocation 5.

# Summary for block white_alpha_beta, invocation 6.
Spends 2.7285928930 secs in 186570 call(s), or 33.40% of total execution time.
Spends 0.2391644960 secs (2.93%) in own code, 2.4894283970 secs (30.48%) in children.

Spends 0.0244363120 secs in 140334 call(s) to gen_white_moves, invocation 1.
Spends 0.0024406400 secs in 213373 call(s) to do_white_move, invocation 1.
Spends 0.0024393940 secs in 213373 call(s) to undo_white_move, invocation 1.
Spends 0.0138846800 secs in 13321 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0006609950 secs in 186570 call(s) to read_endgame, invocation 1.
Spends 0.0000036320 secs in 163 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0001129400 secs in 398 call(s) to return_my_timer, invocation 1.
Spends 2.4454498040 secs in 213416 call(s) to black_alpha_beta, invocation 7.
Is called 186570 time(s) from black_alpha_beta, invocation 6.

# Summary for block black_alpha_beta, invocation 7.
Spends 2.4454498040 secs in 213416 call(s), or 29.94% of total execution time.
Spends 0.2295304000 secs (2.81%) in own code, 2.2159194040 secs (27.13%) in children.

Spends 0.0177879010 secs in 16497 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0007219450 secs in 213416 call(s) to read_endgame, invocation 1.
Spends 0.0359185530 secs in 172569 call(s) to gen_black_moves, invocation 1.
Spends 0.0001162720 secs in 418 call(s) to return_my_timer, invocation 1.
Spends 0.0027086950 secs in 219099 call(s) to do_black_move, invocation 1.
Spends 0.0028203170 secs in 219099 call(s) to undo_black_move, invocation 1.
Spends 0.0000008070 secs in 145 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.1558449140 secs in 219140 call(s) to white_alpha_beta, invocation 7.
Is called 213416 time(s) from white_alpha_beta, invocation 6.

# Summary for block white_alpha_beta, invocation 7.
Spends 2.1558449140 secs in 219140 call(s), or 26.39% of total execution time.
Spends 0.3661916710 secs (4.48%) in own code, 1.7896532430 secs (21.91%) in children.

Spends 0.0292944240 secs in 179514 call(s) to gen_white_moves, invocation 1.
Spends 0.0023277730 secs in 183149 call(s) to do_white_move, invocation 1.
Spends 0.0023982260 secs in 183149 call(s) to undo_white_move, invocation 1.
Spends 0.0248075830 secs in 21992 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0007759710 secs in 219140 call(s) to read_endgame, invocation 1.
Spends 0.0000005990 secs in 136 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0001208440 secs in 432 call(s) to return_my_timer, invocation 1.
Spends 1.7299278230 secs in 183189 call(s) to black_alpha_beta, invocation 8.
Is called 219140 time(s) from black_alpha_beta, invocation 7.

# Summary for block black_alpha_beta, invocation 8.
Spends 1.7299278230 secs in 183189 call(s), or 21.18% of total execution time.
Spends 0.2413565070 secs (2.95%) in own code, 1.4885713160 secs (18.22%) in children.

Spends 0.0180059130 secs in 18093 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0006305690 secs in 183188 call(s) to read_endgame, invocation 1.
Spends 0.0330967230 secs in 168329 call(s) to gen_black_moves, invocation 1.
Spends 0.0001015580 secs in 360 call(s) to return_my_timer, invocation 1.
Spends 0.0020268220 secs in 153525 call(s) to do_black_move, invocation 1.
Spends 0.0029257750 secs in 153525 call(s) to undo_black_move, invocation 1.
Spends 0.0000007830 secs in 125 call(s) to return_black_mcts_best_move, invocation 1.
Spends 1.4317831730 secs in 153564 call(s) to white_alpha_beta, invocation 8.
Is called 183189 time(s) from white_alpha_beta, invocation 7.

# Summary for block white_alpha_beta, invocation 8.
Spends 1.4317831730 secs in 153564 call(s), or 17.53% of total execution time.
Spends 0.3032134870 secs (3.71%) in own code, 1.1285696860 secs (13.82%) in children.

Spends 0.0209996990 secs in 138563 call(s) to gen_white_moves, invocation 1.
Spends 0.0013987380 secs in 106236 call(s) to do_white_move, invocation 1.
Spends 0.0014967930 secs in 106236 call(s) to undo_white_move, invocation 1.
Spends 0.0198312980 secs in 20923 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0005560490 secs in 153564 call(s) to read_endgame, invocation 1.
Spends 0.0000005880 secs in 134 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000899340 secs in 314 call(s) to return_my_timer, invocation 1.
Spends 1.0841965870 secs in 106278 call(s) to black_alpha_beta, invocation 9.
Is called 153564 time(s) from black_alpha_beta, invocation 8.

# Summary for block black_alpha_beta, invocation 9.
Spends 1.0841965870 secs in 106278 call(s), or 13.27% of total execution time.
Spends 0.1498701440 secs (1.83%) in own code, 0.9343264430 secs (11.44%) in children.

Spends 0.0130256280 secs in 13666 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003883700 secs in 106278 call(s) to read_endgame, invocation 1.
Spends 0.0192241000 secs in 101415 call(s) to gen_black_moves, invocation 1.
Spends 0.0000543510 secs in 194 call(s) to return_my_timer, invocation 1.
Spends 0.0012664830 secs in 77558 call(s) to do_black_move, invocation 1.
Spends 0.0013144050 secs in 77558 call(s) to undo_black_move, invocation 1.
Spends 0.0000006340 secs in 101 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.8990524720 secs in 77577 call(s) to white_alpha_beta, invocation 9.
Is called 106278 time(s) from white_alpha_beta, invocation 8.

# Summary for block white_alpha_beta, invocation 9.
Spends 0.8990524720 secs in 77577 call(s), or 11.01% of total execution time.
Spends 0.1620656130 secs (1.98%) in own code, 0.7369868590 secs (9.02%) in children.

Spends 0.0105857840 secs in 72587 call(s) to gen_white_moves, invocation 1.
Spends 0.0007426750 secs in 58887 call(s) to do_white_move, invocation 1.
Spends 0.0008337310 secs in 58887 call(s) to undo_white_move, invocation 1.
Spends 0.0140252270 secs in 15155 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003714260 secs in 77577 call(s) to read_endgame, invocation 1.
Spends 0.0000003220 secs in 86 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000365300 secs in 132 call(s) to return_my_timer, invocation 1.
Spends 0.7103911640 secs in 58926 call(s) to black_alpha_beta, invocation 10.
Is called 77577 time(s) from black_alpha_beta, invocation 9.

# Summary for block black_alpha_beta, invocation 10.
Spends 0.7103911640 secs in 58926 call(s), or 8.70% of total execution time.
Spends 0.0716554880 secs (0.88%) in own code, 0.6387356760 secs (7.82%) in children.

Spends 0.0073626720 secs in 7808 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002246990 secs in 58926 call(s) to read_endgame, invocation 1.
Spends 0.0107262050 secs in 56225 call(s) to gen_black_moves, invocation 1.
Spends 0.0000376500 secs in 134 call(s) to return_my_timer, invocation 1.
Spends 0.0007258440 secs in 61296 call(s) to do_black_move, invocation 1.
Spends 0.0007283320 secs in 61296 call(s) to undo_black_move, invocation 1.
Spends 0.0000005570 secs in 66 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.6189297170 secs in 61324 call(s) to white_alpha_beta, invocation 10.
Is called 58926 time(s) from white_alpha_beta, invocation 9.

# Summary for block white_alpha_beta, invocation 10.
Spends 0.6189297170 secs in 61324 call(s), or 7.58% of total execution time.
Spends 0.0816857360 secs (1.00%) in own code, 0.5372439810 secs (6.58%) in children.

Spends 0.0086454550 secs in 58635 call(s) to gen_white_moves, invocation 1.
Spends 0.0007201860 secs in 58486 call(s) to do_white_move, invocation 1.
Spends 0.0007149660 secs in 58486 call(s) to undo_white_move, invocation 1.
Spends 0.0075353590 secs in 8216 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003621990 secs in 61324 call(s) to read_endgame, invocation 1.
Spends 0.0000002760 secs in 62 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000319280 secs in 118 call(s) to return_my_timer, invocation 1.
Spends 0.5192336120 secs in 58512 call(s) to black_alpha_beta, invocation 11.
Is called 61324 time(s) from black_alpha_beta, invocation 10.

# Summary for block black_alpha_beta, invocation 11.
Spends 0.5192336120 secs in 58512 call(s), or 6.36% of total execution time.
Spends 0.0425423620 secs (0.52%) in own code, 0.4766912500 secs (5.84%) in children.

Spends 0.0037585320 secs in 4083 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003151320 secs in 58512 call(s) to read_endgame, invocation 1.
Spends 0.0107149240 secs in 57393 call(s) to gen_black_moves, invocation 1.
Spends 0.0000259910 secs in 96 call(s) to return_my_timer, invocation 1.
Spends 0.0007559890 secs in 67180 call(s) to do_black_move, invocation 1.
Spends 0.0007550820 secs in 67180 call(s) to undo_black_move, invocation 1.
Spends 0.0000003980 secs in 57 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.4603652020 secs in 67198 call(s) to white_alpha_beta, invocation 11.
Is called 58512 time(s) from white_alpha_beta, invocation 10.

# Summary for block white_alpha_beta, invocation 11.
Spends 0.4603652020 secs in 67198 call(s), or 5.64% of total execution time.
Spends 0.0740875910 secs (0.91%) in own code, 0.3862776110 secs (4.73%) in children.

Spends 0.0091530970 secs in 65628 call(s) to gen_white_moves, invocation 1.
Spends 0.0009133060 secs in 79230 call(s) to do_white_move, invocation 1.
Spends 0.0009593570 secs in 79230 call(s) to undo_white_move, invocation 1.
Spends 0.0044999220 secs in 4716 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0130142770 secs in 67198 call(s) to read_endgame, invocation 1.
Spends 0.0000001200 secs in 32 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000368700 secs in 136 call(s) to return_my_timer, invocation 1.
Spends 0.3577006620 secs in 79230 call(s) to black_alpha_beta, invocation 12.
Is called 67198 time(s) from black_alpha_beta, invocation 11.

# Summary for block black_alpha_beta, invocation 12.
Spends 0.3577006620 secs in 79230 call(s), or 4.38% of total execution time.
Spends 0.0537188950 secs (0.66%) in own code, 0.3039817670 secs (3.72%) in children.

Spends 0.0034903140 secs in 3718 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0117404220 secs in 79230 call(s) to read_endgame, invocation 1.
Spends 0.0142198220 secs in 78730 call(s) to gen_black_moves, invocation 1.
Spends 0.0000426690 secs in 158 call(s) to return_my_timer, invocation 1.
Spends 0.0009499170 secs in 84243 call(s) to do_black_move, invocation 1.
Spends 0.0009629020 secs in 84243 call(s) to undo_black_move, invocation 1.
Spends 0.0000001240 secs in 26 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.2725755970 secs in 84244 call(s) to white_alpha_beta, invocation 12.
Is called 79230 time(s) from white_alpha_beta, invocation 11.

# Summary for block white_alpha_beta, invocation 12.
Spends 0.2725755970 secs in 84244 call(s), or 3.34% of total execution time.
Spends 0.0856674220 secs (1.05%) in own code, 0.1869081750 secs (2.29%) in children.

Spends 0.0109916540 secs in 83400 call(s) to gen_white_moves, invocation 1.
Spends 0.0008736800 secs in 73205 call(s) to do_white_move, invocation 1.
Spends 0.0008161010 secs in 73205 call(s) to undo_white_move, invocation 1.
Spends 0.0041933160 secs in 4188 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0123275110 secs in 84244 call(s) to read_endgame, invocation 1.
Spends 0.0000000160 secs in 13 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000564890 secs in 202 call(s) to return_my_timer, invocation 1.
Spends 0.1576494080 secs in 73205 call(s) to black_alpha_beta, invocation 13.
Is called 84244 time(s) from black_alpha_beta, invocation 12.

# Summary for block black_alpha_beta, invocation 13.
Spends 0.1576494080 secs in 73205 call(s), or 1.93% of total execution time.
Spends 0.0441290510 secs (0.54%) in own code, 0.1135203570 secs (1.39%) in children.

Spends 0.0026698900 secs in 1521 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003107390 secs in 73205 call(s) to read_endgame, invocation 1.
Spends 0.0123447220 secs in 72889 call(s) to gen_black_moves, invocation 1.
Spends 0.0000393520 secs in 144 call(s) to return_my_timer, invocation 1.
Spends 0.0006281840 secs in 47124 call(s) to do_black_move, invocation 1.
Spends 0.0005679050 secs in 47124 call(s) to undo_black_move, invocation 1.
Spends 0.0000000270 secs in 4 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.0969595380 secs in 47124 call(s) to white_alpha_beta, invocation 13.
Is called 73205 time(s) from white_alpha_beta, invocation 12.

# Summary for block white_alpha_beta, invocation 13.
Spends 0.0969595380 secs in 47124 call(s), or 1.19% of total execution time.
Spends 0.0458512920 secs (0.56%) in own code, 0.0511082460 secs (0.63%) in children.

Spends 0.0051647450 secs in 46769 call(s) to gen_white_moves, invocation 1.
Spends 0.0003050210 secs in 27892 call(s) to do_white_move, invocation 1.
Spends 0.0003096890 secs in 27892 call(s) to undo_white_move, invocation 1.
Spends 0.0016981380 secs in 1788 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002434160 secs in 47124 call(s) to read_endgame, invocation 1.
Spends 0.0000257690 secs in 96 call(s) to return_my_timer, invocation 1.
Spends 0.0433614680 secs in 27892 call(s) to black_alpha_beta, invocation 14.
Is called 47124 time(s) from black_alpha_beta, invocation 13.

# Summary for block black_alpha_beta, invocation 14.
Spends 0.0433614680 secs in 27892 call(s), or 0.53% of total execution time.
Spends 0.0170796020 secs (0.21%) in own code, 0.0262818660 secs (0.32%) in children.

Spends 0.0005670410 secs in 635 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001085830 secs in 27892 call(s) to read_endgame, invocation 1.
Spends 0.0045094100 secs in 27542 call(s) to gen_black_moves, invocation 1.
Spends 0.0000174040 secs in 64 call(s) to return_my_timer, invocation 1.
Spends 0.0001731060 secs in 12445 call(s) to do_black_move, invocation 1.
Spends 0.0001629450 secs in 12445 call(s) to undo_black_move, invocation 1.
Spends 0.0207433770 secs in 12445 call(s) to white_alpha_beta, invocation 14.
Is called 27892 time(s) from white_alpha_beta, invocation 13.

# Summary for block white_alpha_beta, invocation 14.
Spends 0.0207433770 secs in 12445 call(s), or 0.25% of total execution time.
Spends 0.0108412100 secs (0.13%) in own code, 0.0099021670 secs (0.12%) in children.

Spends 0.0011007980 secs in 12342 call(s) to gen_white_moves, invocation 1.
Spends 0.0000696860 secs in 6931 call(s) to do_white_move, invocation 1.
Spends 0.0000656670 secs in 6931 call(s) to undo_white_move, invocation 1.
Spends 0.0002559260 secs in 267 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000547680 secs in 12445 call(s) to read_endgame, invocation 1.
Spends 0.0000074910 secs in 28 call(s) to return_my_timer, invocation 1.
Spends 0.0083478310 secs in 6931 call(s) to black_alpha_beta, invocation 15.
Is called 12445 time(s) from black_alpha_beta, invocation 14.

# Summary for block black_alpha_beta, invocation 15.
Spends 0.0083478310 secs in 6931 call(s), or 0.10% of total execution time.
Spends 0.0036580380 secs (0.04%) in own code, 0.0046897930 secs (0.06%) in children.

Spends 0.0000384740 secs in 40 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000353560 secs in 6931 call(s) to read_endgame, invocation 1.
Spends 0.0011784490 secs in 6637 call(s) to gen_black_moves, invocation 1.
Spends 0.0000048120 secs in 18 call(s) to return_my_timer, invocation 1.
Spends 0.0000250620 secs in 1885 call(s) to do_black_move, invocation 1.
Spends 0.0000228660 secs in 1885 call(s) to undo_black_move, invocation 1.
Spends 0.0033847740 secs in 1885 call(s) to white_alpha_beta, invocation 15.
Is called 6931 time(s) from white_alpha_beta, invocation 14.

# Summary for block white_alpha_beta, invocation 15.
Spends 0.0033847740 secs in 1885 call(s), or 0.04% of total execution time.
Spends 0.0017519060 secs (0.02%) in own code, 0.0016328680 secs (0.02%) in children.

Spends 0.0001701800 secs in 1872 call(s) to gen_white_moves, invocation 1.
Spends 0.0000122580 secs in 1408 call(s) to do_white_move, invocation 1.
Spends 0.0000125530 secs in 1408 call(s) to undo_white_move, invocation 1.
Spends 0.0000036900 secs in 3 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000099370 secs in 1885 call(s) to read_endgame, invocation 1.
Spends 0.0000005250 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0014237250 secs in 1408 call(s) to black_alpha_beta, invocation 16.
Is called 1885 time(s) from black_alpha_beta, invocation 15.

# Summary for block black_alpha_beta, invocation 16.
Spends 0.0014237250 secs in 1408 call(s), or 0.02% of total execution time.
Spends 0.0008091860 secs (0.01%) in own code, 0.0006145390 secs (0.01%) in children.

Spends 0.0000057930 secs in 1408 call(s) to read_endgame, invocation 1.
Spends 0.0002238470 secs in 1172 call(s) to gen_black_moves, invocation 1.
Spends 0.0000005260 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0000026640 secs in 186 call(s) to do_black_move, invocation 1.
Spends 0.0000026120 secs in 186 call(s) to undo_black_move, invocation 1.
Spends 0.0003790970 secs in 186 call(s) to white_alpha_beta, invocation 16.
Is called 1408 time(s) from white_alpha_beta, invocation 15.

# Summary for block white_alpha_beta, invocation 16.
Spends 0.0003790970 secs in 186 call(s), or 0.00% of total execution time.
Spends 0.0002855490 secs (0.00%) in own code, 0.0000935480 secs (0.00%) in children.

Spends 0.0000125040 secs in 186 call(s) to gen_white_moves, invocation 1.
Spends 0.0000005000 secs in 50 call(s) to do_white_move, invocation 1.
Spends 0.0000004970 secs in 50 call(s) to undo_white_move, invocation 1.
Spends 0.0000007940 secs in 186 call(s) to read_endgame, invocation 1.
Spends 0.0000792530 secs in 50 call(s) to black_alpha_beta, invocation 17.
Is called 186 time(s) from black_alpha_beta, invocation 16.

# Summary for block black_alpha_beta, invocation 17.
Spends 0.0000792530 secs in 50 call(s), or 0.00% of total execution time.
Spends 0.0000562730 secs (0.00%) in own code, 0.0000229800 secs (0.00%) in children.

Spends 0.0000001760 secs in 50 call(s) to read_endgame, invocation 1.
Spends 0.0000093700 secs in 43 call(s) to gen_black_moves, invocation 1.
Spends 0.0000000940 secs in 6 call(s) to do_black_move, invocation 1.
Spends 0.0000000690 secs in 6 call(s) to undo_black_move, invocation 1.
Spends 0.0000132710 secs in 6 call(s) to white_alpha_beta, invocation 17.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block white_alpha_beta, invocation 17.
Spends 0.0000132710 secs in 6 call(s), or 0.00% of total execution time.
Spends 0.0000130200 secs (0.00%) in own code, 0.0000002510 secs (0.00%) in children.

Spends 0.0000002430 secs in 6 call(s) to gen_white_moves, invocation 1.
Spends 0.0000000080 secs in 6 call(s) to read_endgame, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block black_alpha_beta, invocation 2.
Spends 3.2294950100 secs in 1391 call(s), or 39.53% of total execution time.
Spends 0.0009325020 secs (0.01%) in own code, 3.2285625080 secs (39.52%) in children.

Spends 0.0000985460 secs in 91 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000084100 secs in 1391 call(s) to read_endgame, invocation 1.
Spends 0.0003227010 secs in 1233 call(s) to gen_black_moves, invocation 1.
Spends 0.0000005120 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0000323120 secs in 2699 call(s) to do_black_move, invocation 1.
Spends 0.0000547490 secs in 2699 call(s) to undo_black_move, invocation 1.
Spends 0.0000004440 secs in 35 call(s) to return_black_mcts_best_move, invocation 1.
Spends 3.2280448340 secs in 2717 call(s) to white_alpha_beta, invocation 2.
Is called 1391 time(s) from white_alpha_beta, invocation 1.

# End of profile.
```
