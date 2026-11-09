/* nocopy arena: time. TTLs are whole seconds of the wall clock; a test may replace those seconds (nocopy_set_clock). */

#ifndef NOCOPY_ARENA_CLOCK_H
#define NOCOPY_ARENA_CLOCK_H

#include "base.h"

#define NOCOPY_TTL_MAX ((time_t) 1 << 32) /* s (~136 years): longer TTLs are capped, so now + ttl cannot overflow */

typedef time_t (*nocopy_clock_fn)(void);
AR_HIDDEN extern nocopy_clock_fn ar_clock_fake; /* NULL: the wall clock */

/* The coarse clock is a tick behind at most (1-4 ms) and 3x cheaper than CLOCK_REALTIME (3.5 vs 11.2 ns). Sub-second: always the wall clock. */
static inline void nocopy_clock(struct timespec *ts)
{
#ifdef CLOCK_REALTIME_COARSE
	clock_gettime(CLOCK_REALTIME_COARSE, ts);
#else
	clock_gettime(CLOCK_REALTIME, ts);
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
