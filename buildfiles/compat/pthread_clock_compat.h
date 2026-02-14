#ifndef HSTREAM_BUILD_COMPAT_PTHREAD_CLOCK_COMPAT_H_
#define HSTREAM_BUILD_COMPAT_PTHREAD_CLOCK_COMPAT_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int uint;

int pthread_mutex_clocklock(
    void *mutex,
    int clock_id,
    const void *abstime);

int pthread_cond_clockwait(
    void *cond,
    void *mutex,
    int clock_id,
    const void *abstime);

int pthread_rwlock_clockrdlock(
    void *rwlock,
    int clock_id,
    const void *abstime);

int pthread_rwlock_clockwrlock(
    void *rwlock,
    int clock_id,
    const void *abstime);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // HSTREAM_BUILD_COMPAT_PTHREAD_CLOCK_COMPAT_H_
