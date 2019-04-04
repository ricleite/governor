
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cassert>

#include <vector>
#include <algorithm>

#include "governor.h"

#define GOV_ERR(str, ...) \
    fprintf(stderr, "%s:%d %s " str "\n", __FILE__, \
            __LINE__, __func__, ##__VA_ARGS__);

// file where scheduling data is kept
static const char* OUT_FILE = ".gov";

bool SchedPoint::read(std::fstream& fs)
{
    std::streampos pos = fs.tellg();
    size_t t, a, h;
    if (!(fs >> t && fs >> a && fs >> h))
    {
        fs.clear(); // see: https://stackoverflow.com/questions/16364301/whats-wrong-with-the-ifstream-seekg
        fs.seekg(pos); // restore pos before partially consuming input
        return false;
    }

    threadId = t;
    available = a;
    higher = h;
    return true;
}

bool SchedPoint::write(std::fstream& fs)
{
    if (!fs.is_open())
        return false;

    fs << threadId << ' '
       << available << ' '
       << higher << ' ';
    fs << std::endl;
    return true;
}

Governor::Governor()
{
    std::unique_lock<std::mutex> lock(_mutex);

    // initialize running thread
    // constructor "constructs an id that does not represent a thread"
    _activeThreadId = std::thread::id();
    // init rng
    std::random_device r;
    _rng = std::minstd_rand(r());

    if (char* env = getenv("GOV_MODE"))
    {
        std::string s(env);
        if (s == "RUN_RANDOM")
            _runMode = RUN_RANDOM;
        else if (s == "RUN_EXPLORE")
            _runMode = RUN_EXPLORE;
        else if (s == "RUN_PRESET")
            _runMode = RUN_PRESET;
        else
            GOV_ERR("invalid GOV_MODE variable %s", s.c_str());
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

    // close seq file
    HandleOutFile(true);
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
        return;
    }
    // check if thread is supposed to be able to sub
    if (_threadsToSub == 0)
    {
        GOV_ERR("no more threads were expected to sub");
        return;
    }
    // check if user provided an unused thread id
    for (auto p : _threads)
    {
        ThreadState* state = p.second;
        if (state->threadId == threadId)
        {
            GOV_ERR("threadId %lu provided is already used", threadId);
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
}

void Governor::Unsubscribe()
{
    std::unique_lock<std::mutex> lock(_mutex);

    if (GetThreadState() == nullptr)
    {
        GOV_ERR("thread is not subbed, cannot unsub");
        return;
    }

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

    // wait until it's our turn to execute
    std::thread::id id = std::this_thread::get_id();
    // release lock so other threads can check the running thread
    lock.unlock();

    while (_activeThreadId.load() != id)
        std::this_thread::yield();
}

void Governor::HandleThreadInit() { }

void Governor::HandleThreadFinalize()
{
    bool isSubbed = false;

    // check if thread is subscribed
    // hold a lock just to check, since Unsubscribe() will acquire it again
    {
        std::lock_guard<std::mutex> lock(_mutex);

        isSubbed = (GetThreadState() != nullptr);
    }

    if (isSubbed)
        Unsubscribe();
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
            GOV_ERR("mode is %d but no scheduling available at idx %lu",
                mode, idx);
            return ChooseThread(RUN_RANDOM);
        }

        sp = _sched[idx];

        auto itr = _threadIds.find(sp.threadId);
        if (itr == _threadIds.end())
        {
            GOV_ERR("RUN_PRESET - threadId %lu is invalid at line %lu",
                sp.threadId, idx + 1);
            return ChooseThread(RUN_RANDOM);
        }

        if (sp.available != _threadIds.size())
        {
            GOV_ERR("RUN_PRESET - wrong available value (%lu vs %lu) at "
                "line %lu", sp.available, _threadIds.size(), idx + 1);
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
            return ChooseThread(RUN_RANDOM);
        }
    }

    auto itr = _threadIds.find(sp.threadId);
    assert(itr != _threadIds.end());
    std::thread::id id = itr->second;

    if (_file.is_open())
        sp.write(_file);

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
        //printf("numCPUs: %lu\n", numCPUs);

        _defaultCpuSet = CPU_ALLOC(numCPUs);
        _cpuSet = CPU_ALLOC(numCPUs);
        if (_defaultCpuSet == nullptr || _cpuSet == nullptr)
            fprintf(stderr, "Governor::SetAffinity CPU_ALLOC failed\n");

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
        fprintf(stderr, "Governor::SetAffinity failed\n");
}

void Governor::HandleOutFile(bool close)
{
    if (close)
    {
        if (_file.is_open())
        {
            _file << "END" << std::endl;
            _file.close();
        }

        return;
    }

    // read last sequence, depending on mode
    if (_runMode == RUN_EXPLORE || _runMode == RUN_PRESET)
    {
        // open file for reading
        std::fstream fs(OUT_FILE, std::fstream::in);
        if (fs.is_open())
        {
            // read sched
            _sched.clear();
            SchedPoint tmp;
            while (tmp.read(fs))
                _sched.push_back(tmp);

            // then check if schedule reached end of program
            std::string str;
            fs >> str;
            _schedDone = (str == "END");
        }
        else
        {
            if (_runMode == RUN_PRESET)
                GOV_ERR("mode is RUN_PRESET but can't read %s file", OUT_FILE);
        }
    }

    // then prepare file for writing
    // don't need to write schedule when in RUN_PRESET
    // it is already present
    if (_runMode == RUN_RANDOM || _runMode == RUN_EXPLORE)
        _file.open(OUT_FILE, std::fstream::out | std::fstream::trunc);
}
