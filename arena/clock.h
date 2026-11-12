/* nocopy arena: time. TTLs, leases and versions run on the coarse monotonic clock (the arena does not outlive a boot), so a stepped
 * wall clock neither expires entries early nor keeps them late; wall time only where it leaves the arena. A test may replace the seconds. */

#ifndef NOCOPY_ARENA_CLOCK_H
#define NOCOPY_ARENA_CLOCK_H

#include "base.h"

#define NOCOPY_TTL_MAX ((time_t) 1 << 32) /* s (~136 years): longer TTLs are capped, so now + ttl cannot overflow */

typedef time_t (*nocopy_clock_fn)(void);
AR_HIDDEN extern nocopy_clock_fn ar_clock_fake; /* NULL: the monotonic clock */

/* Coarse: a tick behind at most (1-4 ms), 3x cheaper than CLOCK_MONOTONIC or CLOCK_BOOTTIME (3.6-4.1 vs 11-14 ns). Stops while suspended. */
static inline void nocopy_clock(struct timespec *ts)
{
#ifdef CLOCK_MONOTONIC_COARSE
	clock_gettime(CLOCK_MONOTONIC_COARSE, ts);
#else
	clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

/* The seconds of a nocopy_clock() reading: one clock read gives a write both its TTL time and its sub-second version stamp. */
static inline time_t nocopy_now_of(const struct timespec *ts) { return __builtin_expect(ar_clock_fake != NULL, 0) ? ar_clock_fake() : ts->tv_sec; }

static inline time_t nocopy_now(void)
{
	if (__builtin_expect(ar_clock_fake != NULL, 0)) {
		return ar_clock_fake();
	}
	struct timespec ts;
	nocopy_clock(&ts);
	return ts.tv_sec;
}

/* A local second as unix time, and back: for key listings and the RESP payload (within a second). 0 stays 0 (never). */
static inline time_t nocopy_wall_offset(void) { return time(NULL) - nocopy_now(); }
static inline time_t nocopy_to_wall(time_t t) { return t ? t + nocopy_wall_offset() : 0; }
static inline time_t nocopy_from_wall(time_t t)
{
	time_t o = nocopy_wall_offset();
	return t ? (t > o ? t - o : 1) : 0;
}

/* For durations and deadlines: CLOCK_MONOTONIC ms. */
static inline int64_t nocopy_mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The last second an entry lives: ttl 0 never expires (0), else now + ttl, capped. */
static inline time_t nocopy_expiry(time_t now, time_t ttl) { return ttl ? now + (ttl < NOCOPY_TTL_MAX ? ttl : NOCOPY_TTL_MAX) : 0; }

#endif
