/*
 * Deferred pthread cancellation for Android, written for shairport-sync.
 *
 * pthread_cancel() marks the target thread and sends it SHIM_SIGNAL. The
 * signal handler does nothing; its only job is to make a blocked system call
 * return EINTR. The wrapped cancellation points then end the thread with
 * pthread_exit(PTHREAD_CANCELED), so pthread_cleanup_push() handlers run the
 * same way they do with glibc. While cancellation is disabled, an interrupted
 * call is simply retried.
 *
 * Only threads started through pthread_create() after this shim is linked in
 * can be cancelled. Condition variables are assumed to use CLOCK_REALTIME
 * (the default; shairport-sync never sets a clock on them).
 *
 * Link with the flags in cancel_shim.ldflags.
 */
#include "cancel_shim.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SHIM_SIGNAL (SIGRTMIN + 3)
#define JOIN_NUDGE_NS 20000000L   /* how often pthread_join re-signals */
#define WAIT_SLICE_NS 100000000L  /* longest uninterruptible condition wait */

struct shim_thread {
  pthread_t tid;
  volatile int pending; /* cancel requested */
  volatile int exited;
  int detached;
  int creator_done;
  struct shim_thread *next;
};

struct start_args {
  void *(*fn)(void *);
  void *arg;
  struct shim_thread *st;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shim_thread *registry;
static pthread_key_t self_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static __thread struct shim_thread *self;
static __thread int cancel_disabled;

/* The originals, provided by the linker's --wrap. */
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
int __real_pthread_detach(pthread_t);
int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_recv(int, void *, size_t, int);
ssize_t __real_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int __real_accept(int, struct sockaddr *, socklen_t *);
int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int __real_poll(struct pollfd *, nfds_t, int);
int __real_nanosleep(const struct timespec *, struct timespec *);

/* ---- registry ---------------------------------------------------------- */

static struct shim_thread *find_locked(pthread_t tid) {
  struct shim_thread *st = registry;
  while (st && !(st->creator_done && pthread_equal(st->tid, tid)))
    st = st->next;
  return st;
}

static void remove_locked(struct shim_thread *st) {
  for (struct shim_thread **p = &registry; *p; p = &(*p)->next) {
    if (*p == st) {
      *p = st->next;
      return;
    }
  }
}

/* pthread_key destructor: runs when a shim thread ends, after cleanup handlers. */
static void on_thread_exit(void *arg) {
  struct shim_thread *st = arg;
  pthread_mutex_lock(&registry_lock);
  st->exited = 1;
  int free_it = st->detached && st->creator_done;
  if (free_it)
    remove_locked(st);
  pthread_mutex_unlock(&registry_lock);
  if (free_it)
    free(st);
}

static void on_signal(int sig) { (void)sig; }

static void init(void) {
  pthread_key_create(&self_key, on_thread_exit);
  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: blocked calls must return EINTR */
  sigaction(SHIM_SIGNAL, &sa, NULL);
}

/* ---- cancellation state ------------------------------------------------ */

static void maybe_exit(void) {
  struct shim_thread *st = self;
  if (st && st->pending && !cancel_disabled)
    pthread_exit(PTHREAD_CANCELED);
}

/*
 * Called after a call failed with EINTR. Exits if the thread is cancelled.
 * Returns 1 when the call should be retried (our signal arrived while
 * cancellation is disabled), 0 when EINTR came from some other signal.
 */
static int retry_after_eintr(void) {
  struct shim_thread *st = self;
  if (!st || !st->pending)
    return 0;
  maybe_exit();
  return 1;
}

void pthread_testcancel(void) { maybe_exit(); }

int pthread_setcancelstate(int state, int *oldstate) {
  if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
    return EINVAL;
  if (oldstate)
    *oldstate = cancel_disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
  cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
  return 0;
}

/* Asynchronous cancellation isn't supported; every request is deferred. */
int pthread_setcanceltype(int type, int *oldtype) {
  if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;
  if (oldtype)
    *oldtype = PTHREAD_CANCEL_DEFERRED;
  return 0;
}

int pthread_cancel(pthread_t thread) {
  pthread_once(&init_once, init);
  int rc = ESRCH;
  pthread_mutex_lock(&registry_lock);
  struct shim_thread *st = find_locked(thread);
  if (st) {
    st->pending = 1;
    rc = st->exited ? 0 : pthread_kill(thread, SHIM_SIGNAL);
  }
  pthread_mutex_unlock(&registry_lock);
  return rc;
}

/* ---- thread lifecycle -------------------------------------------------- */

static void *trampoline(void *p) {
  struct start_args a = *(struct start_args *)p;
  free(p);
  self = a.st;
  pthread_setspecific(self_key, a.st);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SHIM_SIGNAL);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  return a.fn(a.arg);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*fn)(void *), void *arg) {
  pthread_once(&init_once, init);
  struct shim_thread *st = calloc(1, sizeof(*st));
  struct start_args *a = malloc(sizeof(*a));
  if (!st || !a) {
    free(st);
    free(a);
    return EAGAIN;
  }
  int detachstate = PTHREAD_CREATE_JOINABLE;
  if (attr)
    pthread_attr_getdetachstate(attr, &detachstate);
  st->detached = detachstate == PTHREAD_CREATE_DETACHED;
  a->fn = fn;
  a->arg = arg;
  a->st = st;

  pthread_mutex_lock(&registry_lock);
  st->next = registry;
  registry = st;
  pthread_mutex_unlock(&registry_lock);

  int rc = __real_pthread_create(thread, attr, trampoline, a);

  /* A detached thread may already have finished; it leaves freeing to us. */
  pthread_mutex_lock(&registry_lock);
  if (rc == 0)
    st->tid = *thread;
  st->creator_done = 1;
  int free_it = rc != 0 || (st->detached && st->exited);
  if (free_it)
    remove_locked(st);
  pthread_mutex_unlock(&registry_lock);
  if (rc != 0)
    free(a);
  if (free_it)
    free(st);
  return rc;
}

int __wrap_pthread_join(pthread_t thread, void **retval) {
  maybe_exit();
  pthread_mutex_lock(&registry_lock);
  struct shim_thread *st = find_locked(thread);
  pthread_mutex_unlock(&registry_lock);

  /* A cancel can land just before the target blocks, so keep nudging it. */
  if (st && st->pending) {
    struct timespec nudge = {0, JOIN_NUDGE_NS};
    while (!st->exited) {
      pthread_kill(thread, SHIM_SIGNAL);
      __real_nanosleep(&nudge, NULL);
    }
  }

  int rc = __real_pthread_join(thread, retval);
  if (st) {
    pthread_mutex_lock(&registry_lock);
    remove_locked(st);
    pthread_mutex_unlock(&registry_lock);
    free(st);
  }
  return rc;
}

int __wrap_pthread_detach(pthread_t thread) {
  pthread_mutex_lock(&registry_lock);
  struct shim_thread *st = find_locked(thread);
  int free_it = 0;
  if (st) {
    st->detached = 1;
    free_it = st->exited;
    if (free_it)
      remove_locked(st);
  }
  pthread_mutex_unlock(&registry_lock);
  if (free_it)
    free(st);
  return __real_pthread_detach(thread);
}

/* ---- cancellation points ----------------------------------------------- */

#define CANCEL_POINT(type, call)                                           \
  do {                                                                     \
    type r_;                                                               \
    maybe_exit();                                                          \
    do {                                                                   \
      r_ = (call);                                                         \
    } while (r_ < 0 && errno == EINTR && retry_after_eintr());             \
    return r_;                                                             \
  } while (0)

ssize_t __wrap_read(int fd, void *buf, size_t count) {
  CANCEL_POINT(ssize_t, __real_read(fd, buf, count));
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags) {
  CANCEL_POINT(ssize_t, __real_recv(fd, buf, len, flags));
}

ssize_t __wrap_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr,
                        socklen_t *addrlen) {
  CANCEL_POINT(ssize_t, __real_recvfrom(fd, buf, len, flags, addr, addrlen));
}

int __wrap_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  CANCEL_POINT(int, __real_accept(fd, addr, addrlen));
}

/* On Linux, select() updates the timeout, so a retry waits only for the rest. */
int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout) {
  CANCEL_POINT(int, __real_select(nfds, r, w, e, timeout));
}

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  CANCEL_POINT(int, __real_poll(fds, nfds, timeout));
}

int __wrap_nanosleep(const struct timespec *req, struct timespec *rem) {
  struct timespec left = *req, r;
  maybe_exit();
  for (;;) {
    if (__real_nanosleep(&left, &r) == 0)
      return 0;
    if (errno != EINTR)
      return -1;
    if (!retry_after_eintr()) {
      if (rem)
        *rem = r;
      return -1;
    }
    left = r;
  }
}

int __wrap_usleep(useconds_t usec) {
  struct timespec ts = {usec / 1000000, (long)(usec % 1000000) * 1000};
  return __wrap_nanosleep(&ts, NULL);
}

unsigned int __wrap_sleep(unsigned int seconds) {
  struct timespec ts = {seconds, 0}, rem = {0, 0};
  if (__wrap_nanosleep(&ts, &rem) == 0)
    return 0;
  return rem.tv_sec + (rem.tv_nsec > 0);
}

/* ---- condition variables ----------------------------------------------- */

static void realtime_in(struct timespec *ts, long ns) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_nsec += ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec++;
  }
}

static int earlier(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

/*
 * Signals don't wake a condition wait, so wait in slices and check for a
 * cancel in between. As with glibc, a cancelled thread exits holding the
 * mutex, and its cleanup handler is expected to unlock it.
 */
int __wrap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime) {
  maybe_exit();
  for (;;) {
    struct timespec slice;
    realtime_in(&slice, WAIT_SLICE_NS);
    const struct timespec *until = earlier(abstime, &slice) ? abstime : &slice;
    int rc = __real_pthread_cond_timedwait(cond, mutex, until);
    maybe_exit();
    if (rc != ETIMEDOUT || until == abstime)
      return rc;
  }
}

/* A slice that times out is reported as a spurious wakeup, which POSIX allows. */
int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  struct timespec slice;
  maybe_exit();
  realtime_in(&slice, WAIT_SLICE_NS);
  int rc = __real_pthread_cond_timedwait(cond, mutex, &slice);
  maybe_exit();
  return rc == ETIMEDOUT ? 0 : rc;
}

/* ---- other bionic gaps ------------------------------------------------- */

#if __ANDROID_API__ < 28
/* Look the real one up at runtime; on devices too old to have it, do nothing. */
int shim_pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
  static int (*real)(pthread_attr_t *, int);
  if (!real)
    real = (int (*)(pthread_attr_t *, int))dlsym(RTLD_DEFAULT, "pthread_attr_setinheritsched");
  return real ? real(attr, inheritsched) : 0;
}
#endif
