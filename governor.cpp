
#include "governor.h"
#include "governor_impl.h"

extern "C"
void governor_prepare(size_t numThreads)
{
    sGovernor->Prepare(numThreads);
}

extern "C"
void governor_subscribe(size_t threadId)
{
    sGovernor->Subscribe(threadId);
}

extern "C"
void governor_control()
{
    sGovernor->ControlPoint();
}

extern "C"
int governor_reset()
{
    return sGovernor->Reset();
}
