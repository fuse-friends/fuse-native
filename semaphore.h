
#ifdef __APPLE__

#include <semaphore.h>
#include <dispatch/dispatch.h>

typedef dispatch_semaphore_t fuse_native_semaphore_t;

static int fuse_native_semaphore_init (dispatch_semaphore_t *sem) {
  *sem = dispatch_semaphore_create(0);
  return *sem == NULL ? -1 : 0;
}

static void fuse_native_semaphore_wait (dispatch_semaphore_t *sem) {
  dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER);
}

static void fuse_native_semaphore_signal (dispatch_semaphore_t *sem) {
  dispatch_semaphore_signal(*sem);
}

#else

#include <semaphore.h>

typedef sem_t fuse_native_semaphore_t;

static int fuse_native_semaphore_init (sem_t *sem) {
  return sem_init(sem, 0, 0);
}

static void fuse_native_semaphore_wait (sem_t *sem) {
  sem_wait(sem);
}

static void fuse_native_semaphore_signal (sem_t *sem) {
  sem_post(sem);
}

#endif
