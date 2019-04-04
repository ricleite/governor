#ifndef __GOVERNOR_IMPL_H__
#define __GOVERNOR_IMPL_H__

#ifndef _GNU_SOURCE // suppress warning
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <sched.h>

#include <cstdio>

#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <fstream>
#include <random>

enum RunMode
{
    // use random scheduling
    RUN_RANDOM      = 0,
    // use ordered scheduling, data is kept in file in execution dir (.gov)
    RUN_EXPLORE     = 1,
    // assume a valid schedule is saved in .gov, and run it (once)
    RUN_PRESET      = 2,
};

// contains info stored at each scheduling point
struct SchedPoint
{
public:
    size_t threadId; // thread id to be run
    size_t available; // number of threads that were available to be run
    size_t higher; // number of threads with threadId higher than `threadId`

public:
    bool read(std::fstream& fs);
    bool write(std::fstream& fs);
};

struct ThreadState
{
    size_t const threadId; // user-provided thread id
    bool isInControlPoint = false;

    ThreadState(size_t t) : threadId(t) { }
};

class Governor
{
public:
    // C++ user API
    // reset governor so that the next schedule sequence can be attempted
    // this is only useful if RUN_EXPLORE is used
    // Reset() returns false if there are no more schedule sequences to
    //  explore, and true otherwise
    bool Reset(bool force = false);
    // prepare governor to begin scheduling a specified number of threads
    // after calling Prepare(N), N distinct threads must call Subscribe()
    //  before scheduling at a control point can occur
    void Prepare(size_t numThreads);
    // subscribe a thread for scheduling
    // after subscribing, and until it unsubs, the thread must *NEVER*
    //  depend on the progress of another (i.e. use locks, joins())
    // all operations that read/write to shared state must call ControlPoint()
    //  before performing the read/write
    void Subscribe(size_t threadId);
    // unsubscribe from scheduling
    void Unsubscribe();
    // give control to governor, only has effect after thread is subscribed
    void ControlPoint();

public:
    // hooks called on thread enter/exit
    void HandleThreadInit();
    void HandleThreadFinalize();

    static Governor* instance()
    {
        static Governor instance;
        return &instance;
    }

private:
    Governor();
    ~Governor();

    ThreadState* GetThreadState() const;

    // update affinity for calling thread
    void SetAffinity(bool apply);
    // determine a new running thread
    // returns true if a new thread was chosen
    bool UpdateActiveThread();
    std::thread::id ChooseThread(RunMode mode);
    // opens or refreshes file handles
    // if close = true, closes all handles
    void HandleOutFile(bool close);

private:
    // mutex that must be held when modifying shared data
    std::mutex _mutex;
    // scheduling mode used
    RunMode _runMode = RUN_RANDOM;
    // file that stores sequence for scheduling
    // depending on run mode, this file is either read or written to
    std::fstream _file;
    // sequence used for scheduling, if not-empty
    std::vector<SchedPoint> _sched;
    size_t _schedIdx = 0;
    bool _schedDone = false;

    size_t _threadsToSub = 0u;
    // maintains state of threads and whether they're on a control point
    std::unordered_map<std::thread::id, ThreadState*> _threads;
    std::map<size_t /*threadId*/, std::thread::id> _threadIds;
    // currently executing thread
    std::atomic<std::thread::id> _activeThreadId;

    // cpu affinity masks
    cpu_set_t* _defaultCpuSet = nullptr; // default affinity mask
    cpu_set_t* _cpuSet = nullptr; // mask with only one random CPU
    // random generator, used in RUN_RANDOM
    std::minstd_rand _rng;
};

#define sGovernor Governor::instance()

#endif // __GOVERNOR_IMPL_H__

