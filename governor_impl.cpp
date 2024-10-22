/*
 * Copyright (C) 2019 Ricardo Leite
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>

#include <vector>
#include <algorithm>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "governor.h"
#include "governor_hooks.h"
#include "governor_impl.h"

#define PAGE (1 << 12)

#define GOV_ERR(str, ...) \
    fprintf(stderr, "%s:%d %s " str "\n", __FILE__, \
            __LINE__, __func__, ##__VA_ARGS__);

// file where scheduling data is kept
constexpr const char* GOV_FILE = "gov.data";

size_t SchedPoint::read(char* buffer)
{
    int nchars = 0; // number of chars read
    int ret = std::sscanf(buffer, "%lu %lu %lu\n%n",
        &threadId, &available, &higher, &nchars);

    if (ret <= 0 || nchars <= 0)
        return 0u;

    return nchars;
}

size_t SchedPoint::write(char* buffer, size_t size)
{
    int ret = std::snprintf(buffer, size, "%lu %lu %lu\n",
        threadId, available, higher);

    if (ret <= 0)
        return 0u;

    return ret;
}

Governor::Governor()
{
    std::unique_lock<std::mutex> lock(_mutex);

    // open schedule file
    _fileDesc = open(GOV_FILE, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (_fileDesc == -1)
    {
        GOV_ERR("failed to open or create %s", GOV_FILE);
        std::abort();
    }

    // get size of file, in multiples of page
    if (_fileDesc != -1)
    {
        struct stat st;
        fstat(_fileDesc, &st);
        _fileSize = (st.st_size / PAGE) * PAGE + ((st.st_size % PAGE) ? PAGE : 0);
        // init file with at least 1 page of storage
        if (_fileSize == 0)
            _fileSize = PAGE;

        // map file into memory
        MapFileToMem(_fileSize);
    }

    // initialize running thread
    // constructor "constructs an id that does not represent a thread"
    _activeThreadId = std::thread::id();
    // init rng
    std::random_device r;
    _rng = std::minstd_rand(r());

    // prepare run mode
    if (char* env = getenv("GOV_MODE"))
    {
        std::string s(env);

        // https://stackoverflow.com/questions/1878001/how-do-i-check-if-a-c-stdstring-starts-with-a-certain-string-and-convert-a
        // std::string::rfind(X) == 0 does the same as a "std::string::start_with"
        auto starts_with = [s](const char* str) -> bool {
            return s.rfind(str) == 0;
        };

        if (s == "RUN_RANDOM" || starts_with("RAND"))
            _runMode = RUN_RANDOM;
        else if (s == "RUN_EXPLORE" || starts_with("EXP"))
            _runMode = RUN_EXPLORE;
        else if (s == "RUN_PRESET" || starts_with("PRE"))
            _runMode = RUN_PRESET;
        else
        {
            GOV_ERR("invalid GOV_MODE variable %s", s.c_str());
            std::abort();
        }
    }

    lock.unlock();
    // read/open seq file
    Reset(true);
}

Governor::~Governor()
{
    std::lock_guard<std::mutex> lock(_mutex);

    // close seq file
    HandleOutFile(true);

    munmap(_filePtr, _fileSize);
    _filePtr = nullptr;
    close(_fileDesc);

    // free up allocated state memory
    for (auto p : _threads)
        delete p.second;

    CPU_FREE(_defaultCpuSet);
    CPU_FREE(_cpuSet);
}

bool Governor::Reset(bool force /*= false*/)
{
    std::lock_guard<std::mutex> lock(_mutex);

    // if no scheduling has been done, ignore call
    // this is needed in case the user calls Reset() several times
    if (!force && _schedIdx == 0)
        return true;

    if (_schedIdx > 0)
    {
        // close seq file
        HandleOutFile(true);
    }

    // then re-read and open
    HandleOutFile(false);

    switch (_runMode)
    {
        case RUN_RANDOM:
            // clear scheduling history
            _schedIdx = 0;
            _sched.clear();
            break;
        case RUN_EXPLORE:
            // prepare next scheduling sequence
            // next scheduling sequence uses same prefix, and uses
            //  a different (higher) threadId at last possible option
            // this is equivalent to a DFS search
            _schedIdx = 0;
            // if last execution wasn't complete, just repeat it
            if (!_schedDone)
                break;

            while (!_sched.empty())
            {
                SchedPoint& sp = _sched.back();
                if (sp.higher == 0)
                {
                    _sched.pop_back();
                    continue;
                }

                // use the next threadId
                // if it does not exist (i.e there's a gap), it will be corrected
                //  at schedule time
                sp.threadId += 1;
                sp.higher -= 1;
                break;
            }

            if (_sched.empty())
            {
                GOV_ERR("RUN_EXPLORE - reached last state");
                std::abort();
                return false;
            }

            break;
        case RUN_PRESET:
        {
            bool firstSched = (_schedIdx == 0);
            _schedIdx = 0; // be a good boy and reset _schedIdx
            // and return false if we've already used the available sequence
            return firstSched;
        }
        default:
            assert(false);
            break;
    }

    return true;
}

void Governor::Prepare(size_t numThreads)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _threadsToSub = numThreads;
}

void Governor::Subscribe(size_t threadId)
{
    std::lock_guard<std::mutex> lock(_mutex);

    // check if thread is already subbed
    if (GetThreadState())
    {
        GOV_ERR("thread %lu already subbed", threadId);
        std::abort();
        return;
    }
    // check if thread is supposed to be able to sub
    if (_threadsToSub == 0)
    {
        GOV_ERR("no more threads were expected to sub");
        std::abort();
        return;
    }
    // check if user provided an unused thread id
    for (auto p : _threads)
    {
        ThreadState* state = p.second;
        if (state->threadId == threadId)
        {
            GOV_ERR("threadId %lu provided is already used", threadId);
            std::abort();
            return;
        }
    }

    // update affinity, thread should only use a specific cpu
    //SetAffinity(true);

    // init thread state data
    std::thread::id id = std::this_thread::get_id();
    ThreadState* state = new ThreadState(threadId);

    _threads[id] = state;
    _threadIds[state->threadId] = id;
    // decrement expected number of subbed threads
    _threadsToSub--;

    assert(GetThreadState() == state);
    assert(_threads.size() == _threadIds.size());

    // call thread hook
    // ensures that thread calls Unsubscribe on thread exit
    sub_hook();
}

void Governor::Unsubscribe()
{
    std::unique_lock<std::mutex> lock(_mutex);

    if (GetThreadState() == nullptr)
        return;

    // update affinity, after unsub thread can use all cpus
    //SetAffinity(false);

    // remove thread state data
    std::thread::id id = std::this_thread::get_id();
    ThreadState* state = GetThreadState();

    _threads.erase(id);
    _threadIds.erase(state->threadId);
    delete state;

    assert(GetThreadState() == nullptr);
    assert(_threads.size() == _threadIds.size());

    // (possibly) update the currently running thread
    UpdateActiveThread();
}

void Governor::ControlPoint()
{
    std::unique_lock<std::mutex> lock(_mutex);

    ThreadState* state = GetThreadState();
    // ignore unsubscribed threads
    if (!state)
        return;

    // mark thread as being in a control point
    state->isInControlPoint = true;

    // and then (possibly) choose a new thread to execute
    UpdateActiveThread();

    // release lock so other threads can check the running thread
    lock.unlock();

    // wait until it's our turn to execute
    std::thread::id id = std::this_thread::get_id();
    while (_activeThreadId.load() != id)
        std::this_thread::yield();
}

ThreadState* Governor::GetThreadState() const
{
    std::thread::id id = std::this_thread::get_id();
    auto itr = _threads.find(id);
    if (itr != _threads.end())
        return itr->second;

    return nullptr;
}

bool Governor::UpdateActiveThread()
{
    std::thread::id id = std::this_thread::get_id();
    if (_activeThreadId.load() == id)
        _activeThreadId.store(std::thread::id());

    // not all threads have subscribed yet, so can't schedule
    //  a running thread yet
    if (_threadsToSub)
        return false;

    // check if we can choose a new thread to execute
    // to do so, all threads must be in a control point
    for (auto p : _threads)
    {
        ThreadState* state = p.second;
        if (state->isInControlPoint == false)
            return false; // found a thread still not on a CP
    }

    // no threads to choose from
    // this can happen when the last thread unsubs
    if (_threads.empty())
        return false;

    std::thread::id threadToRun = ChooseThread(_runMode);

    // launch choosen thread
    ThreadState* state = _threads[threadToRun];
    state->isInControlPoint = false;

    _activeThreadId.store(threadToRun);
    return true;
}

std::thread::id Governor::ChooseThread(RunMode mode)
{
    assert(!_threads.empty());
    assert(!_threadIds.empty());

    SchedPoint sp;
    // choose a random thread of the available ones
    if (mode == RUN_RANDOM)
    {
        std::vector<size_t> available;
        for (auto p : _threadIds)
            available.push_back(p.first);

        std::shuffle(available.begin(), available.end(), _rng);
        sp.threadId = available.front();
        sp.available = available.size();
        sp.higher = std::count_if(_threadIds.begin(), _threadIds.end(),
            [&sp](std::pair<size_t, std::thread::id> p)
        {
            return p.first > sp.threadId;
        });

        _sched.push_back(sp);
        _schedIdx = _sched.size() - 1;
    }
    else if (mode == RUN_EXPLORE)
    {
        size_t idx = _schedIdx++;
        assert(idx <= _sched.size());

        // if there's no info in schedule, use first available threadId
        if (idx == _sched.size())
        {
            auto itr = _threadIds.begin();
            sp.threadId = itr->first;
            sp.available = _threadIds.size();
            sp.higher = sp.available - 1;
            _sched.push_back(sp);
        }

        assert(idx < _sched.size());
        sp = _sched[idx];

        // last point in known schedule
        // this point was generated by adding 1 to last threadId,
        //  so the threadId might not exist, and we need to ensure
        //  that it does
        if (idx == _sched.size() - 1)
        {
            // use first threadId that is >= indicated threadId
            for (auto p : _threadIds)
            {
                size_t threadId = p.first;
                if (threadId >= sp.threadId)
                {
                    sp.threadId = threadId;
                    break;
                }
            }
        }
    }
    else if (mode == RUN_PRESET)
    {
        size_t idx = _schedIdx++;
        if (idx >= _sched.size())
        {
            GOV_ERR("RUN_PRESET - no scheduling available at idx %lu", idx);
            std::abort();
            return ChooseThread(RUN_RANDOM);
        }

        sp = _sched[idx];

        auto itr = _threadIds.find(sp.threadId);
        if (itr == _threadIds.end())
        {
            GOV_ERR("RUN_PRESET - threadId %lu is invalid at line %lu",
                sp.threadId, idx + 1);
            std::abort();
            return ChooseThread(RUN_RANDOM);
        }

        if (sp.available != _threadIds.size())
        {
            GOV_ERR("RUN_PRESET - wrong available value (%lu vs %lu) at "
                "line %lu", sp.available, _threadIds.size(), idx + 1);
            std::abort();
            return ChooseThread(RUN_RANDOM);
        }

        size_t higher = std::count_if(_threadIds.begin(), _threadIds.end(),
            [&sp](std::pair<size_t, std::thread::id> p)
        {
            return p.first > sp.threadId;
        });

        if (sp.higher != higher)
        {
            GOV_ERR("RUN_PRESET - wrong higher value (%lu vs %lu) at "
                "line %lu", sp.higher, higher, idx + 1);
            std::abort();
            return ChooseThread(RUN_RANDOM);
        }
    }

    auto itr = _threadIds.find(sp.threadId);
    assert(itr != _threadIds.end());
    std::thread::id id = itr->second;

    if (_filePtr && (_runMode == RUN_RANDOM || _runMode == RUN_EXPLORE))
    {
        // write sp to file
        while (true)
        {
            size_t len = sp.write(&_filePtr[_fileIdx], _fileSize - _fileIdx);
            if (_fileIdx + len < _fileSize)
            {
                _fileIdx += len;
                break;
            }

            // double size of file
            MapFileToMem(_fileSize * 2);
        }
    }

    return id;
}

void Governor::SetAffinity(bool apply)
{
    // ensure threads only run on a single CPU
    //  by configuring CPU afinity
    if (_defaultCpuSet == nullptr) // init affinities
    {
        assert(_cpuSet == nullptr);

        size_t numCPUs = sysconf(_SC_NPROCESSORS_ONLN);

        _defaultCpuSet = CPU_ALLOC(numCPUs);
        _cpuSet = CPU_ALLOC(numCPUs);
        if (_defaultCpuSet == nullptr || _cpuSet == nullptr)
            GOV_ERR("CPU_ALLOC failed");

        size_t size = CPU_ALLOC_SIZE(numCPUs);
        CPU_ZERO_S(size, _cpuSet);

        // fill all available CPUs for default mask
        for (size_t i = 0; i < numCPUs; ++i)
            CPU_SET_S(i, size, _defaultCpuSet);

        // choose random CPU for single cpu mask
        size_t randCPU = (rand() % numCPUs);
        CPU_SET_S(randCPU, size, _cpuSet);
    }

    cpu_set_t* setToUse = (apply ? _cpuSet : _defaultCpuSet);
    int ret = sched_setaffinity(0, sizeof(cpu_set_t), setToUse);
    if (ret == -1)
        GOV_ERR("SetAffinity failed");
}

void Governor::HandleOutFile(bool close)
{
    if (close)
    {
        if (_filePtr && (_runMode == RUN_RANDOM || _runMode == RUN_EXPLORE))
        {
            // write "END" to file
            while (true)
            {
                size_t len = snprintf(&_filePtr[_fileIdx], _fileSize - _fileIdx, "END\n");
                if (_fileIdx + len < _fileSize)
                {
                    _fileIdx += len;
                    break;
                }

                // double size of file
                MapFileToMem(_fileSize * 2);
            }
        }

        return;
    }

    // read last sequence, depending on mode
    if (_runMode == RUN_EXPLORE || _runMode == RUN_PRESET)
    {
        if (_filePtr == nullptr)
        {
            if (_runMode == RUN_PRESET)
            {
                GOV_ERR("mode is RUN_PRESET but can't read %s file", GOV_FILE);
                std::abort();
            }
        }
        else
        {
            // read sched
            _fileIdx = 0;
            _sched.clear();
            SchedPoint tmp;
            size_t ret;
            while ((ret = tmp.read(&_filePtr[_fileIdx])))
            {
                _sched.push_back(tmp);
                _fileIdx += ret;
            }

            // then check if schedule reached end of program
            int nchars = 0;
            std::sscanf(&_filePtr[_fileIdx], "END\n%n", &nchars);
            _schedDone = (nchars > 0);
        }
    }

    // then prepare file for writing
    // don't need to write schedule when in RUN_PRESET
    // it is already present
    if (_filePtr && (_runMode == RUN_RANDOM || _runMode == RUN_EXPLORE))
    {
        // reset file size
        MapFileToMem(PAGE); // to a single page
        // then clear its' contents
        std::memset(_filePtr, 0x0, _fileSize);
    }

    // start writing from the beginning of the file
    _fileIdx = 0;
}

void Governor::MapFileToMem(size_t size)
{
    if (_fileDesc == -1)
        return; // no file

    if (_filePtr)
    {
        munmap(_filePtr, _fileSize);
        _filePtr = nullptr;
    }

    _fileSize = size;
    // update file size
    ftruncate(_fileDesc, _fileSize);

    // re-map file into memory
    _filePtr = (char*)mmap(nullptr, _fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, _fileDesc, 0);
    // mmap should not fail, fd is valid
    assert(_filePtr && _filePtr != MAP_FAILED);
}
