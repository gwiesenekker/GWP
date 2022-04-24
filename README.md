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
Record the time t1
Store the time t2 in a pointer
```
Ideally t2 - t1 should be zero if there is no code executed between t1 and t2. Recently I started to notice some strange results when profiling small functions (less than 100 ticks) that were called many times, so I decided to take a look at how large the intrinsic profile overhead is using the following program. You can compile it standalone if you are interested in what the intrinsic profile overhead looks like on your system.
```
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#define COUNTER_VARIABLE(V)\
  {struct timespec tv; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv); V = tv.tv_sec * 1000000000 + tv.tv_nsec;}
#define COUNTER_POINTER(P)\
  {struct timespec tv; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv); *(P) = tv.tv_sec * 1000000000 + tv.tv_nsec;}

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
  unsigned long long volatile counter_dummy;
  unsigned long long volatile *counter_pointer;

  counter_pointer = &counter_dummy;
  
  for (int j = 0; j < 10; j++)
  {
    double mn = 0.0;
    double sn = 0.0;
  
    for (long long n = 1; n <= NCALL; ++n)
    {
      unsigned long long volatile counter_stamp;
    
      COUNTER_VARIABLE(counter_stamp)
      COUNTER_POINTER(counter_pointer)
    
      update_mean_sigma(n, counter_dummy - counter_stamp, NULL, &mn, &sn);
    }

    //note that the distribution is very skewed to the right, so the distribution
    //is not normal, the standard deviation has no meaning.
    //deviations LESS than the mean hardly ever occur
    //but deviations LARGER than the mean do occur more often
 
    //arbitrarily set the 'standard deviation' to one-third of the mean
    //to check for large positive deviations
    //sigma = round(sqrt(sigma / NCALL));

    long long sigma = round(mn / 3.0);
    long long mean = round(mn);

    long long nlarge = 0;
    long long largest = 0;

    for (long long n = 1; n <= NCALL; ++n)
    {
      unsigned long long volatile counter_stamp;
    
      COUNTER_VARIABLE(counter_stamp)
      COUNTER_POINTER(counter_pointer)
    
      long long delta = counter_dummy - counter_stamp;

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
NCALL=1000000 mean=190 nlarge=127 largest=5470
NCALL=1000000 mean=191 nlarge=184 largest=13415
NCALL=1000000 mean=192 nlarge=146 largest=30578
NCALL=1000000 mean=195 nlarge=124 largest=9518
NCALL=1000000 mean=187 nlarge=153 largest=4639
NCALL=1000000 mean=193 nlarge=140 largest=15599
NCALL=1000000 mean=192 nlarge=142 largest=9498
NCALL=1000000 mean=193 nlarge=155 largest=4850
NCALL=1000000 mean=190 nlarge=158 largest=4880
NCALL=1000000 mean=191 nlarge=136 largest=5089
```
So the intrinsic profile overhead is 190 ticks on average which is far from negligible when profiling small functions, so we have to correct for it. But as you can see from the results you cannot get the correction perfectly right as the mean is not constant and sometimes (100 times out of 1M samples) a very large value is returned. I do not know why, it could be related to the performance counters on an AMD 1950X, it could be related to the Linux kernel or to the general problem of returning a meaningful high-resolution thread-specific timer on modern processors. Even if you execute the program on one CPU using taskset the results are not constant:
```
$ taskset --cpu-list 0 a.out
NCALL=1000000 mean=188 nlarge=121 largest=6893
NCALL=1000000 mean=182 nlarge=117 largest=4317
NCALL=1000000 mean=185 nlarge=103 largest=7664
NCALL=1000000 mean=184 nlarge=120 largest=8255
NCALL=1000000 mean=184 nlarge=120 largest=4778
NCALL=1000000 mean=187 nlarge=117 largest=10550
NCALL=1000000 mean=185 nlarge=108 largest=9858
NCALL=1000000 mean=187 nlarge=130 largest=7123
NCALL=1000000 mean=187 nlarge=97 largest=8465
NCALL=1000000 mean=187 nlarge=114 largest=5119
$ taskset --cpu-list 7 a.out
NCALL=1000000 mean=187 nlarge=207 largest=45776
NCALL=1000000 mean=187 nlarge=142 largest=44513
NCALL=1000000 mean=189 nlarge=136 largest=43692
NCALL=1000000 mean=189 nlarge=139 largest=44804
NCALL=1000000 mean=186 nlarge=145 largest=44233
NCALL=1000000 mean=189 nlarge=167 largest=44834
NCALL=1000000 mean=190 nlarge=155 largest=43612
NCALL=1000000 mean=188 nlarge=155 largest=44533
NCALL=1000000 mean=188 nlarge=182 largest=44244
NCALL=1000000 mean=189 nlarge=172 largest=44163
```
Oddly, for one CPU (7) my AMD 1950x always returns very large largest deviations. So how should we correct for the intrinsic profile overhead? GWP uses the following method: after calculating t2 - t1 or t3 - t2, GWP takes two samples of the intrinsic profile overhead and subtracts it from t2 - t1 or t3 - t2. The idea of sampling instead of using a fixed value for the intrinsic profile overhead is that it adjusts for the intrinsic profile overhead at that point in time (perhaps the counter is 'slow'), but as you cannot know which value of the intrinsic profile overhead will be returned it will always be an approximation. It can also happen that t2 - t1 or t3 - t2 minus the intrinsic profile overhead is less than zero, especially if a large value for the intrinsic profile overhead is returned. In that case GWP uses zero for t2 - t1 or t3 - t2.

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
profile-3-3-1-3                   17.62  17.62     0.0007671270     100000     0.0000000077          8
main                              17.43  17.43     0.0007590300          1     0.0007590300     759030
profile-2-2                       14.84  14.84     0.0006461510     100000     0.0000000065          6
profile-3-1                        9.08   9.08     0.0003951170     100000     0.0000000040          4
profile-1-1                        8.86   8.86     0.0003859370     100000     0.0000000039          4
profile-0-0                        5.50   5.50     0.0002396430     100000     0.0000000024          2
profile-3-3-1-3-3-0                4.75   4.75     0.0002067190     100000     0.0000000021          2
profile-1-1-0                      4.60   4.60     0.0002003850     100000     0.0000000020          2
profile-3-3-1-3-2-0                4.59   4.59     0.0001996320     100000     0.0000000020          2
profile-2-2-1-0                    4.42   4.42     0.0001924850     100000     0.0000000019          2
profile-2-2-2-0                    4.21   4.21     0.0001831880     100000     0.0000000018          2
profile-3-3-1-3-1-0                4.10   4.10     0.0001783620     100000     0.0000000018          2
```
As you can see the correction works really well. Ideally the ticks/call should be 0 in this case. What you can also see is an intrinsic build-up of the error depending on the number of children, going from 2 (0 children) to 8 (three children). This is unavoidable, as the counters in the parent have to be stopped and started again (with the corresponding error) each time a child is started, otherwise you cannot correct for the intrinsic profile overhead.

## Recursion

Profiling recursive procedures and functions is not easy. GWP solves this problem by profiling each invocation separately. DUMP_PROFILE shows both the time spent in each invocation and summed over invocations.

## Example

Here are the results for a 30 second run of my Draughts program GWD. DUMP_PROFILE will shorten large block names by removing vowels and underscores from the right until they are smaller than 32 characters.
```
# Profile dumped at 14:38:53-23/04/2022
# The frequency is 1000000000 ticks, or 0.0000000010 secs/tick.
# The intrinsic profile overhead is 188 ticks on average.
# 127 out of 1000000 samples of the intrinsic profile overhead
# ..are larger than twice the mean, with a largest deviation of 751.
# The total number of blocks is 57.
# The total run time was 34.0360811660 secs.
# The total self time was 6.2670587290 secs.
# The total profile overhead was 27.7690224370 secs.

# Blocks sorted by total time spent in block and children.
# The sum of total times (or the sum of the percentages)
# does not have any meaning, since children will be double counted.
name                             invocation   perc       total time      calls
main-thread                      1           37.43     2.3454834550          1
solve_problems                   1           37.43     2.3454795030          1
white_search                     1           37.42     2.3450677300          1
black_alpha_beta                 1           37.38     2.3426678070        142
white_alpha_beta                 1           37.38     2.3424447760        449
black_alpha_beta                 2           37.37     2.3419906500       1391
white_alpha_beta                 2           37.35     2.3406240950       2717
black_alpha_beta                 3           37.32     2.3386207480       6968
white_alpha_beta                 3           37.22     2.3325113510      12926
black_alpha_beta                 4           37.08     2.3237431260      29403
white_alpha_beta                 4           36.70     2.2999155070      36935
black_alpha_beta                 5           36.22     2.2701786670      87539
main-init                        1           35.43     2.2204845020          1
white_alpha_beta                 5           35.05     2.1963853640      92250
black_alpha_beta                 6           33.71     2.1128408390     166013
white_alpha_beta                 6           30.99     1.9419748410     187236
black_alpha_beta                 7           28.35     1.7765040580     217687
main-final                       1           25.60     1.6040692500          1
white_alpha_beta                 7           24.19     1.5161888720     220537
black_alpha_beta                 8           20.49     1.2839167000     187777
white_alpha_beta                 8           16.28     1.0204300760     156584
black_alpha_beta                 9           13.27     0.8315721740     110392
white_alpha_beta                 9           10.56     0.6620988470      80030
black_alpha_beta                 10           8.87     0.5559419180      60902
white_alpha_beta                 10           7.48     0.4685736240      62226
black_alpha_beta                 11           6.51     0.4079828150      58899
white_alpha_beta                 11           5.59     0.3500957080      67316
black_alpha_beta                 12           4.54     0.2844315130      79288
white_alpha_beta                 12           3.23     0.2026258570      84259
gen_black_moves                  1            2.93     0.1837417980     954441
return_pattern_score_double      1            2.91     0.1823441210     183338
gen_white_moves                  1            2.48     0.1551287600     926795
black_alpha_beta                 13           2.04     0.1277641810      73207
main-tests                       1            1.55     0.0970215220          1
white_alpha_beta                 13           1.11     0.0697983320      47124
read_endgame                     1            0.68     0.0425113030    2181188
black_alpha_beta                 14           0.58     0.0366133710      27892
test-neural                      1            0.28     0.0176520580          1
return_neural                    1            0.28     0.0174648570      10000
undo_black_move                  1            0.24     0.0150512150    1064821
do_black_move                    1            0.24     0.0149952620    1064821
do_white_move                    1            0.23     0.0146671330    1115602
white_alpha_beta                 14           0.23     0.0146093040      12445
Ax+b-and-activation-other-layrs  1            0.20     0.0123872390      20000
undo_white_move                  1            0.20     0.0123263460    1115602
black_alpha_beta                 15           0.11     0.0068918490       6931
Ax+b-and-activation-first-layer  1            0.08     0.0049430790      10000
white_alpha_beta                 15           0.04     0.0022752910       1885
return_my_timer                  1            0.02     0.0012843880       4392
black_alpha_beta                 16           0.02     0.0011981550       1408
white_alpha_beta                 16           0.00     0.0002285830        186
black_alpha_beta                 17           0.00     0.0000813920         50
check_white_moves                1            0.00     0.0000097240          1
return_white_mcts_best_move      1            0.00     0.0000093650       1123
white_alpha_beta                 17           0.00     0.0000063260          6
return_black_mcts_best_move      1            0.00     0.0000061060       1117
update_crc32                     1            0.00     0.0000000500          1

# Blocks sorted by total time spent in own code.
# The sum of the self times is equal to the total self time.
name                             invocation   perc        self time      calls
main-init                        1           35.43     2.2204845020          1
main-final                       1           25.60     1.6040574250          1
black_alpha_beta                 8            3.31     0.2075535430     187777
black_alpha_beta                 7            3.17     0.1988596010     217687
gen_black_moves                  1            2.93     0.1837417980     954441
return_pattern_score_double      1            2.91     0.1823441210     183338
white_alpha_beta                 7            2.77     0.1733447590     220537
gen_white_moves                  1            2.48     0.1551287600     926795
white_alpha_beta                 8            2.26     0.1417664070     156584
black_alpha_beta                 9            2.13     0.1335012830     110392
black_alpha_beta                 6            2.04     0.1278376910     166013
white_alpha_beta                 6            1.91     0.1198957570     187236
main-tests                       1            1.27     0.0793684980          1
white_alpha_beta                 9            1.24     0.0775718530      80030
black_alpha_beta                 10           1.07     0.0670066240      60902
white_alpha_beta                 5            0.89     0.0560079100      92250
black_alpha_beta                 5            0.87     0.0545657470      87539
black_alpha_beta                 12           0.82     0.0512984770      79288
white_alpha_beta                 12           0.73     0.0455498730      84259
black_alpha_beta                 13           0.70     0.0437062950      73207
read_endgame                     1            0.68     0.0425113030    2181188
white_alpha_beta                 10           0.66     0.0416486550      62226
black_alpha_beta                 11           0.66     0.0412169620      58899
white_alpha_beta                 11           0.62     0.0386206680      67316
white_alpha_beta                 13           0.40     0.0249592460      47124
white_alpha_beta                 4            0.30     0.0190977190      36935
black_alpha_beta                 14           0.27     0.0168441000      27892
black_alpha_beta                 4            0.27     0.0167353390      29403
undo_black_move                  1            0.24     0.0150512150    1064821
do_black_move                    1            0.24     0.0149952620    1064821
do_white_move                    1            0.23     0.0146671330    1115602
Ax+b-and-activation-other-layrs  1            0.20     0.0123872390      20000
undo_white_move                  1            0.20     0.0123263460    1115602
white_alpha_beta                 14           0.10     0.0060354220      12445
white_alpha_beta                 3            0.09     0.0054187680      12926
Ax+b-and-activation-first-layer  1            0.08     0.0049430790      10000
black_alpha_beta                 3            0.06     0.0039828950       6968
black_alpha_beta                 15           0.05     0.0034087910       6931
white_search                     1            0.04     0.0022672780          1
return_my_timer                  1            0.02     0.0012843880       4392
white_alpha_beta                 2            0.02     0.0012154250       2717
white_alpha_beta                 15           0.01     0.0008648920       1885
black_alpha_beta                 2            0.01     0.0008441960       1391
black_alpha_beta                 16           0.01     0.0007516850       1408
solve_problems                   1            0.01     0.0004013880          1
white_alpha_beta                 1            0.00     0.0002878140        449
test-neural                      1            0.00     0.0001872010          1
black_alpha_beta                 1            0.00     0.0001458480        142
return_neural                    1            0.00     0.0001345390      10000
white_alpha_beta                 16           0.00     0.0001317920        186
black_alpha_beta                 17           0.00     0.0000661810         50
check_white_moves                1            0.00     0.0000094940          1
return_white_mcts_best_move      1            0.00     0.0000093650       1123
return_black_mcts_best_move      1            0.00     0.0000061060       1117
white_alpha_beta                 17           0.00     0.0000060690          6
main-thread                      1            0.00     0.0000039520          1
update_crc32                     1            0.00     0.0000000500          1

# Blocks sorted by self times summed over recursive invocations.
name                               perc  %main        self time      calls   self time/call ticks/call
main-init                         35.43  94.67     2.2204845020          1     2.2204845020         -1
main-final                        25.60  68.39     1.6040574250          1     1.6040574250         -1
black_alpha_beta                  15.45  41.28     0.9683252580    1115889     0.0000008678        868
white_alpha_beta                  12.01  32.08     0.7524230290    1065111     0.0000007064        706
gen_black_moves                    2.93   7.83     0.1837417980     954441     0.0000001925        193
return_pattern_score_double        2.91   7.77     0.1823441210     183338     0.0000009946        995
gen_white_moves                    2.48   6.61     0.1551287600     926795     0.0000001674        167
main-tests                         1.27   3.38     0.0793684980          1     0.0793684980   79368498
read_endgame                       0.68   1.81     0.0425113030    2181188     0.0000000195         19
undo_black_move                    0.24   0.64     0.0150512150    1064821     0.0000000141         14
do_black_move                      0.24   0.64     0.0149952620    1064821     0.0000000141         14
do_white_move                      0.23   0.63     0.0146671330    1115602     0.0000000131         13
Ax+b-and-activation-other-layrs    0.20   0.53     0.0123872390      20000     0.0000006194        619
undo_white_move                    0.20   0.53     0.0123263460    1115602     0.0000000110         11
Ax+b-and-activation-first-layer    0.08   0.21     0.0049430790      10000     0.0000004943        494
white_search                       0.04   0.10     0.0022672780          1     0.0022672780    2267278
return_my_timer                    0.02   0.05     0.0012843880       4392     0.0000002924        292
solve_problems                     0.01   0.02     0.0004013880          1     0.0004013880     401388
test-neural                        0.00   0.01     0.0001872010          1     0.0001872010     187201
return_neural                      0.00   0.01     0.0001345390      10000     0.0000000135         13
check_white_moves                  0.00   0.00     0.0000094940          1     0.0000094940       9494
return_white_mcts_best_move        0.00   0.00     0.0000093650       1123     0.0000000083          8
return_black_mcts_best_move        0.00   0.00     0.0000061060       1117     0.0000000055          5
main-thread                        0.00   0.00     0.0000039520          1     0.0000039520       3952
update_crc32                       0.00   0.00     0.0000000500          1     0.0000000500         50

# Summary for block main-init, invocation 1.
Spends 2.2204845020 secs in 1 call(s), or 35.43% of total execution time.
Spends 2.2204845020 secs (35.43%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
No parents were found

# Summary for block main-final, invocation 1.
Spends 1.6040692500 secs in 1 call(s), or 25.60% of total execution time.
Spends 1.6040574250 secs (25.60%) in own code, 0.0000118250 secs (0.00%) in children.

Spends 0.0000118250 secs in 3 call(s) to return_my_timer, invocation 1.
No parents were found

# Summary for block black_alpha_beta, invocation 1.
Spends 2.3426678070 secs in 142 call(s), or 37.38% of total execution time.
Spends 0.0001458480 secs (0.00%) in own code, 2.3425219590 secs (37.38%) in children.

Spends 0.0000093970 secs in 6 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000006410 secs in 142 call(s) to read_endgame, invocation 1.
Spends 0.0000373860 secs in 142 call(s) to gen_black_moves, invocation 1.
Spends 0.0000074150 secs in 435 call(s) to do_black_move, invocation 1.
Spends 2.3424447760 secs in 449 call(s) to white_alpha_beta, invocation 1.
Spends 0.0000217710 secs in 435 call(s) to undo_black_move, invocation 1.
Spends 0.0000005730 secs in 20 call(s) to return_black_mcts_best_move, invocation 1.
Is called 142 time(s) from white_search, invocation 1.

# Summary for block white_alpha_beta, invocation 1.
Spends 2.3424447760 secs in 449 call(s), or 37.38% of total execution time.
Spends 0.0002878140 secs (0.00%) in own code, 2.3421569620 secs (37.37%) in children.

Spends 0.0001028270 secs in 420 call(s) to gen_white_moves, invocation 1.
Spends 0.0000185320 secs in 1382 call(s) to do_white_move, invocation 1.
Spends 0.0000358130 secs in 1382 call(s) to undo_white_move, invocation 1.
Spends 0.0000074820 secs in 6 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000012600 secs in 449 call(s) to read_endgame, invocation 1.
Spends 0.0000003980 secs in 33 call(s) to return_white_mcts_best_move, invocation 1.
Spends 2.3419906500 secs in 1391 call(s) to black_alpha_beta, invocation 2.
Is called 449 time(s) from black_alpha_beta, invocation 1.

# Summary for block gen_black_moves, invocation 1.
Spends 0.1837417980 secs in 954441 call(s), or 2.93% of total execution time.
Spends 0.1837417980 secs (2.93%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 102 time(s) from white_search, invocation 1.
Is called 142 time(s) from black_alpha_beta, invocation 1.
Is called 1233 time(s) from black_alpha_beta, invocation 2.
Is called 5825 time(s) from black_alpha_beta, invocation 3.
Is called 20640 time(s) from black_alpha_beta, invocation 4.
Is called 54995 time(s) from black_alpha_beta, invocation 5.
Is called 117589 time(s) from black_alpha_beta, invocation 6.
Is called 174155 time(s) from black_alpha_beta, invocation 7.
Is called 171397 time(s) from black_alpha_beta, invocation 8.
Is called 105333 time(s) from black_alpha_beta, invocation 9.
Is called 58179 time(s) from black_alpha_beta, invocation 10.
Is called 57779 time(s) from black_alpha_beta, invocation 11.
Is called 78787 time(s) from black_alpha_beta, invocation 12.
Is called 72891 time(s) from black_alpha_beta, invocation 13.
Is called 27542 time(s) from black_alpha_beta, invocation 14.
Is called 6637 time(s) from black_alpha_beta, invocation 15.
Is called 1172 time(s) from black_alpha_beta, invocation 16.
Is called 43 time(s) from black_alpha_beta, invocation 17.

# Summary for block return_pattern_score_double, invocation 1.
Spends 0.1823441210 secs in 183338 call(s), or 2.91% of total execution time.
Spends 0.1823441210 secs (2.91%) in own code, 0.0000000000 secs (0.00%) in children.

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
Is called 13331 time(s) from white_alpha_beta, invocation 6.
Is called 16497 time(s) from black_alpha_beta, invocation 7.
Is called 22030 time(s) from white_alpha_beta, invocation 7.
Is called 18298 time(s) from black_alpha_beta, invocation 8.
Is called 21447 time(s) from white_alpha_beta, invocation 8.
Is called 14330 time(s) from black_alpha_beta, invocation 9.
Is called 15618 time(s) from white_alpha_beta, invocation 9.
Is called 8260 time(s) from black_alpha_beta, invocation 10.
Is called 8355 time(s) from white_alpha_beta, invocation 10.
Is called 4174 time(s) from black_alpha_beta, invocation 11.
Is called 4732 time(s) from white_alpha_beta, invocation 11.
Is called 3722 time(s) from black_alpha_beta, invocation 12.
Is called 4188 time(s) from white_alpha_beta, invocation 12.
Is called 1521 time(s) from black_alpha_beta, invocation 13.
Is called 1788 time(s) from white_alpha_beta, invocation 13.
Is called 635 time(s) from black_alpha_beta, invocation 14.
Is called 267 time(s) from white_alpha_beta, invocation 14.
Is called 40 time(s) from black_alpha_beta, invocation 15.
Is called 3 time(s) from white_alpha_beta, invocation 15.

# Summary for block gen_white_moves, invocation 1.
Spends 0.1551287600 secs in 926795 call(s), or 2.48% of total execution time.
Spends 0.1551287600 secs (2.48%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 1 time(s) from main-tests, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.
Is called 86 time(s) from white_search, invocation 1.
Is called 420 time(s) from white_alpha_beta, invocation 1.
Is called 2342 time(s) from white_alpha_beta, invocation 2.
Is called 9803 time(s) from white_alpha_beta, invocation 3.
Is called 30843 time(s) from white_alpha_beta, invocation 4.
Is called 75109 time(s) from white_alpha_beta, invocation 5.
Is called 140946 time(s) from white_alpha_beta, invocation 6.
Is called 180814 time(s) from white_alpha_beta, invocation 7.
Is called 141526 time(s) from white_alpha_beta, invocation 8.
Is called 75032 time(s) from white_alpha_beta, invocation 9.
Is called 59536 time(s) from white_alpha_beta, invocation 10.
Is called 65746 time(s) from white_alpha_beta, invocation 11.
Is called 83415 time(s) from white_alpha_beta, invocation 12.
Is called 46769 time(s) from white_alpha_beta, invocation 13.
Is called 12342 time(s) from white_alpha_beta, invocation 14.
Is called 1872 time(s) from white_alpha_beta, invocation 15.
Is called 186 time(s) from white_alpha_beta, invocation 16.
Is called 6 time(s) from white_alpha_beta, invocation 17.

# Summary for block main-tests, invocation 1.
Spends 0.0970215220 secs in 1 call(s), or 1.55% of total execution time.
Spends 0.0793684980 secs (1.27%) in own code, 0.0176530240 secs (0.28%) in children.

Spends 0.0000009160 secs in 1 call(s) to gen_white_moves, invocation 1.
Spends 0.0000000500 secs in 1 call(s) to update_crc32, invocation 1.
Spends 0.0176520580 secs in 1 call(s) to test-neural, invocation 1.
No parents were found

# Summary for block read_endgame, invocation 1.
Spends 0.0425113030 secs in 2181188 call(s), or 0.68% of total execution time.
Spends 0.0425113030 secs (0.68%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 189 time(s) from white_search, invocation 1.
Is called 142 time(s) from black_alpha_beta, invocation 1.
Is called 449 time(s) from white_alpha_beta, invocation 1.
Is called 1391 time(s) from black_alpha_beta, invocation 2.
Is called 2717 time(s) from white_alpha_beta, invocation 2.
Is called 6968 time(s) from black_alpha_beta, invocation 3.
Is called 12926 time(s) from white_alpha_beta, invocation 3.
Is called 29403 time(s) from black_alpha_beta, invocation 4.
Is called 36935 time(s) from white_alpha_beta, invocation 4.
Is called 87539 time(s) from black_alpha_beta, invocation 5.
Is called 92250 time(s) from white_alpha_beta, invocation 5.
Is called 166013 time(s) from black_alpha_beta, invocation 6.
Is called 187236 time(s) from white_alpha_beta, invocation 6.
Is called 217687 time(s) from black_alpha_beta, invocation 7.
Is called 220537 time(s) from white_alpha_beta, invocation 7.
Is called 187776 time(s) from black_alpha_beta, invocation 8.
Is called 156584 time(s) from white_alpha_beta, invocation 8.
Is called 110392 time(s) from black_alpha_beta, invocation 9.
Is called 80030 time(s) from white_alpha_beta, invocation 9.
Is called 60902 time(s) from black_alpha_beta, invocation 10.
Is called 62226 time(s) from white_alpha_beta, invocation 10.
Is called 58899 time(s) from black_alpha_beta, invocation 11.
Is called 67316 time(s) from white_alpha_beta, invocation 11.
Is called 79288 time(s) from black_alpha_beta, invocation 12.
Is called 84259 time(s) from white_alpha_beta, invocation 12.
Is called 73207 time(s) from black_alpha_beta, invocation 13.
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
Spends 0.0150512150 secs in 1064821 call(s), or 0.24% of total execution time.
Spends 0.0150512150 secs (0.24%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 86 time(s) from white_search, invocation 1.
Is called 435 time(s) from black_alpha_beta, invocation 1.
Is called 2699 time(s) from black_alpha_beta, invocation 2.
Is called 12889 time(s) from black_alpha_beta, invocation 3.
Is called 36891 time(s) from black_alpha_beta, invocation 4.
Is called 92166 time(s) from black_alpha_beta, invocation 5.
Is called 187203 time(s) from black_alpha_beta, invocation 6.
Is called 220496 time(s) from black_alpha_beta, invocation 7.
Is called 156545 time(s) from black_alpha_beta, invocation 8.
Is called 80011 time(s) from black_alpha_beta, invocation 9.
Is called 62198 time(s) from black_alpha_beta, invocation 10.
Is called 67298 time(s) from black_alpha_beta, invocation 11.
Is called 84258 time(s) from black_alpha_beta, invocation 12.
Is called 47124 time(s) from black_alpha_beta, invocation 13.
Is called 12445 time(s) from black_alpha_beta, invocation 14.
Is called 1885 time(s) from black_alpha_beta, invocation 15.
Is called 186 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block do_black_move, invocation 1.
Spends 0.0149952620 secs in 1064821 call(s), or 0.24% of total execution time.
Spends 0.0149952620 secs (0.24%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 86 time(s) from white_search, invocation 1.
Is called 435 time(s) from black_alpha_beta, invocation 1.
Is called 2699 time(s) from black_alpha_beta, invocation 2.
Is called 12889 time(s) from black_alpha_beta, invocation 3.
Is called 36891 time(s) from black_alpha_beta, invocation 4.
Is called 92166 time(s) from black_alpha_beta, invocation 5.
Is called 187203 time(s) from black_alpha_beta, invocation 6.
Is called 220496 time(s) from black_alpha_beta, invocation 7.
Is called 156545 time(s) from black_alpha_beta, invocation 8.
Is called 80011 time(s) from black_alpha_beta, invocation 9.
Is called 62198 time(s) from black_alpha_beta, invocation 10.
Is called 67298 time(s) from black_alpha_beta, invocation 11.
Is called 84258 time(s) from black_alpha_beta, invocation 12.
Is called 47124 time(s) from black_alpha_beta, invocation 13.
Is called 12445 time(s) from black_alpha_beta, invocation 14.
Is called 1885 time(s) from black_alpha_beta, invocation 15.
Is called 186 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block do_white_move, invocation 1.
Spends 0.0146671330 secs in 1115602 call(s), or 0.23% of total execution time.
Spends 0.0146671330 secs (0.23%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 9 time(s) from check_white_moves, invocation 1.
Is called 244 time(s) from white_search, invocation 1.
Is called 1382 time(s) from white_alpha_beta, invocation 1.
Is called 6948 time(s) from white_alpha_beta, invocation 2.
Is called 29370 time(s) from white_alpha_beta, invocation 3.
Is called 87456 time(s) from white_alpha_beta, invocation 4.
Is called 165950 time(s) from white_alpha_beta, invocation 5.
Is called 217644 time(s) from white_alpha_beta, invocation 6.
Is called 187737 time(s) from white_alpha_beta, invocation 7.
Is called 110350 time(s) from white_alpha_beta, invocation 8.
Is called 60863 time(s) from white_alpha_beta, invocation 9.
Is called 58873 time(s) from white_alpha_beta, invocation 10.
Is called 79288 time(s) from white_alpha_beta, invocation 11.
Is called 73207 time(s) from white_alpha_beta, invocation 12.
Is called 27892 time(s) from white_alpha_beta, invocation 13.
Is called 6931 time(s) from white_alpha_beta, invocation 14.
Is called 1408 time(s) from white_alpha_beta, invocation 15.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block Ax+b-and-activation-other-layrs, invocation 1.
Spends 0.0123872390 secs in 20000 call(s), or 0.20% of total execution time.
Spends 0.0123872390 secs (0.20%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 20000 time(s) from return_neural, invocation 1.

# Summary for block undo_white_move, invocation 1.
Spends 0.0123263460 secs in 1115602 call(s), or 0.20% of total execution time.
Spends 0.0123263460 secs (0.20%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 9 time(s) from check_white_moves, invocation 1.
Is called 244 time(s) from white_search, invocation 1.
Is called 1382 time(s) from white_alpha_beta, invocation 1.
Is called 6948 time(s) from white_alpha_beta, invocation 2.
Is called 29370 time(s) from white_alpha_beta, invocation 3.
Is called 87456 time(s) from white_alpha_beta, invocation 4.
Is called 165950 time(s) from white_alpha_beta, invocation 5.
Is called 217644 time(s) from white_alpha_beta, invocation 6.
Is called 187737 time(s) from white_alpha_beta, invocation 7.
Is called 110350 time(s) from white_alpha_beta, invocation 8.
Is called 60863 time(s) from white_alpha_beta, invocation 9.
Is called 58873 time(s) from white_alpha_beta, invocation 10.
Is called 79288 time(s) from white_alpha_beta, invocation 11.
Is called 73207 time(s) from white_alpha_beta, invocation 12.
Is called 27892 time(s) from white_alpha_beta, invocation 13.
Is called 6931 time(s) from white_alpha_beta, invocation 14.
Is called 1408 time(s) from white_alpha_beta, invocation 15.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block Ax+b-and-activation-first-layer, invocation 1.
Spends 0.0049430790 secs in 10000 call(s), or 0.08% of total execution time.
Spends 0.0049430790 secs (0.08%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 10000 time(s) from return_neural, invocation 1.

# Summary for block white_search, invocation 1.
Spends 2.3450677300 secs in 1 call(s), or 37.42% of total execution time.
Spends 0.0022672780 secs (0.04%) in own code, 2.3428004520 secs (37.38%) in children.

Spends 0.0000204400 secs in 86 call(s) to gen_white_moves, invocation 1.
Spends 0.0000053940 secs in 244 call(s) to do_white_move, invocation 1.
Spends 0.0000185290 secs in 244 call(s) to undo_white_move, invocation 1.
Spends 0.0000461620 secs in 26 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000045330 secs in 189 call(s) to read_endgame, invocation 1.
Spends 0.0000004150 secs in 16 call(s) to return_white_mcts_best_move, invocation 1.
Spends 2.3426678070 secs in 142 call(s) to black_alpha_beta, invocation 1.
Spends 0.0000276230 secs in 102 call(s) to gen_black_moves, invocation 1.
Spends 0.0000078520 secs in 27 call(s) to return_my_timer, invocation 1.
Spends 0.0000007840 secs in 86 call(s) to do_black_move, invocation 1.
Spends 0.0000009130 secs in 86 call(s) to undo_black_move, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.

# Summary for block return_my_timer, invocation 1.
Spends 0.0012843880 secs in 4392 call(s), or 0.02% of total execution time.
Spends 0.0012843880 secs (0.02%) in own code, 0.0000000000 secs (0.00%) in children.

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
Is called 400 time(s) from white_alpha_beta, invocation 6.
Is called 424 time(s) from black_alpha_beta, invocation 7.
Is called 436 time(s) from white_alpha_beta, invocation 7.
Is called 368 time(s) from black_alpha_beta, invocation 8.
Is called 322 time(s) from white_alpha_beta, invocation 8.
Is called 202 time(s) from black_alpha_beta, invocation 9.
Is called 138 time(s) from white_alpha_beta, invocation 9.
Is called 136 time(s) from black_alpha_beta, invocation 10.
Is called 122 time(s) from white_alpha_beta, invocation 10.
Is called 96 time(s) from black_alpha_beta, invocation 11.
Is called 136 time(s) from white_alpha_beta, invocation 11.
Is called 160 time(s) from black_alpha_beta, invocation 12.
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
Spends 2.3454795030 secs in 1 call(s), or 37.43% of total execution time.
Spends 0.0004013880 secs (0.01%) in own code, 2.3450781150 secs (37.42%) in children.

Spends 0.0000006610 secs in 1 call(s) to gen_white_moves, invocation 1.
Spends 0.0000097240 secs in 1 call(s) to check_white_moves, invocation 1.
Spends 2.3450677300 secs in 1 call(s) to white_search, invocation 1.
Is called 1 time(s) from main-thread, invocation 1.

# Summary for block test-neural, invocation 1.
Spends 0.0176520580 secs in 1 call(s), or 0.28% of total execution time.
Spends 0.0001872010 secs (0.00%) in own code, 0.0174648570 secs (0.28%) in children.

Spends 0.0174648570 secs in 10000 call(s) to return_neural, invocation 1.
Is called 1 time(s) from main-tests, invocation 1.

# Summary for block return_neural, invocation 1.
Spends 0.0174648570 secs in 10000 call(s), or 0.28% of total execution time.
Spends 0.0001345390 secs (0.00%) in own code, 0.0173303180 secs (0.28%) in children.

Spends 0.0049430790 secs in 10000 call(s) to Ax+b-and-activation-first-layer, invocation 1.
Spends 0.0123872390 secs in 20000 call(s) to Ax+b-and-activation-other-layrs, invocation 1.
Is called 10000 time(s) from test-neural, invocation 1.

# Summary for block check_white_moves, invocation 1.
Spends 0.0000097240 secs in 1 call(s), or 0.00% of total execution time.
Spends 0.0000094940 secs (0.00%) in own code, 0.0000002300 secs (0.00%) in children.

Spends 0.0000001750 secs in 9 call(s) to do_white_move, invocation 1.
Spends 0.0000000550 secs in 9 call(s) to undo_white_move, invocation 1.
Is called 1 time(s) from solve_problems, invocation 1.

# Summary for block return_white_mcts_best_move, invocation 1.
Spends 0.0000093650 secs in 1123 call(s), or 0.00% of total execution time.
Spends 0.0000093650 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

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

# Summary for block return_black_mcts_best_move, invocation 1.
Spends 0.0000061060 secs in 1117 call(s), or 0.00% of total execution time.
Spends 0.0000061060 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

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

# Summary for block main-thread, invocation 1.
Spends 2.3454834550 secs in 1 call(s), or 37.43% of total execution time.
Spends 0.0000039520 secs (0.00%) in own code, 2.3454795030 secs (37.43%) in children.

Spends 2.3454795030 secs in 1 call(s) to solve_problems, invocation 1.
No parents were found

# Summary for block update_crc32, invocation 1.
Spends 0.0000000500 secs in 1 call(s), or 0.00% of total execution time.
Spends 0.0000000500 secs (0.00%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 1 time(s) from main-tests, invocation 1.

# Summary for block white_alpha_beta, invocation 2.
Spends 2.3406240950 secs in 2717 call(s), or 37.35% of total execution time.
Spends 0.0012154250 secs (0.02%) in own code, 2.3394086700 secs (37.33%) in children.

Spends 0.0005123800 secs in 2342 call(s) to gen_white_moves, invocation 1.
Spends 0.0000764870 secs in 6948 call(s) to do_white_move, invocation 1.
Spends 0.0000865400 secs in 6948 call(s) to undo_white_move, invocation 1.
Spends 0.0001012310 secs in 98 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000082160 secs in 2717 call(s) to read_endgame, invocation 1.
Spends 0.0000013550 secs in 50 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000017130 secs in 4 call(s) to return_my_timer, invocation 1.
Spends 2.3386207480 secs in 6968 call(s) to black_alpha_beta, invocation 3.
Is called 2717 time(s) from black_alpha_beta, invocation 2.

# Summary for block black_alpha_beta, invocation 3.
Spends 2.3386207480 secs in 6968 call(s), or 37.32% of total execution time.
Spends 0.0039828950 secs (0.06%) in own code, 2.3346378530 secs (37.25%) in children.

Spends 0.0003508330 secs in 330 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000191350 secs in 6968 call(s) to read_endgame, invocation 1.
Spends 0.0014195470 secs in 5825 call(s) to gen_black_moves, invocation 1.
Spends 0.0000026130 secs in 10 call(s) to return_my_timer, invocation 1.
Spends 0.0001450460 secs in 12889 call(s) to do_black_move, invocation 1.
Spends 0.0001888810 secs in 12889 call(s) to undo_black_move, invocation 1.
Spends 0.0000004470 secs in 55 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.3325113510 secs in 12926 call(s) to white_alpha_beta, invocation 3.
Is called 6968 time(s) from white_alpha_beta, invocation 2.

# Summary for block white_alpha_beta, invocation 3.
Spends 2.3325113510 secs in 12926 call(s), or 37.22% of total execution time.
Spends 0.0054187680 secs (0.09%) in own code, 2.3270925830 secs (37.13%) in children.

Spends 0.0021000500 secs in 9803 call(s) to gen_white_moves, invocation 1.
Spends 0.0003311300 secs in 29370 call(s) to do_white_move, invocation 1.
Spends 0.0003600800 secs in 29370 call(s) to undo_white_move, invocation 1.
Spends 0.0005073560 secs in 486 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000414390 secs in 12926 call(s) to read_endgame, invocation 1.
Spends 0.0000009730 secs in 76 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000084290 secs in 24 call(s) to return_my_timer, invocation 1.
Spends 2.3237431260 secs in 29403 call(s) to black_alpha_beta, invocation 4.
Is called 12926 time(s) from black_alpha_beta, invocation 3.

# Summary for block black_alpha_beta, invocation 4.
Spends 2.3237431260 secs in 29403 call(s), or 37.08% of total execution time.
Spends 0.0167353390 secs (0.27%) in own code, 2.3070077870 secs (36.81%) in children.

Spends 0.0011362800 secs in 1060 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000900470 secs in 29403 call(s) to read_endgame, invocation 1.
Spends 0.0048724170 secs in 20640 call(s) to gen_black_moves, invocation 1.
Spends 0.0000144150 secs in 50 call(s) to return_my_timer, invocation 1.
Spends 0.0004728330 secs in 36891 call(s) to do_black_move, invocation 1.
Spends 0.0005057560 secs in 36891 call(s) to undo_black_move, invocation 1.
Spends 0.0000005320 secs in 93 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.2999155070 secs in 36935 call(s) to white_alpha_beta, invocation 4.
Is called 29403 time(s) from white_alpha_beta, invocation 3.

# Summary for block white_alpha_beta, invocation 4.
Spends 2.2999155070 secs in 36935 call(s), or 36.70% of total execution time.
Spends 0.0190977190 secs (0.30%) in own code, 2.2808177880 secs (36.39%) in children.

Spends 0.0063485300 secs in 30843 call(s) to gen_white_moves, invocation 1.
Spends 0.0009337790 secs in 87456 call(s) to do_white_move, invocation 1.
Spends 0.0008620100 secs in 87456 call(s) to undo_white_move, invocation 1.
Spends 0.0023533400 secs in 2168 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001210250 secs in 36935 call(s) to read_endgame, invocation 1.
Spends 0.0000009530 secs in 122 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000194840 secs in 70 call(s) to return_my_timer, invocation 1.
Spends 2.2701786670 secs in 87539 call(s) to black_alpha_beta, invocation 5.
Is called 36935 time(s) from black_alpha_beta, invocation 4.

# Summary for block black_alpha_beta, invocation 5.
Spends 2.2701786670 secs in 87539 call(s), or 36.22% of total execution time.
Spends 0.0545657470 secs (0.87%) in own code, 2.2156129200 secs (35.35%) in children.

Spends 0.0041525300 secs in 3635 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002722610 secs in 87539 call(s) to read_endgame, invocation 1.
Spends 0.0124812720 secs in 54995 call(s) to gen_black_moves, invocation 1.
Spends 0.0000521200 secs in 190 call(s) to return_my_timer, invocation 1.
Spends 0.0011452280 secs in 92166 call(s) to do_black_move, invocation 1.
Spends 0.0011230970 secs in 92166 call(s) to undo_black_move, invocation 1.
Spends 0.0000010480 secs in 198 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.1963853640 secs in 92250 call(s) to white_alpha_beta, invocation 5.
Is called 87539 time(s) from white_alpha_beta, invocation 4.

# Summary for block white_alpha_beta, invocation 5.
Spends 2.1963853640 secs in 92250 call(s), or 35.05% of total execution time.
Spends 0.0560079100 secs (0.89%) in own code, 2.1403774540 secs (34.15%) in children.

Spends 0.0149447350 secs in 75109 call(s) to gen_white_moves, invocation 1.
Spends 0.0019990710 secs in 165950 call(s) to do_white_move, invocation 1.
Spends 0.0017393170 secs in 165950 call(s) to undo_white_move, invocation 1.
Spends 0.0084969390 secs in 5938 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003072540 secs in 92250 call(s) to read_endgame, invocation 1.
Spends 0.0000014850 secs in 200 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000478140 secs in 168 call(s) to return_my_timer, invocation 1.
Spends 2.1128408390 secs in 166013 call(s) to black_alpha_beta, invocation 6.
Is called 92250 time(s) from black_alpha_beta, invocation 5.

# Summary for block black_alpha_beta, invocation 6.
Spends 2.1128408390 secs in 166013 call(s), or 33.71% of total execution time.
Spends 0.1278376910 secs (2.04%) in own code, 1.9850031480 secs (31.67%) in children.

Spends 0.0108553580 secs in 10258 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0011963960 secs in 166013 call(s) to read_endgame, invocation 1.
Spends 0.0262391530 secs in 117589 call(s) to gen_black_moves, invocation 1.
Spends 0.0000996420 secs in 348 call(s) to return_my_timer, invocation 1.
Spends 0.0021832440 secs in 187203 call(s) to do_black_move, invocation 1.
Spends 0.0024534500 secs in 187203 call(s) to undo_black_move, invocation 1.
Spends 0.0000010640 secs in 192 call(s) to return_black_mcts_best_move, invocation 1.
Spends 1.9419748410 secs in 187236 call(s) to white_alpha_beta, invocation 6.
Is called 166013 time(s) from white_alpha_beta, invocation 5.

# Summary for block white_alpha_beta, invocation 6.
Spends 1.9419748410 secs in 187236 call(s), or 30.99% of total execution time.
Spends 0.1198957570 secs (1.91%) in own code, 1.8220790840 secs (29.07%) in children.

Spends 0.0261355540 secs in 140946 call(s) to gen_white_moves, invocation 1.
Spends 0.0028210600 secs in 217644 call(s) to do_white_move, invocation 1.
Spends 0.0022786310 secs in 217644 call(s) to undo_white_move, invocation 1.
Spends 0.0136189450 secs in 13331 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0006047780 secs in 187236 call(s) to read_endgame, invocation 1.
Spends 0.0000007750 secs in 163 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0001152830 secs in 400 call(s) to return_my_timer, invocation 1.
Spends 1.7765040580 secs in 217687 call(s) to black_alpha_beta, invocation 7.
Is called 187236 time(s) from black_alpha_beta, invocation 6.

# Summary for block black_alpha_beta, invocation 7.
Spends 1.7765040580 secs in 217687 call(s), or 28.35% of total execution time.
Spends 0.1988596010 secs (3.17%) in own code, 1.5776444570 secs (25.17%) in children.

Spends 0.0166615170 secs in 16497 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0014656280 secs in 217687 call(s) to read_endgame, invocation 1.
Spends 0.0360394200 secs in 174155 call(s) to gen_black_moves, invocation 1.
Spends 0.0001222480 secs in 424 call(s) to return_my_timer, invocation 1.
Spends 0.0035134160 secs in 220496 call(s) to do_black_move, invocation 1.
Spends 0.0036528400 secs in 220496 call(s) to undo_black_move, invocation 1.
Spends 0.0000005160 secs in 145 call(s) to return_black_mcts_best_move, invocation 1.
Spends 1.5161888720 secs in 220537 call(s) to white_alpha_beta, invocation 7.
Is called 217687 time(s) from white_alpha_beta, invocation 6.

# Summary for block white_alpha_beta, invocation 7.
Spends 1.5161888720 secs in 220537 call(s), or 24.19% of total execution time.
Spends 0.1733447590 secs (2.77%) in own code, 1.3428441130 secs (21.43%) in children.

Spends 0.0315257910 secs in 180814 call(s) to gen_white_moves, invocation 1.
Spends 0.0027624910 secs in 187737 call(s) to do_white_move, invocation 1.
Spends 0.0022231960 secs in 187737 call(s) to undo_white_move, invocation 1.
Spends 0.0215629670 secs in 22030 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0007229790 secs in 220537 call(s) to read_endgame, invocation 1.
Spends 0.0000011220 secs in 136 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0001288670 secs in 436 call(s) to return_my_timer, invocation 1.
Spends 1.2839167000 secs in 187777 call(s) to black_alpha_beta, invocation 8.
Is called 220537 time(s) from black_alpha_beta, invocation 7.

# Summary for block black_alpha_beta, invocation 8.
Spends 1.2839167000 secs in 187777 call(s), or 20.49% of total execution time.
Spends 0.2075535430 secs (3.31%) in own code, 1.0763631570 secs (17.17%) in children.

Spends 0.0180870120 secs in 18298 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0006185350 secs in 187776 call(s) to read_endgame, invocation 1.
Spends 0.0326913050 secs in 171397 call(s) to gen_black_moves, invocation 1.
Spends 0.0001065050 secs in 368 call(s) to return_my_timer, invocation 1.
Spends 0.0020758650 secs in 156545 call(s) to do_black_move, invocation 1.
Spends 0.0023532960 secs in 156545 call(s) to undo_black_move, invocation 1.
Spends 0.0000005630 secs in 125 call(s) to return_black_mcts_best_move, invocation 1.
Spends 1.0204300760 secs in 156584 call(s) to white_alpha_beta, invocation 8.
Is called 187777 time(s) from white_alpha_beta, invocation 7.

# Summary for block white_alpha_beta, invocation 8.
Spends 1.0204300760 secs in 156584 call(s), or 16.28% of total execution time.
Spends 0.1417664070 secs (2.26%) in own code, 0.8786636690 secs (14.02%) in children.

Spends 0.0232373150 secs in 141526 call(s) to gen_white_moves, invocation 1.
Spends 0.0017192100 secs in 110350 call(s) to do_white_move, invocation 1.
Spends 0.0013603370 secs in 110350 call(s) to undo_white_move, invocation 1.
Spends 0.0201446800 secs in 21447 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0005344760 secs in 156584 call(s) to read_endgame, invocation 1.
Spends 0.0000008730 secs in 134 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000946040 secs in 322 call(s) to return_my_timer, invocation 1.
Spends 0.8315721740 secs in 110392 call(s) to black_alpha_beta, invocation 9.
Is called 156584 time(s) from black_alpha_beta, invocation 8.

# Summary for block black_alpha_beta, invocation 9.
Spends 0.8315721740 secs in 110392 call(s), or 13.27% of total execution time.
Spends 0.1335012830 secs (2.13%) in own code, 0.6980708910 secs (11.14%) in children.

Spends 0.0139269470 secs in 14330 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003614830 secs in 110392 call(s) to read_endgame, invocation 1.
Spends 0.0193064520 secs in 105333 call(s) to gen_black_moves, invocation 1.
Spends 0.0000596910 secs in 202 call(s) to return_my_timer, invocation 1.
Spends 0.0011122310 secs in 80011 call(s) to do_black_move, invocation 1.
Spends 0.0012048600 secs in 80011 call(s) to undo_black_move, invocation 1.
Spends 0.0000003800 secs in 101 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.6620988470 secs in 80030 call(s) to white_alpha_beta, invocation 9.
Is called 110392 time(s) from white_alpha_beta, invocation 8.

# Summary for block white_alpha_beta, invocation 9.
Spends 0.6620988470 secs in 80030 call(s), or 10.56% of total execution time.
Spends 0.0775718530 secs (1.24%) in own code, 0.5845269940 secs (9.33%) in children.

Spends 0.0119987810 secs in 75032 call(s) to gen_white_moves, invocation 1.
Spends 0.0009142630 secs in 60863 call(s) to do_white_move, invocation 1.
Spends 0.0007108260 secs in 60863 call(s) to undo_white_move, invocation 1.
Spends 0.0145057040 secs in 15618 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0004134820 secs in 80030 call(s) to read_endgame, invocation 1.
Spends 0.0000005020 secs in 86 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000415180 secs in 138 call(s) to return_my_timer, invocation 1.
Spends 0.5559419180 secs in 60902 call(s) to black_alpha_beta, invocation 10.
Is called 80030 time(s) from black_alpha_beta, invocation 9.

# Summary for block black_alpha_beta, invocation 10.
Spends 0.5559419180 secs in 60902 call(s), or 8.87% of total execution time.
Spends 0.0670066240 secs (1.07%) in own code, 0.4889352940 secs (7.80%) in children.

Spends 0.0078593620 secs in 8260 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002164200 secs in 60902 call(s) to read_endgame, invocation 1.
Spends 0.0106406680 secs in 58179 call(s) to gen_black_moves, invocation 1.
Spends 0.0000416610 secs in 136 call(s) to return_my_timer, invocation 1.
Spends 0.0007635780 secs in 62198 call(s) to do_black_move, invocation 1.
Spends 0.0008397540 secs in 62198 call(s) to undo_black_move, invocation 1.
Spends 0.0000002270 secs in 66 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.4685736240 secs in 62226 call(s) to white_alpha_beta, invocation 10.
Is called 60902 time(s) from white_alpha_beta, invocation 9.

# Summary for block white_alpha_beta, invocation 10.
Spends 0.4685736240 secs in 62226 call(s), or 7.48% of total execution time.
Spends 0.0416486550 secs (0.66%) in own code, 0.4269249690 secs (6.81%) in children.

Spends 0.0093192890 secs in 59536 call(s) to gen_white_moves, invocation 1.
Spends 0.0007803590 secs in 58873 call(s) to do_white_move, invocation 1.
Spends 0.0006866800 secs in 58873 call(s) to undo_white_move, invocation 1.
Spends 0.0077733360 secs in 8355 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003469750 secs in 62226 call(s) to read_endgame, invocation 1.
Spends 0.0000003300 secs in 62 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000351850 secs in 122 call(s) to return_my_timer, invocation 1.
Spends 0.4079828150 secs in 58899 call(s) to black_alpha_beta, invocation 11.
Is called 62226 time(s) from black_alpha_beta, invocation 10.

# Summary for block black_alpha_beta, invocation 11.
Spends 0.4079828150 secs in 58899 call(s), or 6.51% of total execution time.
Spends 0.0412169620 secs (0.66%) in own code, 0.3667658530 secs (5.85%) in children.

Spends 0.0038773020 secs in 4174 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002794930 secs in 58899 call(s) to read_endgame, invocation 1.
Spends 0.0100273580 secs in 57779 call(s) to gen_black_moves, invocation 1.
Spends 0.0000278630 secs in 96 call(s) to return_my_timer, invocation 1.
Spends 0.0016663360 secs in 67298 call(s) to do_black_move, invocation 1.
Spends 0.0007915600 secs in 67298 call(s) to undo_black_move, invocation 1.
Spends 0.0000002330 secs in 57 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.3500957080 secs in 67316 call(s) to white_alpha_beta, invocation 11.
Is called 58899 time(s) from white_alpha_beta, invocation 10.

# Summary for block white_alpha_beta, invocation 11.
Spends 0.3500957080 secs in 67316 call(s), or 5.59% of total execution time.
Spends 0.0386206680 secs (0.62%) in own code, 0.3114750400 secs (4.97%) in children.

Spends 0.0098720860 secs in 65746 call(s) to gen_white_moves, invocation 1.
Spends 0.0009785300 secs in 79288 call(s) to do_white_move, invocation 1.
Spends 0.0008407780 secs in 79288 call(s) to undo_white_move, invocation 1.
Spends 0.0044981400 secs in 4732 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0108153660 secs in 67316 call(s) to read_endgame, invocation 1.
Spends 0.0000001250 secs in 32 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000385020 secs in 136 call(s) to return_my_timer, invocation 1.
Spends 0.2844315130 secs in 79288 call(s) to black_alpha_beta, invocation 12.
Is called 67316 time(s) from black_alpha_beta, invocation 11.

# Summary for block black_alpha_beta, invocation 12.
Spends 0.2844315130 secs in 79288 call(s), or 4.54% of total execution time.
Spends 0.0512984770 secs (0.82%) in own code, 0.2331330360 secs (3.72%) in children.

Spends 0.0035749560 secs in 3722 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0118123030 secs in 79288 call(s) to read_endgame, invocation 1.
Spends 0.0130220260 secs in 78787 call(s) to gen_black_moves, invocation 1.
Spends 0.0000458260 secs in 160 call(s) to return_my_timer, invocation 1.
Spends 0.0010136010 secs in 84258 call(s) to do_black_move, invocation 1.
Spends 0.0010383680 secs in 84258 call(s) to undo_black_move, invocation 1.
Spends 0.0000000990 secs in 26 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.2026258570 secs in 84259 call(s) to white_alpha_beta, invocation 12.
Is called 79288 time(s) from white_alpha_beta, invocation 11.

# Summary for block white_alpha_beta, invocation 12.
Spends 0.2026258570 secs in 84259 call(s), or 3.23% of total execution time.
Spends 0.0455498730 secs (0.73%) in own code, 0.1570759840 secs (2.51%) in children.

Spends 0.0119222870 secs in 83415 call(s) to gen_white_moves, invocation 1.
Spends 0.0009110590 secs in 73207 call(s) to do_white_move, invocation 1.
Spends 0.0007781490 secs in 73207 call(s) to undo_white_move, invocation 1.
Spends 0.0040485760 secs in 4188 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0115935790 secs in 84259 call(s) to read_endgame, invocation 1.
Spends 0.0000000590 secs in 13 call(s) to return_white_mcts_best_move, invocation 1.
Spends 0.0000580940 secs in 202 call(s) to return_my_timer, invocation 1.
Spends 0.1277641810 secs in 73207 call(s) to black_alpha_beta, invocation 13.
Is called 84259 time(s) from black_alpha_beta, invocation 12.

# Summary for block black_alpha_beta, invocation 13.
Spends 0.1277641810 secs in 73207 call(s), or 2.04% of total execution time.
Spends 0.0437062950 secs (0.70%) in own code, 0.0840578860 secs (1.34%) in children.

Spends 0.0014804340 secs in 1521 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002551910 secs in 73207 call(s) to read_endgame, invocation 1.
Spends 0.0112262730 secs in 72891 call(s) to gen_black_moves, invocation 1.
Spends 0.0000408610 secs in 144 call(s) to return_my_timer, invocation 1.
Spends 0.0006488730 secs in 47124 call(s) to do_black_move, invocation 1.
Spends 0.0006079030 secs in 47124 call(s) to undo_black_move, invocation 1.
Spends 0.0000000190 secs in 4 call(s) to return_black_mcts_best_move, invocation 1.
Spends 0.0697983320 secs in 47124 call(s) to white_alpha_beta, invocation 13.
Is called 73207 time(s) from white_alpha_beta, invocation 12.

# Summary for block white_alpha_beta, invocation 13.
Spends 0.0697983320 secs in 47124 call(s), or 1.11% of total execution time.
Spends 0.0249592460 secs (0.40%) in own code, 0.0448390860 secs (0.72%) in children.

Spends 0.0056627000 secs in 46769 call(s) to gen_white_moves, invocation 1.
Spends 0.0003384960 secs in 27892 call(s) to do_white_move, invocation 1.
Spends 0.0002740070 secs in 27892 call(s) to undo_white_move, invocation 1.
Spends 0.0017158600 secs in 1788 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002069370 secs in 47124 call(s) to read_endgame, invocation 1.
Spends 0.0000277150 secs in 96 call(s) to return_my_timer, invocation 1.
Spends 0.0366133710 secs in 27892 call(s) to black_alpha_beta, invocation 14.
Is called 47124 time(s) from black_alpha_beta, invocation 13.

# Summary for block black_alpha_beta, invocation 14.
Spends 0.0366133710 secs in 27892 call(s), or 0.58% of total execution time.
Spends 0.0168441000 secs (0.27%) in own code, 0.0197692710 secs (0.32%) in children.

Spends 0.0005890750 secs in 635 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000994550 secs in 27892 call(s) to read_endgame, invocation 1.
Spends 0.0040924510 secs in 27542 call(s) to gen_black_moves, invocation 1.
Spends 0.0000177050 secs in 64 call(s) to return_my_timer, invocation 1.
Spends 0.0001825630 secs in 12445 call(s) to do_black_move, invocation 1.
Spends 0.0001787180 secs in 12445 call(s) to undo_black_move, invocation 1.
Spends 0.0146093040 secs in 12445 call(s) to white_alpha_beta, invocation 14.
Is called 27892 time(s) from white_alpha_beta, invocation 13.

# Summary for block white_alpha_beta, invocation 14.
Spends 0.0146093040 secs in 12445 call(s), or 0.23% of total execution time.
Spends 0.0060354220 secs (0.10%) in own code, 0.0085738820 secs (0.14%) in children.

Spends 0.0012316570 secs in 12342 call(s) to gen_white_moves, invocation 1.
Spends 0.0000668350 secs in 6931 call(s) to do_white_move, invocation 1.
Spends 0.0000592770 secs in 6931 call(s) to undo_white_move, invocation 1.
Spends 0.0002629850 secs in 267 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000518530 secs in 12445 call(s) to read_endgame, invocation 1.
Spends 0.0000094260 secs in 28 call(s) to return_my_timer, invocation 1.
Spends 0.0068918490 secs in 6931 call(s) to black_alpha_beta, invocation 15.
Is called 12445 time(s) from black_alpha_beta, invocation 14.

# Summary for block black_alpha_beta, invocation 15.
Spends 0.0068918490 secs in 6931 call(s), or 0.11% of total execution time.
Spends 0.0034087910 secs (0.05%) in own code, 0.0034830580 secs (0.06%) in children.

Spends 0.0000387720 secs in 40 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000315980 secs in 6931 call(s) to read_endgame, invocation 1.
Spends 0.0010796800 secs in 6637 call(s) to gen_black_moves, invocation 1.
Spends 0.0000052370 secs in 18 call(s) to return_my_timer, invocation 1.
Spends 0.0000272010 secs in 1885 call(s) to do_black_move, invocation 1.
Spends 0.0000252790 secs in 1885 call(s) to undo_black_move, invocation 1.
Spends 0.0022752910 secs in 1885 call(s) to white_alpha_beta, invocation 15.
Is called 6931 time(s) from white_alpha_beta, invocation 14.

# Summary for block white_alpha_beta, invocation 15.
Spends 0.0022752910 secs in 1885 call(s), or 0.04% of total execution time.
Spends 0.0008648920 secs (0.01%) in own code, 0.0014103990 secs (0.02%) in children.

Spends 0.0001786350 secs in 1872 call(s) to gen_white_moves, invocation 1.
Spends 0.0000098360 secs in 1408 call(s) to do_white_move, invocation 1.
Spends 0.0000116950 secs in 1408 call(s) to undo_white_move, invocation 1.
Spends 0.0000036720 secs in 3 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000078060 secs in 1885 call(s) to read_endgame, invocation 1.
Spends 0.0000006000 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0011981550 secs in 1408 call(s) to black_alpha_beta, invocation 16.
Is called 1885 time(s) from black_alpha_beta, invocation 15.

# Summary for block black_alpha_beta, invocation 16.
Spends 0.0011981550 secs in 1408 call(s), or 0.02% of total execution time.
Spends 0.0007516850 secs (0.01%) in own code, 0.0004464700 secs (0.01%) in children.

Spends 0.0000057870 secs in 1408 call(s) to read_endgame, invocation 1.
Spends 0.0002057020 secs in 1172 call(s) to gen_black_moves, invocation 1.
Spends 0.0000005350 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0000027420 secs in 186 call(s) to do_black_move, invocation 1.
Spends 0.0000031210 secs in 186 call(s) to undo_black_move, invocation 1.
Spends 0.0002285830 secs in 186 call(s) to white_alpha_beta, invocation 16.
Is called 1408 time(s) from white_alpha_beta, invocation 15.

# Summary for block white_alpha_beta, invocation 16.
Spends 0.0002285830 secs in 186 call(s), or 0.00% of total execution time.
Spends 0.0001317920 secs (0.00%) in own code, 0.0000967910 secs (0.00%) in children.

Spends 0.0000138880 secs in 186 call(s) to gen_white_moves, invocation 1.
Spends 0.0000004260 secs in 50 call(s) to do_white_move, invocation 1.
Spends 0.0000004260 secs in 50 call(s) to undo_white_move, invocation 1.
Spends 0.0000006590 secs in 186 call(s) to read_endgame, invocation 1.
Spends 0.0000813920 secs in 50 call(s) to black_alpha_beta, invocation 17.
Is called 186 time(s) from black_alpha_beta, invocation 16.

# Summary for block black_alpha_beta, invocation 17.
Spends 0.0000813920 secs in 50 call(s), or 0.00% of total execution time.
Spends 0.0000661810 secs (0.00%) in own code, 0.0000152110 secs (0.00%) in children.

Spends 0.0000002010 secs in 50 call(s) to read_endgame, invocation 1.
Spends 0.0000085080 secs in 43 call(s) to gen_black_moves, invocation 1.
Spends 0.0000000590 secs in 6 call(s) to do_black_move, invocation 1.
Spends 0.0000001170 secs in 6 call(s) to undo_black_move, invocation 1.
Spends 0.0000063260 secs in 6 call(s) to white_alpha_beta, invocation 17.
Is called 50 time(s) from white_alpha_beta, invocation 16.

# Summary for block white_alpha_beta, invocation 17.
Spends 0.0000063260 secs in 6 call(s), or 0.00% of total execution time.
Spends 0.0000060690 secs (0.00%) in own code, 0.0000002570 secs (0.00%) in children.

Spends 0.0000002380 secs in 6 call(s) to gen_white_moves, invocation 1.
Spends 0.0000000190 secs in 6 call(s) to read_endgame, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 17.

# Summary for block black_alpha_beta, invocation 2.
Spends 2.3419906500 secs in 1391 call(s), or 37.37% of total execution time.
Spends 0.0008441960 secs (0.01%) in own code, 2.3411464540 secs (37.36%) in children.

Spends 0.0000969710 secs in 91 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000040930 secs in 1391 call(s) to read_endgame, invocation 1.
Spends 0.0003245570 secs in 1233 call(s) to gen_black_moves, invocation 1.
Spends 0.0000005550 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0000342470 secs in 2699 call(s) to do_black_move, invocation 1.
Spends 0.0000615310 secs in 2699 call(s) to undo_black_move, invocation 1.
Spends 0.0000004050 secs in 35 call(s) to return_black_mcts_best_move, invocation 1.
Spends 2.3406240950 secs in 2717 call(s) to white_alpha_beta, invocation 2.
Is called 1391 time(s) from white_alpha_beta, invocation 1.

# End of profile.
```
