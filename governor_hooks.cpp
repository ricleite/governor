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
