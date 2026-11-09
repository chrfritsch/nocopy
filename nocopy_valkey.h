/* The Valkey tier's surface to the module (nocopy_valkey.c): its state and lifecycle. The store reaches the tier only through nocopy_detour. */

#ifndef NOCOPY_VALKEY_H
#define NOCOPY_VALKEY_H

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
} nocopy_vk;

/* The tier's part of the arena's user area. */
typedef struct {
	uint64_t inval_gen __attribute__((aligned(64))); /* bumped before invalidations are applied (fill guard) */
	int64_t vk_beat;  /* the listener: CLOCK_MONOTONIC ms of its last sign of life, 0 never started */
	uint32_t vk_pid;  /* its pid */
	uint32_t vk_live; /* it is connected and tracking: local copies may be served */
} nocopy_vk_shared;

#ifdef HAVE_NOCOPY_VALKEY
void nocopy_vk_minit(int module_number); /* registers nocopy.valkey* */
void nocopy_vk_mshutdown(void);
const nocopy_detour_ops *nocopy_vk_route(void); /* the tier's detour with nocopy.valkey set, else NULL */
void nocopy_vk_new_request(void);
void nocopy_vk_close(nocopy_vk *v);
uint32_t nocopy_vk_listener(void); /* the live listener's pid, 0 none */
#else
/* --disable-nocopy-valkey: never on */
static inline void nocopy_vk_minit(int module_number) { (void) module_number; }
static inline void nocopy_vk_mshutdown(void) {}
static inline const nocopy_detour_ops *nocopy_vk_route(void) { return NULL; }
static inline void nocopy_vk_new_request(void) {}
static inline void nocopy_vk_close(nocopy_vk *v) { (void) v; }
static inline uint32_t nocopy_vk_listener(void) { return 0; }
#endif

#endif
