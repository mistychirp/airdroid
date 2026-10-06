/*
 * Deferred pthread cancellation for Android (bionic has none).
 *
 * Force-included into every translation unit (-include cancel_shim.h).
 * The blocking calls that act as cancellation points are redirected to the
 * shim with the linker's --wrap option; see cancel_shim.c.
 */
#ifndef CANCEL_SHIM_H
#define CANCEL_SHIM_H

#include <pthread.h>

#ifndef PTHREAD_CANCEL_ENABLE
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#endif

#ifndef PTHREAD_CANCELED
#define PTHREAD_CANCELED ((void *)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);
void pthread_testcancel(void);

/* Exists from API 28, but Termux builds against an older API level. */
#if __ANDROID_API__ < 28
int shim_pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched);
#define pthread_attr_setinheritsched shim_pthread_attr_setinheritsched
#endif

#ifdef __cplusplus
}
#endif

#endif
