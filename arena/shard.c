/* nocopy arena: shards: writes, deletes, updates, making room, and repair after a dead holder. */

#include "shard.h"

ar_shard *ar_shards;
uint32_t ar_shard_mask;
ar_index ar_ix[NOCOPY_MAX_SHARDS];

static uint32_t nshards;
static uint32_t fair_segs; /* nsegs / nshards: what a shard may take back from others, and the largest entry (a division on every write cost store int ~3 ns) */
static uint32_t *ghosts[NOCOPY_MAX_SHARDS];
static nocopy_rebase_fn rebase;

static void repair(ar_shard *sh, uint64_t dead);

void ar_shards_init(ar_shard *shards, uint32_t n, uint32_t slots_per, uint32_t nsegs)
{
	for (uint32_t i = 0; i < n; i++) {
		ar_shard *sh = &shards[i];
		sh->idx = i;
		ar_index_init(&sh->ix, slots_per);
		ar_log_init(&sh->log, (uint8_t) (i + 1), i * (nsegs / n));
	}
}

void ar_shards_adopt(ar_shard *shards, uint32_t n, ar_slot *tables, uint32_t *ghost_tabs, uint32_t slots_per, nocopy_rebase_fn fn)
{
	ar_shards = shards;
	nshards = n;
	ar_shard_mask = n - 1;
	fair_segs = ar_nsegs / n;
	rebase = fn;
	for (uint32_t i = 0; i < n; i++) {
		ar_index_adopt(&ar_ix[i], &shards[i].ix, tables + (size_t) i * slots_per, slots_per);
		ghosts[i] = ghost_tabs + (size_t) i * slots_per;
	}
}

/* ---- locks and entries (shard locked unless noted) ---- */

static void shard_lock(ar_shard *sh)
{
	uint64_t dead;
	if (ar_lock_acquire(&sh->lock, &dead) == AR_STOLEN) {
		repair(sh, dead);
	}
}

static void shard_unlock(ar_shard *sh) { ar_lock_release(&sh->lock); }

static ar_index *ix_of(const ar_shard *sh) { return &ar_ix[sh->idx]; }
static nocopy_entry *slot_entry(ar_slot s) { return ar_at8(ar_slot_off8(s)); }
static ar_slot slot_for(const nocopy_entry *e) { return ar_slot_make(ar_slot_tag(e->h), ar_off8(e)); }

static inline __attribute__((always_inline)) void birth(ar_shard *sh, nocopy_entry *e)
{
	ar_log_born(&sh->log, e);
	ar_policy_born(e);
}

static inline __attribute__((always_inline)) void die(ar_shard *sh, nocopy_entry *e)
{
	ar_policy_died(e);
	ar_log_died(&sh->log, e);
}

/* The slot of the key's live entry (*found), or the empty slot ending its run. */
static inline __attribute__((always_inline)) ar_slot *locate(ar_shard *sh, uint64_t h, const char *k, uint32_t klen, nocopy_entry **found)
{
	ar_probe p;
	ar_slot s;
	ar_probe_start_locked(&p, ix_of(sh), h); /* never full: nlive <= cap < slots */
	while ((s = ar_probe_next_locked(&p))) {
		nocopy_entry *e = slot_entry(s);
		if (e->h == h && e->klen == klen && nocopy_key_eq(nocopy_key(e), k, klen)) {
			*found = e;
			return ar_probe_slot(&p);
		}
	}
	*found = NULL;
	return ar_probe_slot(&p);
}

static void unlink_entry(nocopy_entry *e, void *arg)
{
	ar_shard *sh = arg;
	ar_slot *sp = ar_index_find(ix_of(sh), slot_for(e));
	if (sp) {
		ar_index_remove(ix_of(sh), sp);
	}
	die(sh, e);
}

/* ---- dead writers ---- */

/* Before unlocking to fill e outside the lock: record it, so the entry of a writer that dies meanwhile is rolled back. NULL after the commit. */
static void wip_set(int slot, const nocopy_entry *e) { __atomic_store_n(&ar_worker_at((uint32_t) slot)->wip, e ? ar_off8(e) : 0, __ATOMIC_RELAXED); }
static void wip_clear(uint32_t slot) { __atomic_store_n(&ar_worker_at(slot)->wip, 0, __ATOMIC_RELAXED); }

/* NULL: in the pool. A wip entry's segment stays with its shard: the entry keeps it pending. */
static ar_shard *seg_shard(uint32_t s)
{
	uint8_t o = ar_pool_tenant(s);
	return o ? &ar_shards[o - 1] : NULL;
}

/* The entry worker slot i is filling in sh, or NULL. */
static nocopy_entry *wip_in(uint32_t i, const ar_shard *sh)
{
	uint32_t w = __atomic_load_n(&ar_worker_at(i)->wip, __ATOMIC_RELAXED);
	return w && seg_shard(ar_seg_of(ar_at8(w))) == sh ? ar_at8(w) : NULL;
}

/* Aborts the unfinished entries in sh of writers that died outside the lock. */
static void wip_reap(ar_shard *sh)
{
	uint32_t nw = ar_worker_high();
	for (uint32_t i = 0; i < nw; i++) {
		nocopy_entry *e = wip_in(i, sh);
		if (e && !ar_worker_live(i)) {
			wip_clear(i);
			ar_log_abort(&sh->log, e);
		}
	}
}

void ar_shard_wip_abort(int slot)
{
	uint32_t w = __atomic_load_n(&ar_worker_at((uint32_t) slot)->wip, __ATOMIC_RELAXED);
	if (!w) {
		return;
	}
	ar_shard *sh = seg_shard(ar_seg_of(ar_at8(w)));
	if (!sh) {
		wip_clear((uint32_t) slot);
		return;
	}
	shard_lock(sh);
	if (__atomic_load_n(&ar_worker_at((uint32_t) slot)->wip, __ATOMIC_RELAXED) == w) { /* not reaped meanwhile */
		wip_clear((uint32_t) slot);
		ar_log_abort(&sh->log, ar_at8(w));
	}
	shard_unlock(sh);
}

static bool linked(const nocopy_entry *e, void *arg) { return ar_index_find(ix_of(arg), slot_for(e)) != NULL; }

/* sh's lock was just taken from a dead holder. The slot table only ever changes by single stores, so it is the truth:
 * the index repairs itself, the log rebuilds its segments from it, and entries being filled outside the lock stay pending. */
static void repair(ar_shard *sh, uint64_t dead)
{
	ar_index_repair(ix_of(sh));
	ar_log_rebuild(&sh->log, linked, sh);
	uint32_t nw = ar_worker_high();
	for (uint32_t i = 0; i < nw; i++) {
		nocopy_entry *e = wip_in(i, sh);
		if (!e) {
			continue;
		}
		if (__atomic_load_n(&ar_worker_at(i)->owner, __ATOMIC_RELAXED) == dead) {
			wip_clear(i); /* the holder itself, between relock and commit */
		} else {
			ar_log_hold(e);
		}
	}
	ar_log_settle(&sh->log);
}

/* ---- making room ---- */

/* Copies old forward into its segment's TTL bucket and repoints its slot. False if there is no room. */
static bool relocate(nocopy_entry *old, uint8_t hot, void *arg)
{
	ar_shard *sh = arg;
	nocopy_entry *n = ar_log_alloc(&sh->log, old->size, ar_segs[ar_seg_of(old)].bucket, old->expires, sh->last_seq, true);
	if (!n) {
		return false;
	}
	memcpy(n + 1, old + 1, old->size - sizeof *old);
	n->h = old->h;
	n->expires = old->expires;
	n->created = old->created;
	n->seq = old->seq;
	n->klen = old->klen;
	n->koff = old->koff;
	n->flags = old->flags;
	n->user = old->user;
	n->hot = hot;
	ar_seg_deps_or(ar_seg_of(n), rebase(n, old));
	ar_slot *sp = ar_index_find(ix_of(sh), slot_for(old));
	if (sp) {
		birth(sh, n);
		ar_index_put(ix_of(sh), sp, slot_for(n));
	} else {
		ar_log_abort(&sh->log, n);
	}
	die(sh, old);
	return true;
}

static bool merge(ar_shard *sh, bool evict_only)
{
	ar_merge m = {.log = &sh->log, .pol = &sh->pol, .ghosts = ghosts[sh->idx], .last_seq = sh->last_seq,
		.relocate = relocate, .unlink = unlink_entry, .arg = sh};
	return ar_policy_merge(&m, evict_only);
}

static bool expire_step(ar_shard *sh, time_t now, int budget) { return ar_log_expire_step(&sh->log, now, budget, unlink_entry, sh); }

static void lock_all(void)
{
	for (uint32_t i = 0; i < nshards; i++) { /* in index order: two holders of all never deadlock */
		shard_lock(&ar_shards[i]);
	}
}

static void unlock_all(void)
{
	for (uint32_t i = 0; i < nshards; i++) {
		shard_unlock(&ar_shards[i]);
	}
}

bool ar_shards_deps(uint8_t bits)
{
	lock_all();
	bool used = ar_pool_any_deps(bits);
	unlock_all();
	return used;
}

/* sh locked, below its share, and the pool is dry: the shard holding the most evicts once to give segments back.
 * Drops sh's lock meanwhile (one lock at a time). false: nobody holds more than its share. */
static __attribute__((cold, noinline)) bool rebalance(ar_shard *sh)
{
	uint32_t most = fair_segs;
	ar_shard *big = NULL;
	for (uint32_t i = 0; i < nshards; i++) {
		uint32_t n = __atomic_load_n(&ar_shards[i].log.nown, __ATOMIC_RELAXED);
		if (n > most) {
			most = n;
			big = &ar_shards[i];
		}
	}
	if (!big) {
		return false;
	}
	shard_unlock(sh);
	shard_lock(big);
	ar_log_reclaim(&big->log);
	if (big->log.nown > fair_segs && merge(big, false)) {
		ar_log_reclaim(&big->log);
	}
	shard_unlock(big);
	shard_lock(sh);
	return true;
}

static nocopy_entry *reclaim_alloc(ar_shard *sh, size_t total, int b, time_t exp)
{
	ar_log_reclaim(&sh->log);
	return ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
}

/* A block of total bytes, freeing room as needed, cheapest first: expired entries, dead segments, dead writers' unfinished
 * entries, segments other shards hold beyond their share, merges. NULL: no room.
 * May drop and retake the lock (rebalance): whatever the caller read of the shard before calling is stale afterwards. */
static nocopy_entry *alloc_relocking(ar_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > (size_t) (fair_segs - 1) << ar_seg_shift) {
		return NULL; /* bigger than a shard's share */
	}
	int b = ar_log_bucket(exp ? exp - now : 0);
	if (sh->log.exp_idle != now) { /* the common case: nothing left to expire this second */
		expire_step(sh, now, 64);
	}
	nocopy_entry *e = ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
	if (!e) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	while (!e && expire_step(sh, now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		e = reclaim_alloc(sh, total, b, exp);
	}
	if (!e && sh->wip_checked != now) {
		sh->wip_checked = now;
		wip_reap(sh); /* segments held back by dead writers' unfinished entries */
		e = reclaim_alloc(sh, total, b, exp);
	}
	for (uint32_t tries = 2 * fair_segs; !e && tries && sh->log.nown < fair_segs - fair_segs / 16 && rebalance(sh); tries--) {
		e = ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
	}
	for (uint32_t tries = 2 * fair_segs; !e && tries && merge(sh, false); tries--) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	return e;
}

/* The slot table is full: evict whole segments' worth of entries until a slot is free. False if none could be. */
static bool slot_room(ar_shard *sh)
{
	for (uint32_t tries = 2 * fair_segs; ar_index_full(ix_of(sh)) && tries && merge(sh, true); tries--) {}
	return !ar_index_full(ix_of(sh));
}

/* ---- writes ---- */

/* Publishes e under its key; add_only fails if a live entry exists. Consumes e either way. */
static nocopy_put_result commit(ar_shard *sh, nocopy_entry *e, bool add_only)
{
	nocopy_entry *old;
	ar_slot *sp = locate(sh, e->h, nocopy_key(e), e->klen, &old);
	if (add_only && old && !nocopy_expired(old)) {
		ar_log_abort(&sh->log, e);
		return NOCOPY_PUT_EXISTS;
	}
	if (!old && ar_index_full(ix_of(sh))) {
		bool room = slot_room(sh);
		sp = locate(sh, e->h, nocopy_key(e), e->klen, &old); /* the merge moved entries and slots */
		if (!old && !room) {
			ar_log_abort(&sh->log, e);
			return NOCOPY_PUT_FULL;
		}
	}
	if (!old) {
		e->hot = ar_policy_admit(&sh->pol, ghosts[sh->idx], e->h);
	}
	e->seq = sh->last_seq = e->seq > sh->last_seq ? e->seq : sh->last_seq + (1u << NOCOPY_SEQ_SHARD_BITS); /* keeps the shard bits */
	birth(sh, e);
	ar_index_put(ix_of(sh), sp, slot_for(e));
	if (old) {
		die(sh, old);
	}
	if (sh->log.hint) {
		ar_log_reclaim(&sh->log);
	}
	return NOCOPY_PUT_OK;
}

nocopy_entry *nocopy_put_begin(nocopy_put *p, nocopy_ctx *c, const nocopy_put_req *q)
{
	if (!c->pin_refs) {
		ar_pins_drop_transient(&c->pins); /* our own idle pins would block reclaim; only mixed read/write loops pay */
	}
	ar_shard *sh = &ar_shards[ar_shard_idx(q->h)];
	shard_lock(sh);
	struct timespec ts;
	nocopy_clock(&ts); /* inside the lock: measured faster under write contention than before it */
	time_t now = nocopy_now_of(&ts), exp = q->expires ? q->expires : nocopy_expiry(now, q->ttl);
	nocopy_entry *e = alloc_relocking(sh, q->size, exp, now); /* first: nothing above relies on shard state */
	if (!e) {
		shard_unlock(sh);
		return NULL;
	}
	e->h = q->h;
	e->klen = q->klen;
	e->koff = q->koff;
	e->expires = exp;
	e->created = (uint32_t) now;
	e->flags = q->flags;
	e->user = q->user;
	if (q->deps) { /* before the writer drops its atom view: a rotation relies on it */
		ar_seg_deps_or(ar_seg_of(e), q->deps);
	}
	e->seq = ((uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u) << NOCOPY_SEQ_SHARD_BITS | sh->idx; /* commit makes it unique */
	*p = (nocopy_put) {.c = c, .sh = sh, .e = e, .locked = q->size <= NOCOPY_INLINE_PUT};
	if (!p->locked) {
		wip_set(c->pins.slot, e);
		shard_unlock(sh);
	}
	memcpy((char *) e + q->koff, q->key, q->klen);
	return e;
}

nocopy_put_result nocopy_put_commit(nocopy_put *p, bool add_only)
{
	if (!p->locked) {
		shard_lock(p->sh);
	}
	nocopy_put_result r = commit(p->sh, p->e, add_only);
	if (!p->locked) {
		wip_set(p->c->pins.slot, NULL);
	}
	shard_unlock(p->sh);
	return r;
}

void nocopy_put_abort(nocopy_put *p)
{
	if (!p->locked) {
		shard_lock(p->sh);
		wip_set(p->c->pins.slot, NULL);
	}
	ar_log_abort(&p->sh->log, p->e);
	shard_unlock(p->sh);
}

bool nocopy_delete(uint64_t h, const char *k, uint32_t klen)
{
	ar_shard *sh = &ar_shards[ar_shard_idx(h)];
	shard_lock(sh);
	nocopy_entry *old;
	ar_slot *sp = locate(sh, h, k, klen, &old);
	bool found = old && !nocopy_expired(old);
	if (old) {
		ar_index_remove(ix_of(sh), sp);
		die(sh, old);
		if (sh->log.hint) {
			ar_log_reclaim(&sh->log);
		}
	}
	shard_unlock(sh);
	return found;
}

/* Out of line: locate inlined into nocopy_update made inc 10% slower (ab write). */
static __attribute__((noinline)) nocopy_entry *lookup(ar_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	nocopy_entry *e;
	locate(sh, h, k, klen, &e);
	return e;
}

int nocopy_update(uint64_t h, const char *k, uint32_t klen, int (*fn)(nocopy_entry *e, void *arg), void *arg)
{
	ar_shard *sh = &ar_shards[ar_shard_idx(h)];
	shard_lock(sh);
	nocopy_entry *e = lookup(sh, h, k, klen);
	int r = e && !nocopy_expired(e) ? fn(e, arg) : NOCOPY_GONE;
	shard_unlock(sh);
	return r;
}

static void cleared(ar_slot s, void *arg) { die(arg, slot_entry(s)); }

void nocopy_clear(void)
{
	lock_all();
	for (uint32_t i = 0; i < nshards; i++) {
		ar_index_clear(&ar_ix[i], cleared, &ar_shards[i]);
	}
	ar_policy_count_expunge();
	for (uint32_t i = 0; i < nshards; i++) {
		ar_shard *sh = &ar_shards[i];
		wip_reap(sh);
		ar_log_seal_all(&sh->log); /* so they die and go back to the pool (with their deps) */
		ar_log_reclaim(&sh->log);
		shard_unlock(sh);
	}
}

void ar_shards_stat(ar_shards_stats *st)
{
	memset(st, 0, sizeof *st);
	for (uint32_t i = 0; i < nshards; i++) {
		ar_shard *sh = &ar_shards[i];
		st->used += __atomic_load_n(&sh->log.live_bytes, __ATOMIC_RELAXED);
		st->entries += ar_index_nlive(&ar_ix[i]);
		st->evacuated += __atomic_load_n(&sh->pol.evacuated, __ATOMIC_RELAXED);
		st->evicted += __atomic_load_n(&sh->pol.evicted, __ATOMIC_RELAXED);
		st->expired += __atomic_load_n(&sh->log.expired, __ATOMIC_RELAXED);
		st->ghost_hits += __atomic_load_n(&sh->pol.ghost_hits, __ATOMIC_RELAXED);
		st->dead_segments += __atomic_load_n(&sh->log.ndead, __ATOMIC_RELAXED);
	}
}
