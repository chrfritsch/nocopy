/* stash arena: segments, shards, slot tables, pins, expiry and merge eviction. No PHP in here. */

#include "stash_arena.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sys/sysctl.h>
#else
# include <fcntl.h>
#endif

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#define MERGE_N 4 /* segments merged into at most one segment's worth of survivors (Segcache) */

stash_arena *stash_a;
char *stash_seg_base;
unsigned stash_seg_shift;
uint32_t stash_shard_mask, stash_tab_mask;
stash_slot *stash_tabs[STASH_MAX_SHARDS];
static uint32_t *stash_ghosts[STASH_MAX_SHARDS]; /* per shard, as many as slots: tags of recently evicted keys (S3-FIFO ghost) */
uint64_t stash_self;

static size_t seg_size;
static time_t checked[STASH_MAX_WORKERS]; /* last liveness check of a pinning worker: at most one a second */

static inline char *seg_start(uint32_t s) { return stash_seg_base + ((size_t) s << stash_seg_shift); }
static inline stash_slot slot_make(const stash_entry *e) { return (e->h << 32) | (uint32_t) (((const char *) e - (const char *) stash_a) >> 3); }
static inline stash_slot *tab_of(const stash_shard *sh) { return stash_tabs[sh - stash_a->shards]; }

/* Direct-mapped: a newer eviction overwrites an older one in its cell. Index: Fibonacci mix below the shard's top 8 bits
 * (short keys' DJB hashes have few high bits); tag: the low bits (never 0). */
static inline uint32_t *ghost_cell(const stash_shard *sh, uint64_t h)
{
	return &stash_ghosts[sh - stash_a->shards][(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 24) & stash_tab_mask];
}
static inline uint32_t ghost_tag(uint64_t h) { return (uint32_t) h | 1; }

/* ---- owner ids: pid + process start time, so a recycled pid does not look like its dead predecessor ---- */

/* Start time of pid (any stable 32-bit stamp), 0 if it is gone or a zombie. */
static uint32_t proc_start(int pid)
{
#ifdef __APPLE__
	struct kinfo_proc kp;
	size_t len = sizeof kp;
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof kp || kp.kp_proc.p_stat == SZOMB) {
		return 0;
	}
	return (uint32_t) (kp.kp_proc.p_starttime.tv_sec * 1000003 + kp.kp_proc.p_starttime.tv_usec) | 1;
#else
	char path[32], buf[1024];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) {
		return 0;
	}
	buf[n] = 0;
	char *p = strrchr(buf, ')'); /* "pid (comm) S ...": comm may hold anything; starttime is field 22 */
	if (!p || p[1] != ' ' || p[2] == 'Z') {
		return 0;
	}
	for (int f = 2; f < 22 && p; f++) {
		p = strchr(p + 1, ' ');
	}
	return p ? (uint32_t) strtoull(p + 1, NULL, 10) | 1 : 0;
#endif
}

static uint64_t make_self(void)
{
	int pid = getpid();
	return (uint64_t) (uint32_t) pid | (uint64_t) proc_start(pid) << 32;
}

/* Gone, a zombie, or its pid now belongs to a newer process. */
static bool owner_dead(uint64_t id)
{
	int pid = (int) (uint32_t) id;
	if (kill(pid, 0) == -1 && errno == ESRCH) {
		return true;
	}
	return proc_start(pid) != (uint32_t) (id >> 32);
}

/* ---- locks ---- */

static void shard_repair(stash_shard *sh, uint64_t dead);
static void wip_abort(int slot);
static void wip_reap(stash_shard *sh);

/* Slow path of stash_lock(): test-and-test-and-set holding the owner id; yields once the wait gets long, and takes the lock over from a dead holder.
 * Whatever that holder was in the middle of is then repaired (shard_repair).
 * Robust pthread mutexes were measured 2-8x slower under contention (futex sleeps); this keeps the spin. */
void stash_lock_wait(stash_shard *sh)
{
	uint64_t *l = &sh->lock;
	for (unsigned spins = 0;;) {
		uint64_t h = __atomic_load_n(l, __ATOMIC_RELAXED);
		if (!h) {
			if (__atomic_compare_exchange_n(l, &h, stash_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				return;
			}
		} else if (++spins < 128) {
			stash_cpu_relax();
		} else {
			sched_yield();
			if (!(spins & 1023) && h != stash_self && owner_dead(h)
			 && __atomic_compare_exchange_n(l, &h, stash_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				__atomic_add_fetch(&stash_a->lock_steals, 1, __ATOMIC_RELAXED);
				shard_repair(sh, h);
				return;
			}
		}
	}
}

/* ---- workers and pins ---- */

static void bits_clear(int slot)
{
	for (int w = 0; w < STASH_SEG_WORDS; w++) {
		__atomic_store_n(&stash_a->workers[slot].bits[w], 0, __ATOMIC_SEQ_CST);
	}
}

bool stash_claim(stash_ctx *c)
{
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < STASH_MAX_WORKERS; i++) {
			uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
			if (pass ? !o || !owner_dead(o) : o != 0) {
				continue;
			}
			if (__atomic_compare_exchange_n(&stash_a->workers[i].owner, &o, stash_self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				bits_clear(i);
				wip_abort(i); /* its previous owner died filling an entry */
				uint32_t cur = __atomic_load_n(&stash_a->nworkers, __ATOMIC_RELAXED);
				while (cur < (uint32_t) i + 1
				 && !__atomic_compare_exchange_n(&stash_a->nworkers, &cur, i + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
				c->slot = i;
				c->rng = (uint32_t) (stash_self ^ (stash_self >> 32)) * 2654435761u + (uint32_t) i | 1;
				return true;
			}
		}
	}
	c->slot = -1;
	return false;
}

void stash_release(stash_ctx *c)
{
	if (c->slot >= 0) {
		bits_clear(c->slot);
		__atomic_store_n(&stash_a->workers[c->slot].owner, 0, __ATOMIC_SEQ_CST);
		c->slot = -1;
	}
}

/* A forked child gets its own owner id and worker slot, and inherits the parent's pins. */
void stash_after_fork(stash_ctx *c)
{
	stash_self = make_self();
	if (stash_claim(c)) {
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			__atomic_store_n(&stash_a->workers[c->slot].bits[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
}

/* Drop the kept pins: the caller promises no earlier zero-copy result is used again. */
void stash_keep_release(stash_ctx *c)
{
	memset(c->keep, 0, sizeof c->keep);
	if (!c->pin_refs) {
		c->transient = true;
		stash_drop_transient(c);
	}
}

/* ---- segments (shard locked) ---- */

static void seg_set(uint32_t s, uint8_t st) { __atomic_store_n(&stash_a->segs[s].state, st, __ATOMIC_RELEASE); }

static void seg_check(stash_shard *sh, uint32_t s)
{
	stash_seg *g = &stash_a->segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		seg_set(s, SEG_DEAD);
		sh->ndead++;
		sh->hint = true;
	}
}

static void seg_free(stash_shard *sh, uint32_t s)
{
	uint32_t k = stash_a->segs[s].run;
#ifdef STASH_POISON
	memset(seg_start(s), 0x5A, (size_t) k << stash_seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		uint32_t gen = stash_a->segs[j].gen + 1;
		memset(&stash_a->segs[j], 0, sizeof(stash_seg)); /* SEG_FREE */
		stash_a->segs[j].gen = gen;
	}
	sh->nfree += k;
	sh->ndead--;
}

/* Hand back sh's dead segments that no live worker pins. */
static void reclaim(stash_shard *sh)
{
	sh->hint = false;
	if (!sh->ndead) {
		return;
	}
	/* Dead set first, pins second: a reader pins, then re-checks its slot; a segment that died after we read the pins
	 * could have been pinned (and re-checked) just before. DEAD only ends here, under this lock. */
	uint64_t dead[STASH_SEG_WORDS] = {0};
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (stash_a->segs[s].state == SEG_DEAD) {
			dead[s >> 6] |= (uint64_t) 1 << (s & 63);
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	uint64_t held[STASH_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	time_t now = time(NULL);
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[STASH_SEG_WORDS], blocks = 0;
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			b[w] = __atomic_load_n(&stash_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
			blocks |= b[w] & dead[w];
		}
		if (!blocks) {
			continue;
		}
		uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o && o != stash_self && __atomic_exchange_n(&checked[i], now, __ATOMIC_RELAXED) != now) {
			if (owner_dead(o)) {
				bits_clear(i); /* died holding pins */
				continue;
			}
		}
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			held[w] |= b[w];
		}
	}
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if ((dead[s >> 6] & ~held[s >> 6]) >> (s & 63) & 1) {
			seg_free(sh, s);
		}
	}
}

static void entry_die(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	e->live = false;
	stash_a->segs[s].live -= e->size;
	sh->live_bytes -= e->size;
	seg_check(sh, s);
}

static void entry_birth(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	e->live = true;
	stash_a->segs[s].pending--;
	stash_a->segs[s].live += e->size;
	sh->live_bytes += e->size;
}

void stash_abort(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	stash_a->segs[s].pending--;
	seg_check(sh, s);
}

static int ttl_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < STASH_TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

static stash_entry *seg_carve(uint32_t s, size_t total, time_t exp)
{
	stash_seg *g = &stash_a->segs[s];
	stash_entry *e = (stash_entry *) (seg_start(s) + g->used);
	e->size = total; /* before used covers it: a repair walk must never meet a header without a size */
	e->live = false;
	e->hot = 0;
	g->used += total;
	g->pending++;
	if (exp > g->max_exp) {
		g->max_exp = exp;
	}
	return e;
}

/* k contiguous free segments of sh, leaving `spare` free ones; the first gets `state`. UINT32_MAX if none. */
static uint32_t take_run(stash_shard *sh, uint32_t k, uint32_t spare, uint8_t state)
{
	if (sh->nfree < k + spare) {
		return UINT32_MAX;
	}
	for (uint32_t s = sh->lo, n = 0; s < sh->hi; s++) {
		n = stash_a->segs[s].state == SEG_FREE ? n + 1 : 0;
		if (n == k) {
			uint32_t got = s + 1 - k;
			for (uint32_t j = got + 1; j <= s; j++) {
				seg_set(j, SEG_CONT);
			}
			stash_a->segs[got].run = k;
			stash_a->segs[got].born = __atomic_load_n(&stash_a->inserts, __ATOMIC_RELAXED);
			seg_set(got, state);
			sh->nfree -= k;
			return got;
		}
	}
	return UINT32_MAX;
}

/* reserve: may take the last free segment (kept back so a merge can always make progress). exp picks the TTL bucket. */
static stash_entry *seg_alloc(stash_shard *sh, size_t total, bool reserve, time_t exp, time_t now)
{
	int b = ttl_bucket(exp ? exp - now : 0);
	uint32_t spare = reserve ? 0 : 1, got;
	if (total > seg_size / 2) { /* a run of contiguous segments */
		got = take_run(sh, (uint32_t) ((total + seg_size - 1) >> stash_seg_shift), spare, SEG_SEALED);
		return got == UINT32_MAX ? NULL : seg_carve(got, total, exp);
	}
	int32_t cur = sh->cur[b];
	if (cur >= 0 && stash_a->segs[cur].used + total <= seg_size) {
		return seg_carve(cur, total, exp);
	}
	got = take_run(sh, 1, spare, SEG_OPEN);
	if (got == UINT32_MAX) {
		return NULL;
	}
	if (cur >= 0) {
		seg_set(cur, SEG_SEALED);
		seg_check(sh, cur);
	}
	sh->cur[b] = (int32_t) got;
	return seg_carve(got, total, exp);
}

/* ---- slot table (shard locked) ---- */

static stash_slot *locate(stash_shard *sh, uint64_t h, const char *k, uint32_t klen, stash_entry **found)
{
	stash_slot *t = tab_of(sh);
	uint32_t tag = (uint32_t) h;
	*found = NULL;
	for (uint32_t i = h & stash_tab_mask;; i = (i + 1) & stash_tab_mask) { /* never full: nlive <= cap < slots */
		stash_slot s = t[i];
		if (!s) {
			return &t[i];
		}
		if ((uint32_t) (s >> 32) == tag) {
			stash_entry *e = stash_slot_entry(s);
			if (e->h == h && e->klen == klen && !memcmp(stash_key(e), k, klen)) {
				*found = e;
				return &t[i];
			}
		}
	}
}

/* Backward-shift deletion: no tombstones. Readers that miss while a key moves back past them retry (mseq). */
static void slot_remove(stash_shard *sh, stash_slot *p)
{
	stash_slot *t = tab_of(sh);
	uint32_t m = stash_tab_mask, i = (uint32_t) (p - t);
	bool shifting = false;
	for (uint32_t j = (i + 1) & m;; j = (j + 1) & m) {
		stash_slot s = t[j];
		if (!s) {
			break;
		}
		uint32_t home = (uint32_t) (s >> 32) & m;
		if (((j - home) & m) >= ((j - i) & m)) { /* i lies between its home and j: move it back */
			if (!shifting) {
				shifting = true;
				__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELAXED);
				__atomic_thread_fence(__ATOMIC_RELEASE);
			}
			__atomic_store_n(&t[i], s, __ATOMIC_RELEASE);
			i = j;
		}
	}
	__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
	if (shifting) {
		__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELEASE);
	}
	sh->nlive--;
}

static stash_slot *slot_of(stash_shard *sh, const stash_entry *e)
{
	stash_slot *t = tab_of(sh);
	for (uint32_t i = e->h & stash_tab_mask;; i = (i + 1) & stash_tab_mask) {
		if (!t[i]) {
			return NULL;
		}
		if (stash_slot_entry(t[i]) == e) {
			return &t[i];
		}
	}
}

static void entry_unlink(stash_shard *sh, stash_entry *e)
{
	stash_slot *p = slot_of(sh, e);
	if (p) {
		slot_remove(sh, p);
	}
	entry_die(sh, e);
}

/* ---- expiry ---- */

/* Unlinks entries of sh's segments whose entries have all expired, looking at most `budget` entries per call.
 * True if anything was unlinked. A walk may run past a segment its last unlink made DEAD: only reclaim() frees it, under this lock. */
static bool expire_step(stash_shard *sh, time_t now, int budget)
{
	bool did = false;
	if (sh->exp_idle == now) {
		return false;
	}
	uint32_t n = sh->hi - sh->lo;
	if (sh->exp_next < sh->lo || sh->exp_next >= sh->hi) {
		sh->exp_next = sh->lo;
	}
	for (uint32_t seen = 0; seen < n; seen++) {
		uint32_t s = sh->exp_next;
		stash_seg *g = &stash_a->segs[s];
		if ((g->state == SEG_OPEN || g->state == SEG_SEALED) && g->live && g->max_exp && g->max_exp < now) {
			if (sh->exp_gen != g->gen) { /* freed and reused since the cursor stopped here: entry boundaries moved */
				sh->exp_gen = g->gen;
				sh->exp_off = 0;
			}
			while (sh->exp_off < g->used) {
				if (budget-- <= 0) {
					return did; /* resume here on the next write */
				}
				stash_entry *e = (stash_entry *) (seg_start(s) + sh->exp_off);
				sh->exp_off += e->size;
				if (e->live) {
					entry_unlink(sh, e);
					sh->expired++;
					did = true;
				}
			}
		}
		sh->exp_off = 0;
		sh->exp_next = s + 1 < sh->hi ? s + 1 : sh->lo;
	}
	sh->exp_idle = now;
	return did;
}

/* ---- merge (Segcache): compaction and eviction in one pass ---- */

/* Copies e forward into its TTL bucket and repoints its slot. False if there is no room. */
static bool relocate(stash_shard *sh, stash_entry *old)
{
	/* now = created: keeps the entry in its original TTL bucket */
	stash_entry *n = seg_alloc(sh, old->size, true, old->expires, old->created);
	if (!n) {
		return false;
	}
	n->h = old->h;
	n->expires = old->expires;
	n->created = old->created;
	n->seq = old->seq;
	n->klen = old->klen;
	n->koff = old->koff;
	n->flags = old->flags;
	n->user = old->user;
	n->hot = __atomic_load_n(&old->hot, __ATOMIC_RELAXED) >> 1; /* aging: must be read again to stay on top */
	stash_relocate(n, old);
	sh->evacuated += n->size;
	stash_slot *sp = slot_of(sh, old);
	if (sp) {
		entry_birth(sh, n);
		__atomic_store_n(sp, slot_make(n), __ATOMIC_RELEASE);
	} else {
		stash_abort(sh, n);
	}
	entry_die(sh, old);
	return true;
}

static bool mergeable(uint32_t s)
{
	stash_seg *g = &stash_a->segs[s];
	return g->state == SEG_SEALED && g->run == 1 && !g->pending;
}

/* Frees memory (or slots) in sh. Picks up to MERGE_N victims:
 * - sparse ones (<= 1/4 live, up to one segment's worth together): compaction, every live entry moves forward;
 * - else the oldest: the entries read since they were written (or last moved) move forward, up to one segment's worth,
 *   and the rest is evicted. A moved copy starts unread, so it must be read again to survive the next round (CLOCK).
 * evict_only: the slot table is full, compaction would not free any.
 * A big entry's run is evicted when it is the oldest, unless it was read since: then it goes to the back once.
 * Victims that readers pin die but stay allocated until unpinned; the next call moves on. False if nothing was done. */
static bool merge(stash_shard *sh, bool evict_only)
{
	uint32_t v[MERGE_N], nv = 0;
	size_t budget = 0;
	if (!evict_only) {
		for (uint32_t s = sh->lo; s < sh->hi && nv < MERGE_N; s++) {
			if (mergeable(s) && stash_a->segs[s].live <= seg_size / 4 && budget + stash_a->segs[s].live <= seg_size) {
				budget += stash_a->segs[s].live;
				v[nv++] = s;
			}
		}
		if (nv < 2) {
			nv = 0; /* one sparse segment is just moved, not freed */
		}
	}
	bool lossless = nv > 0;
	if (!lossless) {
		uint32_t oldest_run = UINT32_MAX;
		for (uint32_t s = sh->lo; s < sh->hi; s++) {
			stash_seg *g = &stash_a->segs[s];
			if (g->state != SEG_SEALED || g->pending) {
				continue;
			}
			if (g->run > 1) {
				if (oldest_run == UINT32_MAX || g->born < stash_a->segs[oldest_run].born) {
					oldest_run = s;
				}
				continue;
			}
			if (nv == MERGE_N && g->born >= stash_a->segs[v[MERGE_N - 1]].born) {
				continue;
			}
			uint32_t k = nv < MERGE_N ? nv++ : MERGE_N - 1; /* keep v sorted by born, oldest first */
			for (; k > 0 && stash_a->segs[v[k - 1]].born > g->born; k--) {
				v[k] = v[k - 1];
			}
			v[k] = s;
		}
		if (oldest_run != UINT32_MAX && (!nv || stash_a->segs[oldest_run].born < stash_a->segs[v[0]].born)) {
			stash_entry *e = (stash_entry *) seg_start(oldest_run);
			uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
			if (e->live && f) {
				__atomic_store_n(&e->hot, f >> 1, __ATOMIC_RELAXED);
				stash_a->segs[oldest_run].born = __atomic_load_n(&stash_a->inserts, __ATOMIC_RELAXED);
				return true;
			}
			v[0] = oldest_run;
			nv = 1;
		}
		if (!nv) {
			return false;
		}
		budget = nv > 2 ? seg_size : 0; /* survivors fill at most one segment: frees at least one */
	}
	/* lossy: keep the most-read entries; levels >= cut all fit, level cut-1 fills what is left (first come) */
	uint8_t cut = 1;
	size_t spare = budget;
	if (!lossless && budget) {
		size_t by[STASH_HOT_MAX + 1] = {0};
		for (uint32_t i = 0; i < nv; i++) {
			for (char *p = seg_start(v[i]), *end = p + stash_a->segs[v[i]].used; p < end; p += ((stash_entry *) p)->size) {
				stash_entry *e = (stash_entry *) p;
				if (e->live && !stash_expired(e)) {
					by[__atomic_load_n(&e->hot, __ATOMIC_RELAXED)] += e->size;
				}
			}
		}
		size_t sum = 0;
		for (cut = STASH_HOT_MAX + 1; cut > 1 && sum + by[cut - 1] <= budget; cut--) {
			sum += by[cut - 1];
		}
		spare = budget - sum;
	}
	for (uint32_t i = 0; i < nv && budget; i++) {
		for (char *p = seg_start(v[i]), *end = p + stash_a->segs[v[i]].used; p < end && budget;) {
			stash_entry *e = (stash_entry *) p;
			p += e->size;
			if (!e->live || stash_expired(e)) {
				continue;
			}
			if (!lossless) {
				uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED); /* readers may raise it meanwhile: cut is a hint */
				if (f < cut - 1 || f == 0) {
					continue;
				}
				if (f < cut) {
					if (e->size > spare) {
						continue;
					}
					spare -= e->size;
				}
			}
			if (e->size > budget || !relocate(sh, e)) {
				budget = 0;
				break;
			}
			budget -= e->size;
		}
	}
	bool lost = false;
	for (uint32_t i = 0; i < nv && !lost; i++) {
		for (char *p = seg_start(v[i]), *end = p + stash_a->segs[v[i]].used; p < end && !lost; p += ((stash_entry *) p)->size) {
			lost = ((stash_entry *) p)->live && !stash_expired((stash_entry *) p);
		}
	}
	if (lost) {
		__atomic_add_fetch(&stash_a->expunges, 1, __ATOMIC_RELAXED); /* before the unlinks: a reader that misses then sees it */
	}
	for (uint32_t i = 0; i < nv; i++) {
		for (char *p = seg_start(v[i]), *end = p + stash_a->segs[v[i]].used; p < end;) {
			stash_entry *e = (stash_entry *) p;
			p += e->size;
			if (e->live) {
				if (stash_expired(e)) {
					sh->expired++;
				} else {
					sh->evicted++;
					*ghost_cell(sh, e->h) = ghost_tag(e->h);
				}
				entry_unlink(sh, e);
			}
		}
	}
	return true;
}

/* ---- dead writers ---- */

static stash_entry *wip_entry(uint32_t w) { return (stash_entry *) ((char *) stash_a + ((size_t) w << 3)); }

static stash_shard *seg_shard(uint32_t s)
{
	for (uint32_t i = 0;; i++) {
		if (s < stash_a->shards[i].hi) {
			return &stash_a->shards[i];
		}
	}
}

/* sh locked. Aborts the unfinished entries in sh of writers that died outside the lock. */
static void wip_reap(stash_shard *sh)
{
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		uint32_t w = __atomic_load_n(&stash_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(stash_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o != stash_self && (!o || owner_dead(o))) {
			__atomic_store_n(&stash_a->workers[i].wip, 0, __ATOMIC_RELAXED);
			stash_abort(sh, wip_entry(w));
		}
	}
}

/* A worker slot just claimed from a dead owner: abort the entry it was filling. No lock held. */
static void wip_abort(int slot)
{
	uint32_t w = __atomic_load_n(&stash_a->workers[slot].wip, __ATOMIC_RELAXED);
	if (!w) {
		return;
	}
	stash_shard *sh = seg_shard(stash_seg_of(wip_entry(w)));
	stash_lock(sh);
	if (__atomic_load_n(&stash_a->workers[slot].wip, __ATOMIC_RELAXED) == w) { /* not reaped meanwhile */
		__atomic_store_n(&stash_a->workers[slot].wip, 0, __ATOMIC_RELAXED);
		stash_abort(sh, wip_entry(w));
	}
	stash_unlock(sh);
}

/* sh's lock was just taken from a dead holder. Rebuilds what it may have left half done from the slot table, which only
 * ever changes by single stores: duplicate slots and an odd mseq (a delete's shift), segment states (a run taken or freed
 * halfway, an open segment not yet installed), live/pending counts (an entry allocated, moved or unlinked halfway). */
static void shard_repair(stash_shard *sh, uint64_t dead)
{
	stash_slot *t = tab_of(sh);
	if (sh->mseq & 1) {
		__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELEASE); /* else readers retry every miss forever */
	}
	for (uint32_t i = 0; i <= stash_tab_mask; i++) {
		while (t[i] && slot_of(sh, stash_slot_entry(t[i])) != &t[i]) {
			slot_remove(sh, &t[i]); /* a key the shift copied back but did not yet clear */
		}
	}
	sh->nlive = 0;
	for (uint32_t i = 0; i <= stash_tab_mask; i++) {
		sh->nlive += t[i] != 0;
	}
	for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
		int32_t c = sh->cur[b];
		if (c >= 0 && ((uint32_t) c < sh->lo || (uint32_t) c >= sh->hi || stash_a->segs[c].state != SEG_OPEN)) {
			sh->cur[b] = -1;
		}
	}
	sh->nfree = sh->ndead = 0;
	sh->live_bytes = 0;
	for (uint32_t s = sh->lo; s < sh->hi;) {
		stash_seg *g = &stash_a->segs[s];
		if (g->state == SEG_FREE || g->state == SEG_CONT) { /* CONT outside any run: a run taken or freed halfway */
			uint32_t gen = g->gen + 1;
			memset(g, 0, sizeof *g);
			g->gen = gen;
			sh->nfree++;
			s++;
			continue;
		}
		uint32_t k = g->run ? g->run : 1;
		k = k < sh->hi - s ? k : sh->hi - s;
		for (uint32_t j = s + 1; j < s + k; j++) {
			seg_set(j, SEG_CONT);
		}
		bool installed = false;
		for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
			installed |= sh->cur[b] == (int32_t) s;
		}
		if (g->state == SEG_OPEN && !installed) {
			seg_set(s, SEG_SEALED);
		}
		g->pending = 0;
		if (g->state != SEG_DEAD) {
			g->live = 0;
			for (char *p = seg_start(s), *end = p + g->used; p < end;) {
				stash_entry *e = (stash_entry *) p;
				if (e->size < sizeof *e) {
					break;
				}
				e->live = slot_of(sh, e) != NULL;
				g->live += e->live ? e->size : 0;
				p += e->size;
			}
			sh->live_bytes += g->live;
		}
		s += k;
	}
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) { /* entries being filled outside the lock stay pending */
		uint32_t w = __atomic_load_n(&stash_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(stash_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		if (__atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED) == dead) {
			__atomic_store_n(&stash_a->workers[i].wip, 0, __ATOMIC_RELAXED); /* the holder itself, between relock and commit */
		} else {
			stash_a->segs[stash_seg_of(wip_entry(w))].pending++;
		}
	}
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (stash_a->segs[s].state == SEG_DEAD) {
			sh->ndead++;
		} else {
			seg_check(sh, s);
		}
	}
	sh->exp_off = 0;
	sh->exp_idle = 0;
	reclaim(sh);
}

/* sh locked. Expire, reclaim, merge until `total` bytes fit, or give up. */
stash_entry *stash_alloc(stash_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > (size_t) (sh->hi - sh->lo - 1) << stash_seg_shift) {
		return NULL; /* bigger than the shard's slice */
	}
	expire_step(sh, now, 64);
	stash_entry *e = seg_alloc(sh, total, false, exp, now);
	if (!e) {
		reclaim(sh);
		e = seg_alloc(sh, total, false, exp, now);
	}
	while (!e && expire_step(sh, now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		reclaim(sh);
		e = seg_alloc(sh, total, false, exp, now);
	}
	if (!e && sh->wip_checked != now) {
		sh->wip_checked = now;
		wip_reap(sh); /* segments held back by dead writers' unfinished entries */
		reclaim(sh);
		e = seg_alloc(sh, total, false, exp, now);
	}
	for (uint32_t tries = 2 * (sh->hi - sh->lo); !e && tries && merge(sh, false); tries--) {
		reclaim(sh);
		e = seg_alloc(sh, total, false, exp, now);
	}
	return e;
}

stash_entry *stash_lookup(stash_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	stash_entry *e;
	locate(sh, h, k, klen, &e);
	return e;
}

/* Publish e under its key; add_only fails if a live entry exists. Consumes e either way. Shard stays locked. */
stash_put_result stash_commit(stash_shard *sh, stash_entry *e, bool add_only)
{
	stash_entry *old;
	stash_slot *sp = locate(sh, e->h, stash_key(e), e->klen, &old);
	if (add_only && old && !stash_expired(old)) {
		stash_abort(sh, e);
		return PUT_EXISTS;
	}
	if (!old && sh->nlive >= sh->cap) { /* table full: evict whole segments' worth of entries */
		for (uint32_t tries = 2 * (sh->hi - sh->lo); sh->nlive >= sh->cap && tries && merge(sh, true); tries--) {}
		sp = locate(sh, e->h, stash_key(e), e->klen, &old);
		if (!old && sh->nlive >= sh->cap) {
			stash_abort(sh, e);
			return PUT_FULL;
		}
	}
	if (!old && *ghost_cell(sh, e->h) == ghost_tag(e->h)) {
		*ghost_cell(sh, e->h) = 0;
		e->hot = 2; /* came back after an eviction: survives two merges without a read */
		sh->ghost_hits++;
	}
	e->seq = __atomic_add_fetch(&stash_a->inserts, 1, __ATOMIC_RELAXED);
	sh->nlive += !old;
	entry_birth(sh, e);
	__atomic_store_n(sp, slot_make(e), __ATOMIC_RELEASE);
	if (old) {
		entry_die(sh, old);
	}
	if (sh->hint) {
		reclaim(sh);
	}
	return PUT_OK;
}

bool stash_remove(stash_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	stash_entry *old;
	stash_slot *sp = locate(sh, h, k, klen, &old);
	if (!old) {
		return false;
	}
	bool found = !stash_expired(old);
	slot_remove(sh, sp);
	entry_die(sh, old);
	if (sh->hint) {
		reclaim(sh);
	}
	return found;
}

void stash_clear(void)
{
	for (uint32_t s = 0; s < stash_a->nshards; s++) { /* in order: two clears never deadlock */
		stash_lock(&stash_a->shards[s]);
	}
	for (uint32_t s = 0; s < stash_a->nshards; s++) {
		stash_shard *sh = &stash_a->shards[s];
		stash_slot *t = tab_of(sh);
		for (uint32_t i = 0; i <= stash_tab_mask; i++) {
			stash_slot sl = t[i];
			__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
			if (sl) {
				entry_die(sh, stash_slot_entry(sl));
			}
		}
		sh->nlive = 0;
	}
	__atomic_add_fetch(&stash_a->expunges, 1, __ATOMIC_RELAXED);
	for (uint32_t s = 0; s < stash_a->nshards; s++) {
		wip_reap(&stash_a->shards[s]);
		reclaim(&stash_a->shards[s]);
		stash_unlock(&stash_a->shards[s]);
	}
}

/* ---- leases ---- */

int stash_lease_take(uint64_t h, int32_t slot, time_t now, time_t ttl)
{
	stash_shard *sh = stash_shard_of(h);
	uint64_t dead = 0; /* holder found dead on the first pass: replace it if it still holds */
	for (;;) {
		stash_lock(sh);
		stash_lease *l = NULL, *free = NULL;
		for (int i = 0; i < STASH_LEASES; i++) {
			stash_lease *x = &sh->leases[i];
			if (x->owner && x->until >= now) {
				if (x->h == h) {
					l = x;
					break;
				}
			} else if (!free) {
				free = x;
			}
		}
		int r = STASH_LEASE_TAKEN;
		if (l && l->owner == stash_self && l->slot == slot) {
			r = STASH_LEASE_MINE;
		} else if (l && l->owner != dead) {
			r = STASH_LEASE_HELD;
		} else if (l || free) {
			*(l ? l : free) = (stash_lease) { .h = h, .owner = stash_self, .until = now + ttl, .slot = slot };
		} else {
			r = STASH_LEASE_FULL;
		}
		uint64_t o = r == STASH_LEASE_HELD ? l->owner : 0;
		stash_unlock(sh);
		if (r != STASH_LEASE_HELD || dead || !owner_dead(o)) { /* owner_dead reads /proc: not under the lock */
			return r;
		}
		dead = o;
	}
}

void stash_lease_drop(uint64_t h, int32_t slot)
{
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	for (int i = 0; i < STASH_LEASES; i++) {
		stash_lease *x = &sh->leases[i];
		if (x->h == h && x->owner == stash_self && x->slot == slot) {
			x->owner = 0;
			break;
		}
	}
	stash_unlock(sh);
}

/* ---- scans ---- */

stash_entry *stash_scan_next(stash_ctx *c, uint32_t *sh, uint32_t *i)
{
	for (; *sh < stash_a->nshards; (*sh)++, *i = 0) {
		const stash_slot *t = stash_tabs[*sh];
		while (*i <= stash_tab_mask) {
			const stash_slot *p = &t[(*i)++];
			stash_slot s = __atomic_load_n(p, __ATOMIC_ACQUIRE);
			if (!s) {
				continue;
			}
			stash_entry *e = stash_slot_entry(s);
			if (stash_hold(c, e) && __atomic_load_n(p, __ATOMIC_ACQUIRE) != s) {
				(*i)--; /* re-read this slot */
				continue;
			}
			if (!stash_expired(e)) {
				return e;
			}
		}
	}
	return NULL;
}

/* ---- stats ---- */

void stash_stats(stash_stats_t *st)
{
	memset(st, 0, sizeof *st);
	for (uint32_t i = 0; i < stash_a->nshards; i++) { /* lock-free: a snapshot that may be off by concurrent writes */
		stash_shard *sh = &stash_a->shards[i];
		st->used += __atomic_load_n(&sh->live_bytes, __ATOMIC_RELAXED);
		st->entries += __atomic_load_n(&sh->nlive, __ATOMIC_RELAXED);
		st->evacuated += __atomic_load_n(&sh->evacuated, __ATOMIC_RELAXED);
		st->evicted += __atomic_load_n(&sh->evicted, __ATOMIC_RELAXED);
		st->expired += __atomic_load_n(&sh->expired, __ATOMIC_RELAXED);
		st->ghost_hits += __atomic_load_n(&sh->ghost_hits, __ATOMIC_RELAXED);
		st->free_segments += __atomic_load_n(&sh->nfree, __ATOMIC_RELAXED);
		st->dead_segments += __atomic_load_n(&sh->ndead, __ATOMIC_RELAXED);
		time_t now = time(NULL);
		for (int l = 0; l < STASH_LEASES; l++) {
			st->leases += __atomic_load_n(&sh->leases[l].owner, __ATOMIC_RELAXED) && sh->leases[l].until >= now;
		}
	}
	uint64_t held[STASH_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			held[w] |= __atomic_load_n(&stash_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
		}
		st->hits += __atomic_load_n(&stash_a->workers[i].hits, __ATOMIC_RELAXED);
		st->misses += __atomic_load_n(&stash_a->workers[i].misses, __ATOMIC_RELAXED);
	}
	for (int w = 0; w < STASH_SEG_WORDS; w++) {
		st->pinned_segments += (uint32_t) __builtin_popcountll(held[w]);
	}
	st->segments = stash_a->nsegs;
	st->shards = stash_a->nshards;
	st->slots = (uint64_t) stash_a->nshards * (stash_tab_mask + 1);
	st->expunges = __atomic_load_n(&stash_a->expunges, __ATOMIC_RELAXED);
	st->lock_steals = __atomic_load_n(&stash_a->lock_steals, __ATOMIC_RELAXED);
}

uint32_t stash_free_runs(uint32_t shard, size_t *off, size_t *len, uint32_t max)
{
	const stash_shard *sh = &stash_a->shards[shard];
	uint32_t runs = 0;
	for (uint32_t i = sh->lo, start = 0, n = 0; i <= sh->hi && runs < max; i++) { /* adjacent free segments form one block */
		uint8_t st = i < sh->hi ? __atomic_load_n(&stash_a->segs[i].state, __ATOMIC_RELAXED) : SEG_OPEN;
		bool free_seg = st == SEG_FREE || st == SEG_DEAD;
		if (free_seg && !n++) {
			start = i;
		} else if (!free_seg && n) {
			off[runs] = (size_t) (start - sh->lo) << stash_seg_shift;
			len[runs++] = (size_t) n << stash_seg_shift;
			n = 0;
		}
	}
	return runs;
}

/* ---- setup ---- */

const char STASH_TOO_SMALL[] = "shm_size is too small or entries_hint too large";

const char *stash_arena_create(size_t size, uint64_t hint, size_t *need)
{
	/* shards only where each slice stays >= 8 MB (an entry must fit in one) and holds >= 16 segments */
	unsigned shift = 16;
	while ((size >> shift) > STASH_MAX_SEGS) {
		shift++;
	}
	uint32_t nshards = 1;
	while (nshards < STASH_MAX_SHARDS && size / (nshards * 2) >= ((size_t) 8 << 20) && (size >> shift) / (nshards * 2) >= 16) {
		nshards *= 2;
	}
	/* ~2 slots per hinted entry, and at least one per 256 bytes of arena (3% of it); fixed for the arena's life */
	uint64_t per = 1024;
	while (per * nshards < hint * 2 || per * nshards < size / 256) {
		per <<= 1;
	}
	size_t header = STASH_ALIGN(sizeof(stash_arena)) + (size_t) nshards * per * (sizeof(stash_slot) + sizeof(uint32_t));
	header = (header + 63) & ~(size_t) 63;
	*need = header + ((size_t) 4 << shift);
	if (per > ((uint64_t) 1 << 31) || size < *need) {
		return STASH_TOO_SMALL;
	}
	if (size > ((size_t) 32 << 30)) {
		return "larger than 32G";
	}
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		return "mmap failed";
	}
	stash_a = p; /* zero-filled by the kernel */
	stash_a->size = size;
	stash_a->header_size = header;
	stash_seg_shift = shift;
	seg_size = (size_t) 1 << shift;
	stash_seg_base = (char *) stash_a + header;
	stash_a->nsegs = (uint32_t) ((size - header) >> shift);
	stash_a->nsegs -= stash_a->nsegs % nshards; /* equal slices */
	stash_a->nshards = nshards;
	stash_shard_mask = nshards - 1;
	stash_tab_mask = (uint32_t) per - 1;
	uint32_t slice = stash_a->nsegs / nshards;
	for (uint32_t s = 0; s < nshards; s++) {
		stash_shard *sh = &stash_a->shards[s];
		stash_tabs[s] = (stash_slot *) (stash_a->tables + (size_t) s * per * sizeof(stash_slot));
		stash_ghosts[s] = (uint32_t *) (stash_a->tables + (size_t) nshards * per * sizeof(stash_slot)) + (size_t) s * per;
		sh->lo = s * slice;
		sh->hi = sh->lo + slice;
		sh->nfree = slice;
		sh->exp_next = sh->lo;
		sh->cap = (uint32_t) (per / 4 * 3);
		for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
			sh->cur[b] = -1;
		}
	}
	stash_a->start_time = time(NULL);
	stash_self = make_self();
	return NULL;
}

void stash_arena_destroy(void)
{
	if (stash_a) {
		munmap(stash_a, stash_a->size);
		stash_a = NULL;
	}
}
