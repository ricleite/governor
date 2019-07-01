## Introduction

Governor is a tool to debug lock-free algorithms written in C/C++.

It allows the deterministic execution of programs by controlling thread
scheduling, with just a few simple hooks in the program's source code.

Given a program that implements/tests a lock-free algorithm, Governor can be
used to:
* Run it with a randomly generated scheduling sequence
* Run it with a previously generated scheduling sequence

    This is especially useful to debug, as we can re-run a scheduling
sequence that triggered a bug, and easily replicate the problem in
`gdb` or equivalent tool

* Exhaustively test the program by exploring all scheduling sequences

## Building

To compile, just download this repository and run
```console
make
```

This will generate a `libgovernor.a`, which is the static library file you need
to link with at compilation time in order to use Governor.


## Usage

To employ the Governor, you must integrate it with your project. Follow these
steps:

* `#include governor.h` in order to use the library API 
* Annotate all atomic operations with `GOV_CONTROL()`
* Call `GOV_SUBSCRIBE(threadId)` from threads in which you want Governor to
  control scheduling.

   `threadId` is an integer that that must uniquely identify the calling thread

* Call `GOV_PREPARE(numThreads)` before launching any threads that will
  subscribe

   `numThreads` is an integer that informs Governor how many threads will later
call `GOV_SUBSCRIBE`

* Link with `libgovernor.a`at compilation time

Note that in order for Governor to function correctly, you must ensure that:

* Threads that subscribe (by calling `GOV_SUBSCRIBE()`) must run lock-free
  code from that moment on, until they terminate or unsubscribe (by
calling `GOV_UNSUBSCRIBE()`)
* Threads that subscribe **MUST NOT** call any blocking constructs (locks,
  mutexes, `pthread_join()`, ...)
* In order for scheduling to be completely deterministic, you must ensure
  there's no other source of non-determinism (i.e., rngs with random seeds)

While using the Governor, the execution of your program is going to use a
file named `gov.data` which will store the scheduling sequence. The
behavior of the Governor and what will be written to `gov.data` depends on the
*run mode*. The run mode can be set by changing the `GOV_MODE` environment
variable.

```console
GOV_MODE=something ./your_program
```

The following run modes are available:

* `RUN_RANDOM`. A random schedule sequence will be generated on-the-fly
  and saved in `gov.data`
* `RUN_EXPLORE`. Each program run will read `gov.data` to generate the next
  schedule sequence using a strict ordering. If `gov.data` does not exist,
an initial schedule is generated. If the next schedule cannot be generated
(because the very last sequence was reached), the program will immediately
exit
* `RUN_PRESET`. Run using the existing schedule sequence in `gov.data`. If
  `gov.data` does not exist, or is incoherent/incomplete, an occur will occur
during runtime

If unspecified, the run mode is `RUN_PRESET`.

You can use abbreviations for the run modes. `RUN_RANDOM` can be used as
`RANDOM` or just `RAND`, `RUN_EXPLORE` as `EXPLORE` or just `EXP`, and
`RUN_PRESET` as `PRESET` or just `PRE`.

## Details

Governor uses the observation that the outcome of a lock-free algorithm depends
on the order of atomic operations, and that if we assume that those atomic
operations are sequentially consistent, the order can be manipulated by
controlling the scheduling of threads.

Furthermore, by definition, in a lock-free algorithm, if enough execution
time is given to any particular thread, progress will be made by the algorithm.
This implies that:

* Threads do not block to wait on the progress of some other thread. Therefore a tool such as Governor can yield control to some thread and expect it not to block
* The algorithm will finish after a *finite* number of steps is made, and thus all scheduling sequences are finite. Thus a tool like Governor will not induce a lock-free algorithm into an infinite looping execution unless it is incorrect


## Copyright

License: GPL 3.0

Read file [COPYING](COPYING)

## TODOs

This project is a work-in-progress. TODOs:

* Add a few examples of Governor integration (an examples/ dir)
* Decrease runtime cost of control points

