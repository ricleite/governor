
#ifndef __GOVERNOR_HOOKS_H_
#define __GOVERNOR_HOOKS_H_

// program start/exit hooks
void initializer();
void finalizer();
// thread hooks that must be called by Governor code
// if sub_hook() is called by a thread, unsub_hook() will
//  be called when that thread exits
void sub_hook();
void unsub_hook(void* argptr);

#endif // __GOVERNOR_HOOKS_H_