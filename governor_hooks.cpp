#include <dlfcn.h>
#include <pthread.h>

#include <cassert>

#include "governor_impl.h"

// handle process init/exit hooks
// global variables
static pthread_key_t dummyKey; // used for thread exit hook
static bool isInit = false;

void initializer();
void finalizer();
void* thread_initializer(void* argptr);
void thread_finalizer(void* argptr);

// call initializer() at process startup
__attribute__((constructor))
void initializer()
{
    if (isInit) // ensure initializer is only called once
        return;

    isInit = true;
    pthread_key_create(&dummyKey, thread_finalizer);

    sGovernor->HandleThreadInit();
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

// handle thread init/exit hooks
struct ThreadInfo
{
    void* (*fn)(void*);
    void* args;
};

void* thread_initializer(void* argptr)
{
    // consume and deallocate info
    ThreadInfo* info = (ThreadInfo*)argptr;
    void* (*fn)(void*) = info->fn;
    void* args = info->args;
    delete info; // deallocate

    sGovernor->HandleThreadInit();

    pthread_setspecific(dummyKey, (void*)1);
    return (*fn)(args);
}

void thread_finalizer(void* /*value*/)
{
    sGovernor->HandleThreadFinalize();
}

// override pthread_create
int pthread_create(pthread_t* thread, pthread_attr_t const* attr,
    void* (startRoutine)(void*), void* arg)
{
    static int (*pthread_create_fn)(pthread_t*,
                                    pthread_attr_t const*,
                                    void* (void*),
                                    void*) = nullptr;
    if (pthread_create_fn == nullptr)
        pthread_create_fn = (int(*)(pthread_t*, pthread_attr_t const*, void* (void*), void*))dlsym(RTLD_NEXT, "pthread_create");

    ThreadInfo* info = new ThreadInfo();
    info->fn = startRoutine;
    info->args = arg;

    return pthread_create_fn(thread, attr, thread_initializer, info);
}

