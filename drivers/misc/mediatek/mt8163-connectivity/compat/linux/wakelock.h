/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT8163_CONN_ANDROID_WAKELOCK_H
#define _MT8163_CONN_ANDROID_WAKELOCK_H

#include <linux/jiffies.h>
#include <linux/pm_wakeup.h>

#define WAKE_LOCK_SUSPEND 0

struct wake_lock {
	struct wakeup_source *ws;
};

static inline void wake_lock_init(struct wake_lock *lock, int type,
				  const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}

static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}

static inline void wake_lock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_stay_awake(lock->ws);
}

static inline void wake_unlock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_relax(lock->ws);
}

static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	if (lock->ws)
		__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}

static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

#endif
