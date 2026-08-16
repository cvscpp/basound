#ifndef _LINUX_MUTEX_H_
#define _LINUX_MUTEX_H_

#include <sys/lock.h>
#include <sys/mutex.h>

/* Linux mutex wrapper around FreeBSD's mtx */
struct mutex {
	struct mtx lock;
};

typedef struct mtx mutex_t;

/* Use macros to wrap FreeBSD mtx functions without conflicting with kernel declarations */
#define mutex_init(m) do { \
	mtx_init(&(m)->lock, "mutex", NULL, MTX_DEF); \
} while (0)

#define mutex_lock(m) do { \
	mtx_lock(&(m)->lock); \
} while (0)

#define mutex_unlock(m) do { \
	mtx_unlock(&(m)->lock); \
} while (0)

#define mutex_destroy(m) do { \
	mtx_destroy(&(m)->lock); \
} while (0)

#endif
