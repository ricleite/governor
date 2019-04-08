#include <pthread.h>

#include "governor_hooks.h"
#include "governor_impl.h"

// handle process init/exit hooks
// global variables
static pthread_key_t dummyKey; // used for thread exit hook
static bool isInit = false;

// call initializer() at process startup
__attribute__((constructor))
void initializer()
{
    if (isInit) // ensure initializer is only called once
        return;

    isInit = true;
    pthread_key_create(&dummyKey, unsub_hook);
}

// call finalizer() at process finish
__attribute__((destructor))
void finalizer()
{
    // note: sGovernor is already destroyed at this step
    // so can't call sGovernor->HandleThreadFinalize()
    // this is not a problem, because main thread should
    //  be subscribed at process finish anyway
}

void sub_hook()
{
    pthread_setspecific(dummyKey, (void*)1);
}

void unsub_hook(void* /*argptr*/)
{
    sGovernor->Unsubscribe();
}
