#ifndef __MUTEX_IMPL_H
#define __MUTEX_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

void init_drain_mutex();
void lock_drain_mutex();
void unlock_drain_mutex();

#ifdef __cplusplus
}
#endif

#endif
