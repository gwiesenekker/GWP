# GWP a high-resolution code profiler for C/C++ source code

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
 //Both the main program and the threads should bracket the main program/thread code with a block "main-thread"
 
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
END_BLOCK

//DUMP_PROFILE should be called once by each thread at the end of the thread 
//VERBOSE can be 0 or 1. Creates files profile-<thread-sequence-number>.txt for each thread.

DUMP_PROFILE(VERBOSE) 
```
BLOCKS can be nested and recursion is supported.

The macro's expand to code that collect the profile information when your program is compiled with -DPROFILE. Obviously BEGIN_BLOCK/END_BLOCK macro's have to match, so multiple returns within procedures and functions should be avoided.

Currently GWP uses hard-coded limits for the number of blocks (BLOCK_MAX), the number of recursive invocations (RECURSE_MAX), the call chain (STACK_MAX) and the number of threads (THREAD_MAX). Increase these as needed if you hit any of these limits.

## Method

GWP needs to collect some information (the time spent, the number of calls, which blocks call which blocks etc) so BEGIN_BLOCK creates static variables in a code block to store this information and to avoid name-clashes with your current code. Because these variables reside in a code block, these are not available outside of this block but END_BLOCK and DUMP_PROFILE need some way to access them. BEGIN_BLOCK links these static variables to global arrays and pointers so that END_BLOCK and DUMP_PROFILE can update and use that information.

GWP uses the following method to clearly separate the time spent in your code and the time needed to collect the profiling information (the profile overhead): when BEGIN_BLOCK/END_BLOCK are entered the time is recorded (you could say that the stopwatch for your code is stopped and the stopwatch for the profiler is started) and when BEGIN_BLOCK/END_BLOCK are exited the time is recorded again (the stopwatch for the profiler is stopped and the stopwatch for your code is started again).

The call stack records the times and the blocks being started and stopped. If there are no nested blocks the code looks like:
```
//BEGIN_BLOCK
Record the time (t1)
Do some administration (the profile overhead):
  Store t1 (confusingly called counter_overhead_begin)
  Record the block in the stack frame
  Setup a pointer that will record the starting time t2 in stack_counter_begin
  Record the time (t2) and store it in that pointer

//END_BLOCK
Record the time (t3)
Do some administration (the profile overhead):
  //Self and total time are the same if there are no nested blocks
  Increment the self-time of the block with (t3 - t2)
  Increment the total-time of the block with (t3 - t2)
  Record the time (t4)
  Store t4 (confusingly called counter_overhead_end)
  Increment the total-time with (t4 - t1)

The profile overhead will be (total-time) - (the self-time of the block)
```
For nested blocks the code looks like:
```
//BEGIN_BLOCK
Record the time (t1)
  Do some administration (the profile overhead):
    //Stop the clock in the previous stack frame and update the self time
    Previous stack_counter_end = t1
    Increment previous stack_time_self = (t1 - previous stack_counter_begin)
  Record the block in the stack frame
  Setup a pointer that will record the time t2 in stack_counter_begin
Record the time (t2) and store it in that pointer

//END_BLOCK
Record the time (t3)
  Do some administration (the profile overhead):
  Increment the self-time of the block with (t3 - t2)
  Increment the total-time of the block with (t3 - t2)
    Increment previous stack_time_total with current stack_time_total
    Setup a pointer that will record the time t4 in previous stack_counter_begin
Record the time (t4)
Store t4 in that pointer
```
## Intrinsic profile overhead
So the minimum unavoidable or intrinsic profile overhead is the sequence
```
Record the time t1
Store the time t2 in a pointer
```
Ideally (t2 - t1) should be zero if there is no code executed between t1 and t2. Recently I started to notice some strange results when profiling small functions (less than 100 ticks) that were called many times, so I decided to take a look at how large the intrinsic profile overhead is using the following program. You can compile it standalone if you are interested in what the intrinsic profile overhead is on your platform.

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

    //note that the distribution is very skewed, so the distribution
    //is not normal
    //deviations LESS than the mean (or the mean - sigma) hardly ever occur
    //but deviations LRRGER than (mean + sigma) do occur more often
 
    //arbitrarily set the standard deviation to one-third of the mean
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
So the intrinsic profile overhead is 190 ticks which is far from negligible when profiling small functions, so we have to correct for it. But as you can see from the results you cannot get it perfectly right as the mean is not constant and sometimes (100 times out of 1M samples) a very large value is returned. I do not know why, it could be related to the performance counters on an AMD 1950X, it could be related to the Linux kernel or to the general problem of returning a meaningful high-resolution thread-specific timer on modern processors. Even if you execute the program on one CPU using taskset the results are not constant:

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
Oddly, for one CPU (7) my AMD 1950x always returns very large largest deviations. So how should we correct for the intrinsic profile overhead? GWP uses the following method: after calculating t2 - t1, GWP takes two samples of the intrinsic profile overhead and subtracts it from t2 - t1. The idea of sampling instead of using a fixed value for the intrinsic profile overhead is that it adjusts for the intrinsic profile overhead at that point in time (perhaps the processor is 'slow'), but as you cannot know which value of the intrinsic profile overhead will be returned this will always be an approximation. It can also happen that (t2 - t1) minus the intrinsic profile overhead is less that zero, especially if a large value for the intrinsic profile overhead is returned. In that case GWP returns zero.
So how well does the correction work? The self-times of the 

## Recursion

Profiling recursive procedures and functions is not easy. GWP solves this problem by profiling each invocation separately. DUMP_PROFILE shows both the time spent in each invocation and summed over invocations.
DUMP_PROFILE will shorten large block names by removing vowels and underscores from the right until they are smaller than 32 characters.

## Example

Here are the results for a 30 second run of my Draughts program GWD:
```
# Profile dumped at 09:23:53-25/11/2018
## Resolution is 1000000000 counts/sec, or 0.0000000010 secs/count.
## Needed 0.0000000016 secs for dummy loop.
## Needed 0.0000003088 secs for querying counter.
## The total number of blocks is 50.
## Total run time was 30.0029084380 secs.
## Total self time was 19.1064846660 secs.
## Total profile overhead was 10.8964237720 secs.

## Blocks sorted by total time spent in block and children.
## The sum of total times (or the sum of the percentages)
## does not have any meaning, since children will be double counted.
name                             invocation   perc       total time      calls estimated error
thread_func                      1          100.00    19.1064846660          1 0.0003958755
black_alpha_beta                 1           99.98    19.1035457910        179 0.0005669481
white_alpha_beta                 1           99.97    19.1015463850        420 0.0028288732
black_alpha_beta                 2           99.92    19.0921369630       2270 0.0048014572
white_alpha_beta                 2           99.84    19.0768137300       3536 0.0188420682
black_alpha_beta                 3           99.53    19.0171929860      16214 0.0288766784
white_alpha_beta                 3           99.06    18.9270871920      20354 0.0961999087
black_alpha_beta                 4           97.51    18.6303767610      86633 0.1234776454
white_alpha_beta                 4           95.56    18.2587785140      84173 0.3070795776
black_alpha_beta                 5           90.73    17.3346482160     273732 0.3144903547
white_alpha_beta                 5           85.92    16.4162179410     204173 0.4546364586
black_alpha_beta                 6           78.70    15.0363098410     363417 0.5472197574
white_alpha_beta                 6           70.11    13.3958574840     388605 0.6880893686
black_alpha_beta                 7           59.29    11.3291561140     518763 0.6848380635
white_alpha_beta                 7           48.53     9.2725743680     441222 0.6442679267
black_alpha_beta                 8           38.22     7.3022278620     420540 0.6161048741
white_alpha_beta                 8           28.27     5.4015454610     384013 0.4780554899
black_alpha_beta                 9           20.47     3.9105551420     260470 0.3683306582
white_alpha_beta                 9           14.49     2.7692576080     221589 0.2804808817
black_alpha_beta                 10           9.91     1.8929936510     152321 0.2141664855
white_alpha_beta                 10           6.46     1.2341349850     128247 0.1592633087
do_white_move                    1            6.45     1.2332186110    2216297 0.6843819729
undo_white_move                  1            6.39     1.2206790600    2216297 0.6843819729
do_black_move                    1            5.71     1.0902130560    1954590 0.6035680960
undo_black_move                  1            5.65     1.0789288200    1954590 0.6035680960
gen_white_moves                  1            5.28     1.0093139050    1516131 0.4681740421
gen_black_moves                  1            5.26     1.0043648060    1543211 0.4765362173
black_alpha_beta                 11           3.85     0.7352170110      83067 0.1086310789
white_alpha_beta                 11           2.11     0.4022262830      61062 0.0708289827
return_pattern_score_double      1            1.67     0.3187345400     403437 0.1245794269
black_alpha_beta                 12           0.94     0.1797970230      33478 0.0356318832
white_alpha_beta                 12           0.37     0.0700327290      15918 0.0151164536
choose_workload_next_move        1            0.20     0.0375117750      64685 0.0199744204
update_workloadmvbstmvscrpvdpth  1            0.16     0.0307639390      54142 0.0167187921
black_alpha_beta                 13           0.12     0.0225022090       5476 0.0049367096
return_my_timer                  1            0.04     0.0079264430       8381 0.0025880129
white_alpha_beta                 13           0.04     0.0078385460       2121 0.0018685200
publish_workload                 1            0.03     0.0061192650       8429 0.0026028351
unpublish_workload               1            0.03     0.0050871690       8428 0.0026025263
return_workload_bestmvscrpvdpth  1            0.03     0.0050285870       8427 0.0026022175
poll_queue                       1            0.01     0.0025935330       4173 0.0012886026
black_alpha_beta                 14           0.01     0.0022084690        722 0.0005243343
white_alpha_beta                 14           0.00     0.0006924940        213 0.0001627351
black_alpha_beta                 15           0.00     0.0001959610         50 0.0000240860
white_alpha_beta                 15           0.00     0.0001356110          5 0.0000052495
black_alpha_beta                 16           0.00     0.0001176840          3 0.0000064847
white_alpha_beta                 16           0.00     0.0000944320          5 0.0000052495
black_alpha_beta                 17           0.00     0.0000591030          3 0.0000074111
check_white_moves                1            0.00     0.0000429610          1 0.0000064847
white_alpha_beta                 17           0.00     0.0000242890          6 0.0000049407

## Blocks sorted by total time spent in own code.
## The sum of the self times is equal to the total self time.
name                             invocation   perc        self time      calls estimated error
black_alpha_beta                 7            6.89     1.3173903570     518763 0.6848380635
white_alpha_beta                 6            6.81     1.3004780300     388605 0.6880893686
white_alpha_beta                 7            6.50     1.2424828620     441222 0.6442679267
do_white_move                    1            6.45     1.2332186110    2216297 0.6843819729
undo_white_move                  1            6.39     1.2206790600    2216297 0.6843819729
black_alpha_beta                 8            6.25     1.1942906240     420540 0.6161048741
do_black_move                    1            5.71     1.0902130560    1954590 0.6035680960
undo_black_move                  1            5.65     1.0789288200    1954590 0.6035680960
black_alpha_beta                 6            5.46     1.0437073100     363417 0.5472197574
gen_white_moves                  1            5.28     1.0093139050    1516131 0.4681740421
gen_black_moves                  1            5.26     1.0043648060    1543211 0.4765362173
white_alpha_beta                 8            4.93     0.9426774200     384013 0.4780554899
white_alpha_beta                 5            4.48     0.8551403070     204173 0.4546364586
black_alpha_beta                 9            3.76     0.7192048980     260470 0.3683306582
black_alpha_beta                 5            3.15     0.6020803520     273732 0.3144903547
white_alpha_beta                 4            2.95     0.5635955110      84173 0.3070795776
white_alpha_beta                 9            2.90     0.5532378630     221589 0.2804808817
black_alpha_beta                 10           2.18     0.4158395500     152321 0.2141664855
return_pattern_score_double      1            1.67     0.3187345400     403437 0.1245794269
white_alpha_beta                 10           1.64     0.3138144130     128247 0.1592633087
black_alpha_beta                 4            1.24     0.2370949870      86633 0.1234776454
black_alpha_beta                 11           1.10     0.2106623690      83067 0.1086310789
white_alpha_beta                 3            0.94     0.1787409040      20354 0.0961999087
white_alpha_beta                 11           0.73     0.1399298350      61062 0.0708289827
black_alpha_beta                 12           0.37     0.0704291190      33478 0.0356318832
black_alpha_beta                 3            0.29     0.0561222770      16214 0.0288766784
choose_workload_next_move        1            0.20     0.0375117750      64685 0.0199744204
white_alpha_beta                 2            0.19     0.0353606640       3536 0.0188420682
update_workloadmvbstmvscrpvdpth  1            0.16     0.0307639390      54142 0.0167187921
white_alpha_beta                 12           0.16     0.0304279090      15918 0.0151164536
black_alpha_beta                 13           0.05     0.0097221930       5476 0.0049367096
black_alpha_beta                 2            0.05     0.0094340740       2270 0.0048014572
return_my_timer                  1            0.04     0.0079264430       8381 0.0025880129
publish_workload                 1            0.03     0.0061192650       8429 0.0026028351
white_alpha_beta                 1            0.03     0.0054975860        420 0.0028288732
unpublish_workload               1            0.03     0.0050871690       8428 0.0026025263
return_workload_bestmvscrpvdpth  1            0.03     0.0050285870       8427 0.0026022175
white_alpha_beta                 13           0.02     0.0037117540       2121 0.0018685200
poll_queue                       1            0.01     0.0025935330       4173 0.0012886026
thread_func                      1            0.01     0.0021906930          1 0.0003958755
black_alpha_beta                 1            0.01     0.0011989990        179 0.0005669481
black_alpha_beta                 14           0.01     0.0010314690        722 0.0005243343
white_alpha_beta                 14           0.00     0.0003344890        213 0.0001627351
black_alpha_beta                 15           0.00     0.0000463670         50 0.0000240860
check_white_moves                1            0.00     0.0000312190          1 0.0000064847
white_alpha_beta                 16           0.00     0.0000293150          5 0.0000052495
black_alpha_beta                 17           0.00     0.0000221030          3 0.0000074111
white_alpha_beta                 17           0.00     0.0000165130          6 0.0000049407
black_alpha_beta                 16           0.00     0.0000149820          3 0.0000064847
white_alpha_beta                 15           0.00     0.0000118400          5 0.0000052495

## Blocks sorted by self times summed over recursive invocations.
name                               perc        self time      calls   self time/call
white_alpha_beta                  32.27     6.1654872150    1955662     0.0000031526
black_alpha_beta                  30.82     5.8882920300    2217338     0.0000026556
do_white_move                      6.45     1.2332186110    2216297     0.0000005564
undo_white_move                    6.39     1.2206790600    2216297     0.0000005508
do_black_move                      5.71     1.0902130560    1954590     0.0000005578
undo_black_move                    5.65     1.0789288200    1954590     0.0000005520
gen_white_moves                    5.28     1.0093139050    1516131     0.0000006657
gen_black_moves                    5.26     1.0043648060    1543211     0.0000006508
return_pattern_score_double        1.67     0.3187345400     403437     0.0000007900
choose_workload_next_move          0.20     0.0375117750      64685     0.0000005799
update_workloadmvbstmvscrpvdpth    0.16     0.0307639390      54142     0.0000005682
return_my_timer                    0.04     0.0079264430       8381     0.0000009458
publish_workload                   0.03     0.0061192650       8429     0.0000007260
unpublish_workload                 0.03     0.0050871690       8428     0.0000006036
return_workload_bestmvscrpvdpth    0.03     0.0050285870       8427     0.0000005967
poll_queue                         0.01     0.0025935330       4173     0.0000006215
thread_func                        0.01     0.0021906930          1     0.0021906930
check_white_moves                  0.00     0.0000312190          1     0.0000312190

## Summary for block white_alpha_beta, invocation 1.
Spends 19.1015463850 secs in 420 call(s), or 99.97% of total execution time.
Spends 0.0054975860 secs (0.03%) in own code, 19.0960487990 secs (99.95%) in children.

Spends 0.0002883710 secs in 399 call(s) to gen_white_moves, invocation 1.
Spends 0.0013206110 secs in 2261 call(s) to do_white_move, invocation 1.
Spends 0.0013708140 secs in 2261 call(s) to undo_white_move, invocation 1.
Spends 0.0000161920 secs in 19 call(s) to return_pattern_score_double, invocation 1.
Spends 19.0921369630 secs in 2270 call(s) to black_alpha_beta, invocation 2.
Spends 0.0000511380 secs in 71 call(s) to publish_workload, invocation 1.
Spends 0.0004317390 secs in 722 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0003486720 secs in 596 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000422920 secs in 71 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000420070 secs in 71 call(s) to unpublish_workload, invocation 1.
Is called 420 time(s) from black_alpha_beta, invocation 1.

## Summary for block black_alpha_beta, invocation 1.
Spends 19.1035457910 secs in 179 call(s), or 99.98% of total execution time.
Spends 0.0011989990 secs (0.01%) in own code, 19.1023467920 secs (99.98%) in children.

Spends 0.0000050090 secs in 6 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001915460 secs in 179 call(s) to gen_black_moves, invocation 1.
Spends 0.0002270250 secs in 409 call(s) to do_black_move, invocation 1.
Spends 19.1015463850 secs in 420 call(s) to white_alpha_beta, invocation 1.
Spends 0.0002391440 secs in 409 call(s) to undo_black_move, invocation 1.
Spends 0.0000017520 secs in 2 call(s) to return_my_timer, invocation 1.
Spends 0.0000005530 secs in 1 call(s) to poll_queue, invocation 1.
Spends 0.0000078530 secs in 11 call(s) to publish_workload, invocation 1.
Spends 0.0000615880 secs in 107 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0000524480 secs in 91 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000067070 secs in 11 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000067820 secs in 11 call(s) to unpublish_workload, invocation 1.
Is called 179 time(s) from thread_func, invocation 1.

## Summary for block do_white_move, invocation 1.
Spends 1.2332186110 secs in 2216297 call(s), or 6.45% of total execution time.
Spends 1.2332186110 secs (6.45%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 301 time(s) from thread_func, invocation 1.
Is called 10 time(s) from check_white_moves, invocation 1.
Is called 2261 time(s) from white_alpha_beta, invocation 1.
Is called 16196 time(s) from white_alpha_beta, invocation 2.
Is called 86616 time(s) from white_alpha_beta, invocation 3.
Is called 273690 time(s) from white_alpha_beta, invocation 4.
Is called 363322 time(s) from white_alpha_beta, invocation 5.
Is called 518590 time(s) from white_alpha_beta, invocation 6.
Is called 420310 time(s) from white_alpha_beta, invocation 7.
Is called 260213 time(s) from white_alpha_beta, invocation 8.
Is called 152144 time(s) from white_alpha_beta, invocation 9.
Is called 82944 time(s) from white_alpha_beta, invocation 10.
Is called 33448 time(s) from white_alpha_beta, invocation 11.
Is called 5474 time(s) from white_alpha_beta, invocation 12.
Is called 722 time(s) from white_alpha_beta, invocation 13.
Is called 50 time(s) from white_alpha_beta, invocation 14.
Is called 3 time(s) from white_alpha_beta, invocation 15.
Is called 3 time(s) from white_alpha_beta, invocation 16.

## Summary for block undo_white_move, invocation 1.
Spends 1.2206790600 secs in 2216297 call(s), or 6.39% of total execution time.
Spends 1.2206790600 secs (6.39%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 301 time(s) from thread_func, invocation 1.
Is called 10 time(s) from check_white_moves, invocation 1.
Is called 2261 time(s) from white_alpha_beta, invocation 1.
Is called 16196 time(s) from white_alpha_beta, invocation 2.
Is called 86616 time(s) from white_alpha_beta, invocation 3.
Is called 273690 time(s) from white_alpha_beta, invocation 4.
Is called 363322 time(s) from white_alpha_beta, invocation 5.
Is called 518590 time(s) from white_alpha_beta, invocation 6.
Is called 420310 time(s) from white_alpha_beta, invocation 7.
Is called 260213 time(s) from white_alpha_beta, invocation 8.
Is called 152144 time(s) from white_alpha_beta, invocation 9.
Is called 82944 time(s) from white_alpha_beta, invocation 10.
Is called 33448 time(s) from white_alpha_beta, invocation 11.
Is called 5474 time(s) from white_alpha_beta, invocation 12.
Is called 722 time(s) from white_alpha_beta, invocation 13.
Is called 50 time(s) from white_alpha_beta, invocation 14.
Is called 3 time(s) from white_alpha_beta, invocation 15.
Is called 3 time(s) from white_alpha_beta, invocation 16.

## Summary for block do_black_move, invocation 1.
Spends 1.0902130560 secs in 1954590 call(s), or 5.71% of total execution time.
Spends 1.0902130560 secs (5.71%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 103 time(s) from thread_func, invocation 1.
Is called 409 time(s) from black_alpha_beta, invocation 1.
Is called 3513 time(s) from black_alpha_beta, invocation 2.
Is called 20326 time(s) from black_alpha_beta, invocation 3.
Is called 84123 time(s) from black_alpha_beta, invocation 4.
Is called 204095 time(s) from black_alpha_beta, invocation 5.
Is called 388479 time(s) from black_alpha_beta, invocation 6.
Is called 441010 time(s) from black_alpha_beta, invocation 7.
Is called 383812 time(s) from black_alpha_beta, invocation 8.
Is called 221377 time(s) from black_alpha_beta, invocation 9.
Is called 128058 time(s) from black_alpha_beta, invocation 10.
Is called 61020 time(s) from black_alpha_beta, invocation 11.
Is called 15915 time(s) from black_alpha_beta, invocation 12.
Is called 2121 time(s) from black_alpha_beta, invocation 13.
Is called 213 time(s) from black_alpha_beta, invocation 14.
Is called 5 time(s) from black_alpha_beta, invocation 15.
Is called 5 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

## Summary for block undo_black_move, invocation 1.
Spends 1.0789288200 secs in 1954590 call(s), or 5.65% of total execution time.
Spends 1.0789288200 secs (5.65%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 103 time(s) from thread_func, invocation 1.
Is called 409 time(s) from black_alpha_beta, invocation 1.
Is called 3513 time(s) from black_alpha_beta, invocation 2.
Is called 20326 time(s) from black_alpha_beta, invocation 3.
Is called 84123 time(s) from black_alpha_beta, invocation 4.
Is called 204095 time(s) from black_alpha_beta, invocation 5.
Is called 388479 time(s) from black_alpha_beta, invocation 6.
Is called 441010 time(s) from black_alpha_beta, invocation 7.
Is called 383812 time(s) from black_alpha_beta, invocation 8.
Is called 221377 time(s) from black_alpha_beta, invocation 9.
Is called 128058 time(s) from black_alpha_beta, invocation 10.
Is called 61020 time(s) from black_alpha_beta, invocation 11.
Is called 15915 time(s) from black_alpha_beta, invocation 12.
Is called 2121 time(s) from black_alpha_beta, invocation 13.
Is called 213 time(s) from black_alpha_beta, invocation 14.
Is called 5 time(s) from black_alpha_beta, invocation 15.
Is called 5 time(s) from black_alpha_beta, invocation 16.
Is called 6 time(s) from black_alpha_beta, invocation 17.

## Summary for block gen_white_moves, invocation 1.
Spends 1.0093139050 secs in 1516131 call(s), or 5.28% of total execution time.
Spends 1.0093139050 secs (5.28%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 104 time(s) from thread_func, invocation 1.
Is called 399 time(s) from white_alpha_beta, invocation 1.
Is called 3286 time(s) from white_alpha_beta, invocation 2.
Is called 17643 time(s) from white_alpha_beta, invocation 3.
Is called 66028 time(s) from white_alpha_beta, invocation 4.
Is called 142327 time(s) from white_alpha_beta, invocation 5.
Is called 246846 time(s) from white_alpha_beta, invocation 6.
Is called 329095 time(s) from white_alpha_beta, invocation 7.
Is called 323782 time(s) from white_alpha_beta, invocation 8.
Is called 195647 time(s) from white_alpha_beta, invocation 9.
Is called 117154 time(s) from white_alpha_beta, invocation 10.
Is called 57272 time(s) from white_alpha_beta, invocation 11.
Is called 14707 time(s) from white_alpha_beta, invocation 12.
Is called 1676 time(s) from white_alpha_beta, invocation 13.
Is called 153 time(s) from white_alpha_beta, invocation 14.
Is called 3 time(s) from white_alpha_beta, invocation 15.
Is called 3 time(s) from white_alpha_beta, invocation 16.
Is called 6 time(s) from white_alpha_beta, invocation 17.

## Summary for block gen_black_moves, invocation 1.
Spends 1.0043648060 secs in 1543211 call(s), or 5.26% of total execution time.
Spends 1.0043648060 secs (5.26%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 122 time(s) from thread_func, invocation 1.
Is called 179 time(s) from black_alpha_beta, invocation 1.
Is called 1977 time(s) from black_alpha_beta, invocation 2.
Is called 12338 time(s) from black_alpha_beta, invocation 3.
Is called 48264 time(s) from black_alpha_beta, invocation 4.
Is called 105659 time(s) from black_alpha_beta, invocation 5.
Is called 209645 time(s) from black_alpha_beta, invocation 6.
Is called 326346 time(s) from black_alpha_beta, invocation 7.
Is called 363642 time(s) from black_alpha_beta, invocation 8.
Is called 231762 time(s) from black_alpha_beta, invocation 9.
Is called 135027 time(s) from black_alpha_beta, invocation 10.
Is called 74001 time(s) from black_alpha_beta, invocation 11.
Is called 30037 time(s) from black_alpha_beta, invocation 12.
Is called 3871 time(s) from black_alpha_beta, invocation 13.
Is called 322 time(s) from black_alpha_beta, invocation 14.
Is called 13 time(s) from black_alpha_beta, invocation 15.
Is called 3 time(s) from black_alpha_beta, invocation 16.
Is called 3 time(s) from black_alpha_beta, invocation 17.

## Summary for block return_pattern_score_double, invocation 1.
Spends 0.3187345400 secs in 403437 call(s), or 1.67% of total execution time.
Spends 0.3187345400 secs (1.67%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 32 time(s) from thread_func, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 1.
Is called 19 time(s) from white_alpha_beta, invocation 1.
Is called 45 time(s) from black_alpha_beta, invocation 2.
Is called 163 time(s) from white_alpha_beta, invocation 2.
Is called 337 time(s) from black_alpha_beta, invocation 3.
Is called 658 time(s) from white_alpha_beta, invocation 3.
Is called 1328 time(s) from black_alpha_beta, invocation 4.
Is called 2532 time(s) from white_alpha_beta, invocation 4.
Is called 6206 time(s) from black_alpha_beta, invocation 5.
Is called 11446 time(s) from white_alpha_beta, invocation 5.
Is called 19114 time(s) from black_alpha_beta, invocation 6.
Is called 29408 time(s) from white_alpha_beta, invocation 6.
Is called 39094 time(s) from black_alpha_beta, invocation 7.
Is called 48740 time(s) from white_alpha_beta, invocation 7.
Is called 52479 time(s) from black_alpha_beta, invocation 8.
Is called 55581 time(s) from white_alpha_beta, invocation 8.
Is called 33064 time(s) from black_alpha_beta, invocation 9.
Is called 33002 time(s) from white_alpha_beta, invocation 9.
Is called 21144 time(s) from black_alpha_beta, invocation 10.
Is called 20962 time(s) from white_alpha_beta, invocation 10.
Is called 11356 time(s) from black_alpha_beta, invocation 11.
Is called 10469 time(s) from white_alpha_beta, invocation 11.
Is called 4013 time(s) from black_alpha_beta, invocation 12.
Is called 1859 time(s) from white_alpha_beta, invocation 12.
Is called 271 time(s) from black_alpha_beta, invocation 13.
Is called 79 time(s) from white_alpha_beta, invocation 13.
Is called 15 time(s) from black_alpha_beta, invocation 14.
Is called 11 time(s) from white_alpha_beta, invocation 14.
Is called 4 time(s) from white_alpha_beta, invocation 17.

## Summary for block choose_workload_next_move, invocation 1.
Spends 0.0375117750 secs in 64685 call(s), or 0.20% of total execution time.
Spends 0.0375117750 secs (0.20%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 107 time(s) from black_alpha_beta, invocation 1.
Is called 722 time(s) from white_alpha_beta, invocation 1.
Is called 313 time(s) from black_alpha_beta, invocation 2.
Is called 2506 time(s) from white_alpha_beta, invocation 2.
Is called 1600 time(s) from black_alpha_beta, invocation 3.
Is called 6028 time(s) from white_alpha_beta, invocation 3.
Is called 4971 time(s) from black_alpha_beta, invocation 4.
Is called 9325 time(s) from white_alpha_beta, invocation 4.
Is called 8846 time(s) from black_alpha_beta, invocation 5.
Is called 10508 time(s) from white_alpha_beta, invocation 5.
Is called 5890 time(s) from black_alpha_beta, invocation 6.
Is called 2648 time(s) from white_alpha_beta, invocation 6.
Is called 3963 time(s) from black_alpha_beta, invocation 7.
Is called 2031 time(s) from white_alpha_beta, invocation 7.
Is called 2468 time(s) from black_alpha_beta, invocation 8.
Is called 1179 time(s) from white_alpha_beta, invocation 8.
Is called 1080 time(s) from black_alpha_beta, invocation 9.
Is called 366 time(s) from white_alpha_beta, invocation 9.
Is called 118 time(s) from black_alpha_beta, invocation 10.
Is called 16 time(s) from white_alpha_beta, invocation 10.

## Summary for block update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0307639390 secs in 54142 call(s), or 0.16% of total execution time.
Spends 0.0307639390 secs (0.16%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 91 time(s) from black_alpha_beta, invocation 1.
Is called 596 time(s) from white_alpha_beta, invocation 1.
Is called 272 time(s) from black_alpha_beta, invocation 2.
Is called 2090 time(s) from white_alpha_beta, invocation 2.
Is called 1380 time(s) from black_alpha_beta, invocation 3.
Is called 4972 time(s) from white_alpha_beta, invocation 3.
Is called 4163 time(s) from black_alpha_beta, invocation 4.
Is called 7779 time(s) from white_alpha_beta, invocation 4.
Is called 7374 time(s) from black_alpha_beta, invocation 5.
Is called 8922 time(s) from white_alpha_beta, invocation 5.
Is called 4802 time(s) from black_alpha_beta, invocation 6.
Is called 2300 time(s) from white_alpha_beta, invocation 6.
Is called 3255 time(s) from black_alpha_beta, invocation 7.
Is called 1743 time(s) from white_alpha_beta, invocation 7.
Is called 2104 time(s) from black_alpha_beta, invocation 8.
Is called 997 time(s) from white_alpha_beta, invocation 8.
Is called 910 time(s) from black_alpha_beta, invocation 9.
Is called 290 time(s) from white_alpha_beta, invocation 9.
Is called 90 time(s) from black_alpha_beta, invocation 10.
Is called 12 time(s) from white_alpha_beta, invocation 10.

## Summary for block return_my_timer, invocation 1.
Spends 0.0079264430 secs in 8381 call(s), or 0.04% of total execution time.
Spends 0.0079264430 secs (0.04%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 35 time(s) from thread_func, invocation 1.
Is called 2 time(s) from black_alpha_beta, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 2.
Is called 6 time(s) from white_alpha_beta, invocation 2.
Is called 30 time(s) from black_alpha_beta, invocation 3.
Is called 34 time(s) from white_alpha_beta, invocation 3.
Is called 168 time(s) from black_alpha_beta, invocation 4.
Is called 144 time(s) from white_alpha_beta, invocation 4.
Is called 560 time(s) from black_alpha_beta, invocation 5.
Is called 402 time(s) from white_alpha_beta, invocation 5.
Is called 750 time(s) from black_alpha_beta, invocation 6.
Is called 764 time(s) from white_alpha_beta, invocation 6.
Is called 1018 time(s) from black_alpha_beta, invocation 7.
Is called 916 time(s) from white_alpha_beta, invocation 7.
Is called 846 time(s) from black_alpha_beta, invocation 8.
Is called 758 time(s) from white_alpha_beta, invocation 8.
Is called 506 time(s) from black_alpha_beta, invocation 9.
Is called 446 time(s) from white_alpha_beta, invocation 9.
Is called 298 time(s) from black_alpha_beta, invocation 10.
Is called 270 time(s) from white_alpha_beta, invocation 10.
Is called 176 time(s) from black_alpha_beta, invocation 11.
Is called 130 time(s) from white_alpha_beta, invocation 11.
Is called 76 time(s) from black_alpha_beta, invocation 12.
Is called 30 time(s) from white_alpha_beta, invocation 12.
Is called 4 time(s) from black_alpha_beta, invocation 13.
Is called 6 time(s) from white_alpha_beta, invocation 13.

## Summary for block publish_workload, invocation 1.
Spends 0.0061192650 secs in 8429 call(s), or 0.03% of total execution time.
Spends 0.0061192650 secs (0.03%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 11 time(s) from black_alpha_beta, invocation 1.
Is called 71 time(s) from white_alpha_beta, invocation 1.
Is called 34 time(s) from black_alpha_beta, invocation 2.
Is called 274 time(s) from white_alpha_beta, invocation 2.
Is called 198 time(s) from black_alpha_beta, invocation 3.
Is called 654 time(s) from white_alpha_beta, invocation 3.
Is called 613 time(s) from black_alpha_beta, invocation 4.
Is called 1093 time(s) from white_alpha_beta, invocation 4.
Is called 1141 time(s) from black_alpha_beta, invocation 5.
Is called 1417 time(s) from white_alpha_beta, invocation 5.
Is called 852 time(s) from black_alpha_beta, invocation 6.
Is called 469 time(s) from white_alpha_beta, invocation 6.
Is called 528 time(s) from black_alpha_beta, invocation 7.
Is called 343 time(s) from white_alpha_beta, invocation 7.
Is called 350 time(s) from black_alpha_beta, invocation 8.
Is called 182 time(s) from white_alpha_beta, invocation 8.
Is called 137 time(s) from black_alpha_beta, invocation 9.
Is called 45 time(s) from white_alpha_beta, invocation 9.
Is called 15 time(s) from black_alpha_beta, invocation 10.
Is called 2 time(s) from white_alpha_beta, invocation 10.

## Summary for block unpublish_workload, invocation 1.
Spends 0.0050871690 secs in 8428 call(s), or 0.03% of total execution time.
Spends 0.0050871690 secs (0.03%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 11 time(s) from black_alpha_beta, invocation 1.
Is called 71 time(s) from white_alpha_beta, invocation 1.
Is called 34 time(s) from black_alpha_beta, invocation 2.
Is called 274 time(s) from white_alpha_beta, invocation 2.
Is called 198 time(s) from black_alpha_beta, invocation 3.
Is called 654 time(s) from white_alpha_beta, invocation 3.
Is called 613 time(s) from black_alpha_beta, invocation 4.
Is called 1093 time(s) from white_alpha_beta, invocation 4.
Is called 1141 time(s) from black_alpha_beta, invocation 5.
Is called 1417 time(s) from white_alpha_beta, invocation 5.
Is called 852 time(s) from black_alpha_beta, invocation 6.
Is called 469 time(s) from white_alpha_beta, invocation 6.
Is called 528 time(s) from black_alpha_beta, invocation 7.
Is called 342 time(s) from white_alpha_beta, invocation 7.
Is called 350 time(s) from black_alpha_beta, invocation 8.
Is called 182 time(s) from white_alpha_beta, invocation 8.
Is called 137 time(s) from black_alpha_beta, invocation 9.
Is called 45 time(s) from white_alpha_beta, invocation 9.
Is called 15 time(s) from black_alpha_beta, invocation 10.
Is called 2 time(s) from white_alpha_beta, invocation 10.

## Summary for block return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0050285870 secs in 8427 call(s), or 0.03% of total execution time.
Spends 0.0050285870 secs (0.03%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 11 time(s) from black_alpha_beta, invocation 1.
Is called 71 time(s) from white_alpha_beta, invocation 1.
Is called 33 time(s) from black_alpha_beta, invocation 2.
Is called 274 time(s) from white_alpha_beta, invocation 2.
Is called 198 time(s) from black_alpha_beta, invocation 3.
Is called 654 time(s) from white_alpha_beta, invocation 3.
Is called 613 time(s) from black_alpha_beta, invocation 4.
Is called 1093 time(s) from white_alpha_beta, invocation 4.
Is called 1141 time(s) from black_alpha_beta, invocation 5.
Is called 1417 time(s) from white_alpha_beta, invocation 5.
Is called 852 time(s) from black_alpha_beta, invocation 6.
Is called 469 time(s) from white_alpha_beta, invocation 6.
Is called 528 time(s) from black_alpha_beta, invocation 7.
Is called 342 time(s) from white_alpha_beta, invocation 7.
Is called 350 time(s) from black_alpha_beta, invocation 8.
Is called 182 time(s) from white_alpha_beta, invocation 8.
Is called 137 time(s) from black_alpha_beta, invocation 9.
Is called 45 time(s) from white_alpha_beta, invocation 9.
Is called 15 time(s) from black_alpha_beta, invocation 10.
Is called 2 time(s) from white_alpha_beta, invocation 10.

## Summary for block poll_queue, invocation 1.
Spends 0.0025935330 secs in 4173 call(s), or 0.01% of total execution time.
Spends 0.0025935330 secs (0.01%) in own code, 0.0000000000 secs (0.00%) in children.

No children were found.
Is called 1 time(s) from black_alpha_beta, invocation 1.
Is called 3 time(s) from black_alpha_beta, invocation 2.
Is called 3 time(s) from white_alpha_beta, invocation 2.
Is called 15 time(s) from black_alpha_beta, invocation 3.
Is called 17 time(s) from white_alpha_beta, invocation 3.
Is called 84 time(s) from black_alpha_beta, invocation 4.
Is called 72 time(s) from white_alpha_beta, invocation 4.
Is called 280 time(s) from black_alpha_beta, invocation 5.
Is called 201 time(s) from white_alpha_beta, invocation 5.
Is called 375 time(s) from black_alpha_beta, invocation 6.
Is called 382 time(s) from white_alpha_beta, invocation 6.
Is called 509 time(s) from black_alpha_beta, invocation 7.
Is called 458 time(s) from white_alpha_beta, invocation 7.
Is called 423 time(s) from black_alpha_beta, invocation 8.
Is called 379 time(s) from white_alpha_beta, invocation 8.
Is called 253 time(s) from black_alpha_beta, invocation 9.
Is called 223 time(s) from white_alpha_beta, invocation 9.
Is called 149 time(s) from black_alpha_beta, invocation 10.
Is called 135 time(s) from white_alpha_beta, invocation 10.
Is called 88 time(s) from black_alpha_beta, invocation 11.
Is called 65 time(s) from white_alpha_beta, invocation 11.
Is called 38 time(s) from black_alpha_beta, invocation 12.
Is called 15 time(s) from white_alpha_beta, invocation 12.
Is called 2 time(s) from black_alpha_beta, invocation 13.
Is called 3 time(s) from white_alpha_beta, invocation 13.

## Summary for block thread_func, invocation 1.
Spends 19.1064846660 secs in 1 call(s), or 100.00% of total execution time.
Spends 0.0021906930 secs (0.01%) in own code, 19.1042939730 secs (99.99%) in children.

Spends 0.0000695620 secs in 104 call(s) to gen_white_moves, invocation 1.
Spends 0.0000429610 secs in 1 call(s) to check_white_moves, invocation 1.
Spends 0.0001850280 secs in 301 call(s) to do_white_move, invocation 1.
Spends 0.0001766230 secs in 301 call(s) to undo_white_move, invocation 1.
Spends 0.0000307460 secs in 32 call(s) to return_pattern_score_double, invocation 1.
Spends 19.1035457910 secs in 179 call(s) to black_alpha_beta, invocation 1.
Spends 0.0000836950 secs in 122 call(s) to gen_black_moves, invocation 1.
Spends 0.0000558430 secs in 103 call(s) to do_black_move, invocation 1.
Spends 0.0000552370 secs in 103 call(s) to undo_black_move, invocation 1.
Spends 0.0000484870 secs in 35 call(s) to return_my_timer, invocation 1.
No parents were found

## Summary for block check_white_moves, invocation 1.
Spends 0.0000429610 secs in 1 call(s), or 0.00% of total execution time.
Spends 0.0000312190 secs (0.00%) in own code, 0.0000117420 secs (0.00%) in children.

Spends 0.0000059740 secs in 10 call(s) to do_white_move, invocation 1.
Spends 0.0000057680 secs in 10 call(s) to undo_white_move, invocation 1.
Is called 1 time(s) from thread_func, invocation 1.

## Summary for block black_alpha_beta, invocation 5.
Spends 17.3346482160 secs in 273732 call(s), or 90.73% of total execution time.
Spends 0.6020803520 secs (3.15%) in own code, 16.7325678640 secs (87.58%) in children.

Spends 0.0050568360 secs in 6206 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0711786360 secs in 105659 call(s) to gen_black_moves, invocation 1.
Spends 0.1143291860 secs in 204095 call(s) to do_black_move, invocation 1.
Spends 0.1135190300 secs in 204095 call(s) to undo_black_move, invocation 1.
Spends 0.0005153980 secs in 560 call(s) to return_my_timer, invocation 1.
Spends 16.4162179410 secs in 204173 call(s) to white_alpha_beta, invocation 5.
Spends 0.0001661610 secs in 280 call(s) to poll_queue, invocation 1.
Spends 0.0008523100 secs in 1141 call(s) to publish_workload, invocation 1.
Spends 0.0051391500 secs in 8846 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0042165790 secs in 7374 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0006975310 secs in 1141 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0006791060 secs in 1141 call(s) to unpublish_workload, invocation 1.
Is called 273732 time(s) from white_alpha_beta, invocation 4.

## Summary for block white_alpha_beta, invocation 5.
Spends 16.4162179410 secs in 204173 call(s), or 85.92% of total execution time.
Spends 0.8551403070 secs (4.48%) in own code, 15.5610776340 secs (81.44%) in children.

Spends 0.0962263550 secs in 142327 call(s) to gen_white_moves, invocation 1.
Spends 0.2033467600 secs in 363322 call(s) to do_white_move, invocation 1.
Spends 0.2015864090 secs in 363322 call(s) to undo_white_move, invocation 1.
Spends 0.0090907450 secs in 11446 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0003889130 secs in 402 call(s) to return_my_timer, invocation 1.
Spends 15.0363098410 secs in 363417 call(s) to black_alpha_beta, invocation 6.
Spends 0.0001385030 secs in 201 call(s) to poll_queue, invocation 1.
Spends 0.0010698940 secs in 1417 call(s) to publish_workload, invocation 1.
Spends 0.0061954340 secs in 10508 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0049517390 secs in 8922 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0009058490 secs in 1417 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0008671920 secs in 1417 call(s) to unpublish_workload, invocation 1.
Is called 204173 time(s) from black_alpha_beta, invocation 5.

## Summary for block black_alpha_beta, invocation 6.
Spends 15.0363098410 secs in 363417 call(s), or 78.70% of total execution time.
Spends 1.0437073100 secs (5.46%) in own code, 13.9926025310 secs (73.23%) in children.

Spends 0.0150928060 secs in 19114 call(s) to return_pattern_score_double, invocation 1.
Spends 0.1392751930 secs in 209645 call(s) to gen_black_moves, invocation 1.
Spends 0.2180482180 secs in 388479 call(s) to do_black_move, invocation 1.
Spends 0.2156391460 secs in 388479 call(s) to undo_black_move, invocation 1.
Spends 0.0007173280 secs in 750 call(s) to return_my_timer, invocation 1.
Spends 0.0002318540 secs in 375 call(s) to poll_queue, invocation 1.
Spends 0.0006212000 secs in 852 call(s) to publish_workload, invocation 1.
Spends 0.0033237930 secs in 5890 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0027622070 secs in 4802 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0004972950 secs in 852 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0005360070 secs in 852 call(s) to unpublish_workload, invocation 1.
Spends 13.3958574840 secs in 388605 call(s) to white_alpha_beta, invocation 6.
Is called 363417 time(s) from white_alpha_beta, invocation 5.

## Summary for block white_alpha_beta, invocation 3.
Spends 18.9270871920 secs in 20354 call(s), or 99.06% of total execution time.
Spends 0.1787409040 secs (0.94%) in own code, 18.7483462880 secs (98.13%) in children.

Spends 0.0121584030 secs in 17643 call(s) to gen_white_moves, invocation 1.
Spends 0.0491136770 secs in 86616 call(s) to do_white_move, invocation 1.
Spends 0.0486349800 secs in 86616 call(s) to undo_white_move, invocation 1.
Spends 0.0005474880 secs in 658 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000311580 secs in 34 call(s) to return_my_timer, invocation 1.
Spends 18.6303767610 secs in 86633 call(s) to black_alpha_beta, invocation 4.
Spends 0.0000107210 secs in 17 call(s) to poll_queue, invocation 1.
Spends 0.0004611030 secs in 654 call(s) to publish_workload, invocation 1.
Spends 0.0035178280 secs in 6028 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0027251090 secs in 4972 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0003633360 secs in 654 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0004057240 secs in 654 call(s) to unpublish_workload, invocation 1.
Is called 20354 time(s) from black_alpha_beta, invocation 3.

## Summary for block black_alpha_beta, invocation 2.
Spends 19.0921369630 secs in 2270 call(s), or 99.92% of total execution time.
Spends 0.0094340740 secs (0.05%) in own code, 19.0827028890 secs (99.88%) in children.

Spends 0.0000356030 secs in 45 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0013696730 secs in 1977 call(s) to gen_black_moves, invocation 1.
Spends 0.0020489870 secs in 3513 call(s) to do_black_move, invocation 1.
Spends 0.0020094610 secs in 3513 call(s) to undo_black_move, invocation 1.
Spends 0.0000054170 secs in 6 call(s) to return_my_timer, invocation 1.
Spends 19.0768137300 secs in 3536 call(s) to white_alpha_beta, invocation 2.
Spends 0.0000018200 secs in 3 call(s) to poll_queue, invocation 1.
Spends 0.0000235400 secs in 34 call(s) to publish_workload, invocation 1.
Spends 0.0001892040 secs in 313 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0001659560 secs in 272 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000191260 secs in 33 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000203720 secs in 34 call(s) to unpublish_workload, invocation 1.
Is called 2270 time(s) from white_alpha_beta, invocation 1.

## Summary for block black_alpha_beta, invocation 4.
Spends 18.6303767610 secs in 86633 call(s), or 97.51% of total execution time.
Spends 0.2370949870 secs (1.24%) in own code, 18.3932817740 secs (96.27%) in children.

Spends 0.0011116190 secs in 1328 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0325617740 secs in 48264 call(s) to gen_black_moves, invocation 1.
Spends 0.0473510080 secs in 84123 call(s) to do_black_move, invocation 1.
Spends 0.0467182440 secs in 84123 call(s) to undo_black_move, invocation 1.
Spends 0.0001542240 secs in 168 call(s) to return_my_timer, invocation 1.
Spends 18.2587785140 secs in 84173 call(s) to white_alpha_beta, invocation 4.
Spends 0.0000525880 secs in 84 call(s) to poll_queue, invocation 1.
Spends 0.0004489920 secs in 613 call(s) to publish_workload, invocation 1.
Spends 0.0029513220 secs in 4971 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0024114170 secs in 4163 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0003819690 secs in 613 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0003601030 secs in 613 call(s) to unpublish_workload, invocation 1.
Is called 86633 time(s) from white_alpha_beta, invocation 3.

## Summary for block white_alpha_beta, invocation 4.
Spends 18.2587785140 secs in 84173 call(s), or 95.56% of total execution time.
Spends 0.5635955110 secs (2.95%) in own code, 17.6951830030 secs (92.61%) in children.

Spends 0.0445030140 secs in 66028 call(s) to gen_white_moves, invocation 1.
Spends 0.1516343620 secs in 273690 call(s) to do_white_move, invocation 1.
Spends 0.1503363090 secs in 273690 call(s) to undo_white_move, invocation 1.
Spends 0.0020994480 secs in 2532 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001311970 secs in 144 call(s) to return_my_timer, invocation 1.
Spends 17.3346482160 secs in 273732 call(s) to black_alpha_beta, invocation 5.
Spends 0.0000446810 secs in 72 call(s) to poll_queue, invocation 1.
Spends 0.0007546450 secs in 1093 call(s) to publish_workload, invocation 1.
Spends 0.0053012510 secs in 9325 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0044124560 secs in 7779 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0006287710 secs in 1093 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0006886530 secs in 1093 call(s) to unpublish_workload, invocation 1.
Is called 84173 time(s) from black_alpha_beta, invocation 4.

## Summary for block black_alpha_beta, invocation 3.
Spends 19.0171929860 secs in 16214 call(s), or 99.53% of total execution time.
Spends 0.0561222770 secs (0.29%) in own code, 18.9610707090 secs (99.24%) in children.

Spends 0.0003023730 secs in 337 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0083302980 secs in 12338 call(s) to gen_black_moves, invocation 1.
Spends 0.0116683090 secs in 20326 call(s) to do_black_move, invocation 1.
Spends 0.0115784390 secs in 20326 call(s) to undo_black_move, invocation 1.
Spends 0.0000271190 secs in 30 call(s) to return_my_timer, invocation 1.
Spends 18.9270871920 secs in 20354 call(s) to white_alpha_beta, invocation 3.
Spends 0.0000092150 secs in 15 call(s) to poll_queue, invocation 1.
Spends 0.0001390310 secs in 198 call(s) to publish_workload, invocation 1.
Spends 0.0008977160 secs in 1600 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0007865050 secs in 1380 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0001274910 secs in 198 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0001170210 secs in 198 call(s) to unpublish_workload, invocation 1.
Is called 16214 time(s) from white_alpha_beta, invocation 2.

## Summary for block white_alpha_beta, invocation 2.
Spends 19.0768137300 secs in 3536 call(s), or 99.84% of total execution time.
Spends 0.0353606640 secs (0.19%) in own code, 19.0414530660 secs (99.66%) in children.

Spends 0.0023608750 secs in 3286 call(s) to gen_white_moves, invocation 1.
Spends 0.0093439920 secs in 16196 call(s) to do_white_move, invocation 1.
Spends 0.0092461080 secs in 16196 call(s) to undo_white_move, invocation 1.
Spends 0.0001300510 secs in 163 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000055010 secs in 6 call(s) to return_my_timer, invocation 1.
Spends 19.0171929860 secs in 16214 call(s) to black_alpha_beta, invocation 3.
Spends 0.0000019620 secs in 3 call(s) to poll_queue, invocation 1.
Spends 0.0001883630 secs in 274 call(s) to publish_workload, invocation 1.
Spends 0.0014538340 secs in 2506 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0012007780 secs in 2090 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0001683720 secs in 274 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0001602440 secs in 274 call(s) to unpublish_workload, invocation 1.
Is called 3536 time(s) from black_alpha_beta, invocation 2.

## Summary for block white_alpha_beta, invocation 6.
Spends 13.3958574840 secs in 388605 call(s), or 70.11% of total execution time.
Spends 1.3004780300 secs (6.81%) in own code, 12.0953794540 secs (63.31%) in children.

Spends 0.1661617700 secs in 246846 call(s) to gen_white_moves, invocation 1.
Spends 0.2872306910 secs in 518590 call(s) to do_white_move, invocation 1.
Spends 0.2842352470 secs in 518590 call(s) to undo_white_move, invocation 1.
Spends 0.0237872220 secs in 29408 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0007122370 secs in 764 call(s) to return_my_timer, invocation 1.
Spends 0.0002368350 secs in 382 call(s) to poll_queue, invocation 1.
Spends 0.0003720380 secs in 469 call(s) to publish_workload, invocation 1.
Spends 0.0015787520 secs in 2648 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0013731110 secs in 2300 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0002638940 secs in 469 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0002715430 secs in 469 call(s) to unpublish_workload, invocation 1.
Spends 11.3291561140 secs in 518763 call(s) to black_alpha_beta, invocation 7.
Is called 388605 time(s) from black_alpha_beta, invocation 6.

## Summary for block black_alpha_beta, invocation 7.
Spends 11.3291561140 secs in 518763 call(s), or 59.29% of total execution time.
Spends 1.3173903570 secs (6.89%) in own code, 10.0117657570 secs (52.40%) in children.

Spends 0.0312151150 secs in 39094 call(s) to return_pattern_score_double, invocation 1.
Spends 0.2131273150 secs in 326346 call(s) to gen_black_moves, invocation 1.
Spends 0.2452988270 secs in 441010 call(s) to do_black_move, invocation 1.
Spends 0.2430892690 secs in 441010 call(s) to undo_black_move, invocation 1.
Spends 0.0009864510 secs in 1018 call(s) to return_my_timer, invocation 1.
Spends 0.0003125140 secs in 509 call(s) to poll_queue, invocation 1.
Spends 0.0003782900 secs in 528 call(s) to publish_workload, invocation 1.
Spends 0.0023187900 secs in 3963 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0018671710 secs in 3255 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0002942170 secs in 528 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0003034300 secs in 528 call(s) to unpublish_workload, invocation 1.
Spends 9.2725743680 secs in 441222 call(s) to white_alpha_beta, invocation 7.
Is called 518763 time(s) from white_alpha_beta, invocation 6.

## Summary for block white_alpha_beta, invocation 7.
Spends 9.2725743680 secs in 441222 call(s), or 48.53% of total execution time.
Spends 1.2424828620 secs (6.50%) in own code, 8.0300915060 secs (42.03%) in children.

Spends 0.2199827080 secs in 329095 call(s) to gen_white_moves, invocation 1.
Spends 0.2335794710 secs in 420310 call(s) to do_white_move, invocation 1.
Spends 0.2317002480 secs in 420310 call(s) to undo_white_move, invocation 1.
Spends 0.0386130220 secs in 48740 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0008611400 secs in 916 call(s) to return_my_timer, invocation 1.
Spends 0.0002809400 secs in 458 call(s) to poll_queue, invocation 1.
Spends 0.0002371930 secs in 343 call(s) to publish_workload, invocation 1.
Spends 0.0012125850 secs in 2031 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0009969740 secs in 1743 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0002031940 secs in 342 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0001961690 secs in 342 call(s) to unpublish_workload, invocation 1.
Spends 7.3022278620 secs in 420540 call(s) to black_alpha_beta, invocation 8.
Is called 441222 time(s) from black_alpha_beta, invocation 7.

## Summary for block black_alpha_beta, invocation 8.
Spends 7.3022278620 secs in 420540 call(s), or 38.22% of total execution time.
Spends 1.1942906240 secs (6.25%) in own code, 6.1079372380 secs (31.97%) in children.

Spends 0.0413461320 secs in 52479 call(s) to return_pattern_score_double, invocation 1.
Spends 0.2360738450 secs in 363642 call(s) to gen_black_moves, invocation 1.
Spends 0.2135231060 secs in 383812 call(s) to do_black_move, invocation 1.
Spends 0.2111661510 secs in 383812 call(s) to undo_black_move, invocation 1.
Spends 0.0008048210 secs in 846 call(s) to return_my_timer, invocation 1.
Spends 0.0002717990 secs in 423 call(s) to poll_queue, invocation 1.
Spends 0.0002414210 secs in 350 call(s) to publish_workload, invocation 1.
Spends 0.0013614280 secs in 2468 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0012064580 secs in 2104 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0001962880 secs in 350 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0002003280 secs in 350 call(s) to unpublish_workload, invocation 1.
Spends 5.4015454610 secs in 384013 call(s) to white_alpha_beta, invocation 8.
Is called 420540 time(s) from white_alpha_beta, invocation 7.

## Summary for block white_alpha_beta, invocation 8.
Spends 5.4015454610 secs in 384013 call(s), or 28.27% of total execution time.
Spends 0.9426774200 secs (4.93%) in own code, 4.4588680410 secs (23.34%) in children.

Spends 0.2137724260 secs in 323782 call(s) to gen_white_moves, invocation 1.
Spends 0.1452192620 secs in 260213 call(s) to do_white_move, invocation 1.
Spends 0.1428857540 secs in 260213 call(s) to undo_white_move, invocation 1.
Spends 0.0439059660 secs in 55581 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0007082940 secs in 758 call(s) to return_my_timer, invocation 1.
Spends 0.0002310400 secs in 379 call(s) to poll_queue, invocation 1.
Spends 0.0001314640 secs in 182 call(s) to publish_workload, invocation 1.
Spends 0.0006779710 secs in 1179 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0005611960 secs in 997 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0001022810 secs in 182 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0001172450 secs in 182 call(s) to unpublish_workload, invocation 1.
Spends 3.9105551420 secs in 260470 call(s) to black_alpha_beta, invocation 9.
Is called 384013 time(s) from black_alpha_beta, invocation 8.

## Summary for block black_alpha_beta, invocation 9.
Spends 3.9105551420 secs in 260470 call(s), or 20.47% of total execution time.
Spends 0.7192048980 secs (3.76%) in own code, 3.1913502440 secs (16.70%) in children.

Spends 0.0259095310 secs in 33064 call(s) to return_pattern_score_double, invocation 1.
Spends 0.1486819460 secs in 231762 call(s) to gen_black_moves, invocation 1.
Spends 0.1237281280 secs in 221377 call(s) to do_black_move, invocation 1.
Spends 0.1217363230 secs in 221377 call(s) to undo_black_move, invocation 1.
Spends 0.0004741740 secs in 506 call(s) to return_my_timer, invocation 1.
Spends 0.0001530990 secs in 253 call(s) to poll_queue, invocation 1.
Spends 0.0000970200 secs in 137 call(s) to publish_workload, invocation 1.
Spends 0.0006235590 secs in 1080 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0005143900 secs in 910 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000948830 secs in 137 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000795830 secs in 137 call(s) to unpublish_workload, invocation 1.
Spends 2.7692576080 secs in 221589 call(s) to white_alpha_beta, invocation 9.
Is called 260470 time(s) from white_alpha_beta, invocation 8.

## Summary for block white_alpha_beta, invocation 9.
Spends 2.7692576080 secs in 221589 call(s), or 14.49% of total execution time.
Spends 0.5532378630 secs (2.90%) in own code, 2.2160197450 secs (11.60%) in children.

Spends 0.1286414170 secs in 195647 call(s) to gen_white_moves, invocation 1.
Spends 0.0840037440 secs in 152144 call(s) to do_white_move, invocation 1.
Spends 0.0833621500 secs in 152144 call(s) to undo_white_move, invocation 1.
Spends 0.0260204710 secs in 33002 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0004209510 secs in 446 call(s) to return_my_timer, invocation 1.
Spends 0.0001355070 secs in 223 call(s) to poll_queue, invocation 1.
Spends 0.0000322310 secs in 45 call(s) to publish_workload, invocation 1.
Spends 0.0002022670 secs in 366 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0001560330 secs in 290 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000254430 secs in 45 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000258800 secs in 45 call(s) to unpublish_workload, invocation 1.
Spends 1.8929936510 secs in 152321 call(s) to black_alpha_beta, invocation 10.
Is called 221589 time(s) from black_alpha_beta, invocation 9.

## Summary for block black_alpha_beta, invocation 10.
Spends 1.8929936510 secs in 152321 call(s), or 9.91% of total execution time.
Spends 0.4158395500 secs (2.18%) in own code, 1.4771541010 secs (7.73%) in children.

Spends 0.0165080320 secs in 21144 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0853935760 secs in 135027 call(s) to gen_black_moves, invocation 1.
Spends 0.0705672180 secs in 128058 call(s) to do_black_move, invocation 1.
Spends 0.0700209060 secs in 128058 call(s) to undo_black_move, invocation 1.
Spends 0.0002814590 secs in 298 call(s) to return_my_timer, invocation 1.
Spends 0.0001075730 secs in 149 call(s) to poll_queue, invocation 1.
Spends 0.0000101940 secs in 15 call(s) to publish_workload, invocation 1.
Spends 0.0000647370 secs in 118 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0000483410 secs in 90 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000084980 secs in 15 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000085820 secs in 15 call(s) to unpublish_workload, invocation 1.
Spends 1.2341349850 secs in 128247 call(s) to white_alpha_beta, invocation 10.
Is called 152321 time(s) from white_alpha_beta, invocation 9.

## Summary for block white_alpha_beta, invocation 10.
Spends 1.2341349850 secs in 128247 call(s), or 6.46% of total execution time.
Spends 0.3138144130 secs (1.64%) in own code, 0.9203205720 secs (4.82%) in children.

Spends 0.0767879480 secs in 117154 call(s) to gen_white_moves, invocation 1.
Spends 0.0461941630 secs in 82944 call(s) to do_white_move, invocation 1.
Spends 0.0456051830 secs in 82944 call(s) to undo_white_move, invocation 1.
Spends 0.0161607210 secs in 20962 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002562090 secs in 270 call(s) to return_my_timer, invocation 1.
Spends 0.0000804180 secs in 135 call(s) to poll_queue, invocation 1.
Spends 0.0000013450 secs in 2 call(s) to publish_workload, invocation 1.
Spends 0.0000088270 secs in 16 call(s) to choose_workload_next_move, invocation 1.
Spends 0.0000063990 secs in 12 call(s) to update_workloadmvbstmvscrpvdpth, invocation 1.
Spends 0.0000011500 secs in 2 call(s) to return_workload_bestmvscrpvdpth, invocation 1.
Spends 0.0000011980 secs in 2 call(s) to unpublish_workload, invocation 1.
Spends 0.7352170110 secs in 83067 call(s) to black_alpha_beta, invocation 11.
Is called 128247 time(s) from black_alpha_beta, invocation 10.

## Summary for block black_alpha_beta, invocation 11.
Spends 0.7352170110 secs in 83067 call(s), or 3.85% of total execution time.
Spends 0.2106623690 secs (1.10%) in own code, 0.5245546420 secs (2.75%) in children.

Spends 0.0088778520 secs in 11356 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0467027140 secs in 74001 call(s) to gen_black_moves, invocation 1.
Spends 0.0333589610 secs in 61020 call(s) to do_black_move, invocation 1.
Spends 0.0331784700 secs in 61020 call(s) to undo_black_move, invocation 1.
Spends 0.0001576080 secs in 176 call(s) to return_my_timer, invocation 1.
Spends 0.0000527540 secs in 88 call(s) to poll_queue, invocation 1.
Spends 0.4022262830 secs in 61062 call(s) to white_alpha_beta, invocation 11.
Is called 83067 time(s) from white_alpha_beta, invocation 10.

## Summary for block white_alpha_beta, invocation 11.
Spends 0.4022262830 secs in 61062 call(s), or 2.11% of total execution time.
Spends 0.1399298350 secs (0.73%) in own code, 0.2622964480 secs (1.37%) in children.

Spends 0.0376033770 secs in 57272 call(s) to gen_white_moves, invocation 1.
Spends 0.0186161520 secs in 33448 call(s) to do_white_move, invocation 1.
Spends 0.0181539050 secs in 33448 call(s) to undo_white_move, invocation 1.
Spends 0.0079724470 secs in 10469 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001154100 secs in 130 call(s) to return_my_timer, invocation 1.
Spends 0.0000381340 secs in 65 call(s) to poll_queue, invocation 1.
Spends 0.1797970230 secs in 33478 call(s) to black_alpha_beta, invocation 12.
Is called 61062 time(s) from black_alpha_beta, invocation 11.

## Summary for block black_alpha_beta, invocation 12.
Spends 0.1797970230 secs in 33478 call(s), or 0.94% of total execution time.
Spends 0.0704291190 secs (0.37%) in own code, 0.1093679040 secs (0.57%) in children.

Spends 0.0031057730 secs in 4013 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0187457850 secs in 30037 call(s) to gen_black_moves, invocation 1.
Spends 0.0086768910 secs in 15915 call(s) to do_black_move, invocation 1.
Spends 0.0087164350 secs in 15915 call(s) to undo_black_move, invocation 1.
Spends 0.0000674490 secs in 76 call(s) to return_my_timer, invocation 1.
Spends 0.0000228420 secs in 38 call(s) to poll_queue, invocation 1.
Spends 0.0700327290 secs in 15918 call(s) to white_alpha_beta, invocation 12.
Is called 33478 time(s) from white_alpha_beta, invocation 11.

## Summary for block white_alpha_beta, invocation 12.
Spends 0.0700327290 secs in 15918 call(s), or 0.37% of total execution time.
Spends 0.0304279090 secs (0.16%) in own code, 0.0396048200 secs (0.21%) in children.

Spends 0.0095698270 secs in 14707 call(s) to gen_white_moves, invocation 1.
Spends 0.0030115520 secs in 5474 call(s) to do_white_move, invocation 1.
Spends 0.0029588860 secs in 5474 call(s) to undo_white_move, invocation 1.
Spends 0.0015085790 secs in 1859 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000447400 secs in 30 call(s) to return_my_timer, invocation 1.
Spends 0.0000090270 secs in 15 call(s) to poll_queue, invocation 1.
Spends 0.0225022090 secs in 5476 call(s) to black_alpha_beta, invocation 13.
Is called 15918 time(s) from black_alpha_beta, invocation 12.

## Summary for block black_alpha_beta, invocation 13.
Spends 0.0225022090 secs in 5476 call(s), or 0.12% of total execution time.
Spends 0.0097221930 secs (0.05%) in own code, 0.0127800160 secs (0.07%) in children.

Spends 0.0002024100 secs in 271 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0024196540 secs in 3871 call(s) to gen_black_moves, invocation 1.
Spends 0.0011723690 secs in 2121 call(s) to do_black_move, invocation 1.
Spends 0.0011423110 secs in 2121 call(s) to undo_black_move, invocation 1.
Spends 0.0000035090 secs in 4 call(s) to return_my_timer, invocation 1.
Spends 0.0000012170 secs in 2 call(s) to poll_queue, invocation 1.
Spends 0.0078385460 secs in 2121 call(s) to white_alpha_beta, invocation 13.
Is called 5476 time(s) from white_alpha_beta, invocation 12.

## Summary for block white_alpha_beta, invocation 13.
Spends 0.0078385460 secs in 2121 call(s), or 0.04% of total execution time.
Spends 0.0037117540 secs (0.02%) in own code, 0.0041267920 secs (0.02%) in children.

Spends 0.0010769810 secs in 1676 call(s) to gen_white_moves, invocation 1.
Spends 0.0003833340 secs in 722 call(s) to do_white_move, invocation 1.
Spends 0.0003910080 secs in 722 call(s) to undo_white_move, invocation 1.
Spends 0.0000597270 secs in 79 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0000054970 secs in 6 call(s) to return_my_timer, invocation 1.
Spends 0.0000017760 secs in 3 call(s) to poll_queue, invocation 1.
Spends 0.0022084690 secs in 722 call(s) to black_alpha_beta, invocation 14.
Is called 2121 time(s) from black_alpha_beta, invocation 13.

## Summary for block black_alpha_beta, invocation 14.
Spends 0.0022084690 secs in 722 call(s), or 0.01% of total execution time.
Spends 0.0010314690 secs (0.01%) in own code, 0.0011770000 secs (0.01%) in children.

Spends 0.0000110800 secs in 15 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0002118090 secs in 322 call(s) to gen_black_moves, invocation 1.
Spends 0.0001502060 secs in 213 call(s) to do_black_move, invocation 1.
Spends 0.0001114110 secs in 213 call(s) to undo_black_move, invocation 1.
Spends 0.0006924940 secs in 213 call(s) to white_alpha_beta, invocation 14.
Is called 722 time(s) from white_alpha_beta, invocation 13.

## Summary for block white_alpha_beta, invocation 14.
Spends 0.0006924940 secs in 213 call(s), or 0.00% of total execution time.
Spends 0.0003344890 secs (0.00%) in own code, 0.0003580050 secs (0.00%) in children.

Spends 0.0001008730 secs in 153 call(s) to gen_white_moves, invocation 1.
Spends 0.0000264750 secs in 50 call(s) to do_white_move, invocation 1.
Spends 0.0000263000 secs in 50 call(s) to undo_white_move, invocation 1.
Spends 0.0000083960 secs in 11 call(s) to return_pattern_score_double, invocation 1.
Spends 0.0001959610 secs in 50 call(s) to black_alpha_beta, invocation 15.
Is called 213 time(s) from black_alpha_beta, invocation 14.

## Summary for block black_alpha_beta, invocation 15.
Spends 0.0001959610 secs in 50 call(s), or 0.00% of total execution time.
Spends 0.0000463670 secs (0.00%) in own code, 0.0001495940 secs (0.00%) in children.

Spends 0.0000085590 secs in 13 call(s) to gen_black_moves, invocation 1.
Spends 0.0000027010 secs in 5 call(s) to do_black_move, invocation 1.
Spends 0.0000027230 secs in 5 call(s) to undo_black_move, invocation 1.
Spends 0.0001356110 secs in 5 call(s) to white_alpha_beta, invocation 15.
Is called 50 time(s) from white_alpha_beta, invocation 14.

## Summary for block white_alpha_beta, invocation 15.
Spends 0.0001356110 secs in 5 call(s), or 0.00% of total execution time.
Spends 0.0000118400 secs (0.00%) in own code, 0.0001237710 secs (0.00%) in children.

Spends 0.0000027390 secs in 3 call(s) to gen_white_moves, invocation 1.
Spends 0.0000016490 secs in 3 call(s) to do_white_move, invocation 1.
Spends 0.0000016990 secs in 3 call(s) to undo_white_move, invocation 1.
Spends 0.0001176840 secs in 3 call(s) to black_alpha_beta, invocation 16.
Is called 5 time(s) from black_alpha_beta, invocation 15.

## Summary for block black_alpha_beta, invocation 16.
Spends 0.0001176840 secs in 3 call(s), or 0.00% of total execution time.
Spends 0.0000149820 secs (0.00%) in own code, 0.0001027020 secs (0.00%) in children.

Spends 0.0000027840 secs in 3 call(s) to gen_black_moves, invocation 1.
Spends 0.0000027200 secs in 5 call(s) to do_black_move, invocation 1.
Spends 0.0000027660 secs in 5 call(s) to undo_black_move, invocation 1.
Spends 0.0000944320 secs in 5 call(s) to white_alpha_beta, invocation 16.
Is called 3 time(s) from white_alpha_beta, invocation 15.

## Summary for block white_alpha_beta, invocation 16.
Spends 0.0000944320 secs in 5 call(s), or 0.00% of total execution time.
Spends 0.0000293150 secs (0.00%) in own code, 0.0000651170 secs (0.00%) in children.

Spends 0.0000026310 secs in 3 call(s) to gen_white_moves, invocation 1.
Spends 0.0000017140 secs in 3 call(s) to do_white_move, invocation 1.
Spends 0.0000016690 secs in 3 call(s) to undo_white_move, invocation 1.
Spends 0.0000591030 secs in 3 call(s) to black_alpha_beta, invocation 17.
Is called 5 time(s) from black_alpha_beta, invocation 16.

## Summary for block black_alpha_beta, invocation 17.
Spends 0.0000591030 secs in 3 call(s), or 0.00% of total execution time.
Spends 0.0000221030 secs (0.00%) in own code, 0.0000370000 secs (0.00%) in children.

Spends 0.0000060040 secs in 3 call(s) to gen_black_moves, invocation 1.
Spends 0.0000033530 secs in 6 call(s) to do_black_move, invocation 1.
Spends 0.0000033540 secs in 6 call(s) to undo_black_move, invocation 1.
Spends 0.0000242890 secs in 6 call(s) to white_alpha_beta, invocation 17.
Is called 3 time(s) from white_alpha_beta, invocation 16.

## Summary for block white_alpha_beta, invocation 17.
Spends 0.0000242890 secs in 6 call(s), or 0.00% of total execution time.
Spends 0.0000165130 secs (0.00%) in own code, 0.0000077760 secs (0.00%) in children.

Spends 0.0000046280 secs in 6 call(s) to gen_white_moves, invocation 1.
Spends 0.0000031480 secs in 4 call(s) to return_pattern_score_double, invocation 1.
Is called 6 time(s) from black_alpha_beta, invocation 17.

## End of profile.
```
