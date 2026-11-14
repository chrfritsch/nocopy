/* The RESP tier's surface to the module (nocopy_resp.c): its state and lifecycle. The store reaches the tier only through nocopy_detour. */

#ifndef NOCOPY_RESP_H
#define NOCOPY_RESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct nocopy_detour_ops nocopy_detour_ops; /* nocopy_php.h */

/* This worker's connection. */
typedef struct {
	void *c;          /* valkeyContext *, NULL: not connected */
	void *ssl;        /* SSL *, with TLS */
	char err[160];    /* why the connection failed last */
	int64_t retry_at; /* ms, monotonic: after a failure, no new attempt before */
	bool synced;      /* connected and the listener live, checked in this request */
} nocopy_resp_conn;

/* The tier's part of the arena's user area. */
typedef struct {
	uint64_t inval_gen __attribute__((aligned(64))); /* bumped before invalidations are applied (fill guard) */
	int64_t beat;  /* the listener: CLOCK_MONOTONIC ms of its last sign of life, 0 never started */
	uint32_t pid;  /* its pid */
	uint32_t live; /* it is connected and tracking: local copies may be served */
} nocopy_resp_shared;

#ifdef HAVE_NOCOPY_RESP
void nocopy_resp_minit(int module_number); /* registers nocopy.resp* */
void nocopy_resp_mshutdown(void);
const nocopy_detour_ops *nocopy_resp_route(void); /* the tier's detour with nocopy.resp set, else NULL */
void nocopy_resp_new_request(void);
void nocopy_resp_close(nocopy_resp_conn *v);
uint32_t nocopy_resp_listener(void); /* the live listener's pid, 0 none */
#else
/* --disable-nocopy-resp: never on */
static inline void nocopy_resp_minit(int module_number) { (void) module_number; }
static inline void nocopy_resp_mshutdown(void) {}
static inline const nocopy_detour_ops *nocopy_resp_route(void) { return NULL; }
static inline void nocopy_resp_new_request(void) {}
static inline void nocopy_resp_close(nocopy_resp_conn *v) { (void) v; }
static inline uint32_t nocopy_resp_listener(void) { return 0; }
#endif

#endif
