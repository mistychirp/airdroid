/* Checks the cancellation shim the way shairport-sync uses cancellation. */
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int cleanups;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static void count_cleanup(void *arg) { (void)arg; cleanups++; }
static void unlock_cleanup(void *arg) { pthread_mutex_unlock(arg); cleanups++; }

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *blocked_in_recv(void *arg) {
  int fd = *(int *)arg;
  char buf[16];
  pthread_cleanup_push(count_cleanup, NULL);
  recv(fd, buf, sizeof(buf), 0); /* nothing is ever sent */
  pthread_cleanup_pop(0);
  return NULL;
}

static void *blocked_in_cond_wait(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
  pthread_cleanup_push(unlock_cleanup, &mutex);
  for (;;)
    pthread_cond_wait(&cond, &mutex); /* never signalled */
  pthread_cleanup_pop(1);
  return NULL;
}

static void *sleeps_with_cancel_disabled(void *arg) {
  int *finished_sleep = arg;
  int old;
  pthread_cleanup_push(count_cleanup, NULL);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
  usleep(300000);
  *finished_sleep = 1;
  pthread_setcancelstate(old, NULL);
  pthread_testcancel();
  pthread_cleanup_pop(0);
  return NULL;
}

static int check(const char *name, void *(*fn)(void *), void *arg, double max_s) {
  pthread_t t;
  void *ret = NULL;
  pthread_create(&t, NULL, fn, arg);
  usleep(100000);
  double start = now();
  pthread_cancel(t);
  pthread_join(t, &ret);
  double took = now() - start;
  int ok = ret == PTHREAD_CANCELED && took < max_s;
  printf("%-28s %s (%.0f ms)\n", name, ok ? "ok" : "FAIL", took * 1000);
  return ok;
}

int main(void) {
  int sv[2], finished_sleep = 0, ok = 1;
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

  ok &= check("cancel blocked recv", blocked_in_recv, &sv[0], 0.5);
  ok &= check("cancel blocked cond_wait", blocked_in_cond_wait, NULL, 0.5);
  int lockable = pthread_mutex_trylock(&mutex) == 0;
  if (lockable)
    pthread_mutex_unlock(&mutex);
  printf("%-28s %s\n", "mutex released by cleanup", lockable ? "ok" : "FAIL");
  ok &= lockable;

  ok &= check("cancel while disabled", sleeps_with_cancel_disabled, &finished_sleep, 1.0);
  printf("%-28s %s\n", "disabled section completed", finished_sleep ? "ok" : "FAIL");
  ok &= finished_sleep;

  printf("%-28s %s (%d)\n", "cleanup handlers ran", cleanups == 3 ? "ok" : "FAIL", cleanups);
  ok &= cleanups == 3;
  printf(ok ? "ALL OK\n" : "SOME FAILED\n");
  return !ok;
}
