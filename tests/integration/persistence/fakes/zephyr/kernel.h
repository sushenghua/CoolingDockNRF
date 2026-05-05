/* Tiny shim for the slice of <zephyr/kernel.h> that sys_data and
 * cmd_interpreter consume. Backed by pthreads on the host so the
 * production source files compile without modification. */

#ifndef SHIM_ZEPHYR_KERNEL_H
#define SHIM_ZEPHYR_KERNEL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARG_UNUSED(x)  ((void)(x))

#ifndef MIN
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#endif

/* k_mutex → pthread_mutex */
struct k_mutex {
	pthread_mutex_t m;
};
/* Production callsite is `static K_MUTEX_DEFINE(...)`, so the macro
 * itself doesn't add `static` — that would be duplicate. */
#define K_MUTEX_DEFINE(name) \
	struct k_mutex name = { .m = PTHREAD_MUTEX_INITIALIZER }
#define K_FOREVER (-1)

static inline int k_mutex_lock(struct k_mutex *mu, int timeout)
{
	(void)timeout;
	return pthread_mutex_lock(&mu->m);
}
static inline int k_mutex_unlock(struct k_mutex *mu)
{
	return pthread_mutex_unlock(&mu->m);
}

/* k_work — no-ops; cmd_interpreter only uses this to schedule a delayed
 * reboot, which the test isn't going to wait for anyway. */
struct k_work       { int unused; };
struct k_work_delayable { int unused; };
typedef int k_timeout_t;

#define K_MSEC(ms)    (ms)
#define K_NO_WAIT     (0)

/* Used as `static K_WORK_DELAYABLE_DEFINE(...)` in production, so we
 * leave `static` to the callsite. The function-pointer trick the real
 * Zephyr macro plays isn't needed here because we never run the work
 * — just satisfy the type. */
#define K_WORK_DELAYABLE_DEFINE(_name, _fn) \
	struct k_work_delayable _name __attribute__((unused))

static inline int k_work_schedule(struct k_work_delayable *w, k_timeout_t d)
{
	(void)w; (void)d;
	return 0;
}

/* IS_ENABLED — Zephyr's compile-time Kconfig probe. We stub it false so
 * any "if (IS_ENABLED(CONFIG_X))" branch in tested code takes the
 * non-X path. */
#define IS_ENABLED(x) (0)

#endif /* SHIM_ZEPHYR_KERNEL_H */
