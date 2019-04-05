#ifndef __GOVERNOR_H__
#define __GOVERNOR_H__

// by default, governor is disabled
#ifndef GOVERNOR
#define GOVERNOR 0
#endif // GOVERNOR

#if GOVERNOR == 0

// macro user API
#define GOV_PREPARE(numThreads)
#define GOV_SUBSCRIBE(threadId)
#define GOV_UNSUBSCRIBE()
#define GOV_CONTROL()
#define GOV_RESET() (1)

#else // if GOVERNOR

#define GOV_PREPARE(numThreads) governor_prepare(numThreads)
#define GOV_SUBSCRIBE(threadId) governor_subscribe(threadId)
#define GOV_UNSUBSCRIBE() governor_unsubscribe()
#define GOV_CONTROL() governor_control()
#define GOV_RESET() governor_reset()

#ifdef __cplusplus
#include <cstddef>
#else
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void governor_prepare(size_t numThreads);
void governor_subscribe(size_t threadId);
void governor_unsubscribe();
void governor_control();
int governor_reset();

#ifdef __cplusplus
}
#endif

#endif // GOVERNOR

#endif // __GOVERNOR_H__

