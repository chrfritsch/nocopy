/* nocopy arena: shards: locks, writes, deletes, updates, clear, and repair after a dead holder. Making room: room.c. */

#include "shard_int.h"
#include <stdio.h>

ar_shard *ar_shards;
uint32_t ar_shard_mask;
ar_index ar_ix[NOCOPY_MAX_SHARDS];

static uint32_t nshards;

static void repair(ar_shard *sh);

void ar_shards_init(ar_shard *shards, uint32_t n, uint32_t slots_per, uint32_t nsegs)
{
	for (uint32_t i = 0; i < n; i++) {
		ar_shard *sh = &shards[i];
		sh->idx = i;
		ar_index_init(&sh->ix, slots_per);
		ar_log_init(&sh->log, (uint8_t) (i + 1), i * (nsegs / n));
	}
}

void ar_shards_adopt(ar_shard *shards, uint32_t n, ar_slot *tables, uint16_t *ghost_tabs, uint32_t slots_per, uint64_t *expunges, nocopy_rebase_fn fn)
{
	ar_shards = shards;
	nshards = n;
	ar_shard_mask = n - 1;
	for (uint32_t i = 0; i < n; i++) {
		ar_index_adopt(&ar_ix[i], &shards[i].ix, tables + (size_t) i * slots_per, slots_per);
	}
	ar_room_adopt(n, ghost_tabs, slots_per, expunges, fn);
}

/* ---- locks and entries (shard locked unless noted) ---- */

/* Lock-order checks (test and debug builds): one shard lock at a time, or all of them in index order (lock_all). */
#if AR_CHECKS
static __thread uint32_t held; /* shard locks this thread holds */
static __thread bool all;

static void note_locking_all(void)
{
	AR_ASSERT(!held);
	all = true;
}

static void note_locking(const ar_shard *sh)
{
	AR_ASSERT(!held || (all && sh->idx == held));
	held++;
}

static void note_unlocking(void)
{
	AR_ASSERT(held);
	all = --held && all;
}

void ar_shards_forget_held(void)
{
	held = 0;
	all = false;
}
#else
static void note_locking_all(void) {}
static void note_locking(const ar_shard *sh) { (void) sh; }
static void note_unlocking(void) {}
void ar_shards_forget_held(void) {}
#endif

void ar_shard_lock(ar_shard *sh)
{
	uint64_t dead;
	note_locking(sh);
	if (ar_lock_acquire(&sh->lock, &dead) == AR_STOLEN) {
		repair(sh);
	}
}

void ar_shard_unlock(ar_shard *sh)
{
	note_unlocking();
	ar_lock_release(&sh->lock);
}

/* A reader stuck behind an odd mseq: its delete's holder may have died mid-shift; taking the lock steals and repairs. */
__attribute__((cold, noinline)) void ar_shard_unstick(uint32_t i)
{
	ar_shard_lock(&ar_shards[i]);
	ar_shard_unlock(&ar_shards[i]);
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

/* ---- dead holders ---- */

static bool linked(const nocopy_entry *e, void *arg) { return ar_index_find(ix_of(arg), slot_for(e)) != NULL; }

/* sh's lock was just taken from a dead holder. The slot table only ever changes by single stores, so it is the truth:
 * the index repairs itself, the log rebuilds its segments from it (an entry the holder was filling is unlinked: dead bytes). */
static void repair(ar_shard *sh)
{
	ar_index_repair(ix_of(sh));
	ar_log_rebuild(&sh->log, linked, sh);
	ar_log_settle(&sh->log);
}

static void lock_all(void)
{
	note_locking_all();
	for (uint32_t i = 0; i < nshards; i++) { /* in index order: two holders of all never deadlock */
		ar_shard_lock(&ar_shards[i]);
	}
}

static void unlock_all(void)
{
	for (uint32_t i = 0; i < nshards; i++) {
		ar_shard_unlock(&ar_shards[i]);
	}
}

/* Every shard locked: open segments with nothing left in them (no live or pending entry) that no live worker pins hold no
 * dependency; their bits would otherwise stay until a write fills them (a rarely used TTL bucket: never). */
static void drop_idle_deps(uint8_t bits)
{
	uint64_t idle[NOCOPY_SEG_WORDS] = {0}, pinned[NOCOPY_SEG_WORDS];
	bool any = false;
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		const ar_seg *g = &ar_segs[s];
		if ((ar_seg_deps(s) & bits) && g->state == SEG_OPEN && !g->live && !g->pending) {
			ar_segs_add(idle, s);
			any = true;
		}
	}
	if (!any) {
		return;
	}
	ar_pool_pinned(idle, pinned, nocopy_now()); /* a reader may still hold a dead entry's pointers into the atoms */
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		if (ar_segs_has(idle, s) && !ar_segs_has(pinned, s)) {
			ar_seg_deps_drop(s, bits);
		}
	}
}

bool ar_shards_still_depend(uint8_t bits)
{
	lock_all();
	for (uint32_t i = 0; i < nshards; i++) {
		ar_log_reclaim(&ar_shards[i].log); /* dead segments no longer pinned: a shard nobody writes to would keep them */
	}
	drop_idle_deps(bits);
	bool used = ar_pool_any_deps(bits);
	unlock_all();
	return used;
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
		bool room = ar_room_slot(sh);
		sp = locate(sh, e->h, nocopy_key(e), e->klen, &old); /* the merge moved entries and slots */
		if (!old && !room) {
			ar_log_abort(&sh->log, e);
			return NOCOPY_PUT_FULL;
		}
	}
	if (!old) {
		e->hot = ar_room_admit(sh, e->h);
	}
	e->seq = sh->last_seq = e->seq > sh->last_seq ? e->seq : sh->last_seq + (1u << NOCOPY_SEQ_SHARD_BITS); /* keeps the shard bits */
	birth(sh, e);
	AR_HOOK("commit.born");
	ar_index_put(ix_of(sh), sp, slot_for(e));
	AR_HOOK("commit.linked");
	if (old) {
		die(sh, old);
	}
	AR_HOOK("commit.done");
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
	ar_shard_lock(sh);
	struct timespec ts;
	nocopy_clock(&ts); /* inside the lock: measured faster under write contention than before it */
	time_t now = nocopy_now_of(&ts), exp = q->expires ? q->expires : nocopy_expiry(now, q->ttl);
	exp = exp > UINT32_MAX ? UINT32_MAX : exp;
	nocopy_entry *e = ar_room_alloc(sh, q->size, exp, now); /* first: nothing above relies on shard state */
	if (!e) {
		ar_shard_unlock(sh);
		return NULL;
	}
	AR_HOOK("put.alloc");
	e->h = q->h;
	e->klen = q->klen;
	e->koff = q->koff;
	e->expires = (uint32_t) exp;
	e->created = (uint32_t) now;
	e->flags = q->flags;
	e->user = q->user;
	if (q->deps) { /* before the writer drops its atom view: a rotation relies on it */
		ar_seg_deps_or(ar_seg_of(e), q->deps);
	}
	e->seq = ((uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u) << NOCOPY_SEQ_SHARD_BITS | sh->idx; /* commit makes it unique */
	*p = (nocopy_put) {.sh = sh, .e = e};
	memcpy((char *) e + q->koff, q->key, q->klen);
	AR_HOOK("put.fill");
	return e;
}

nocopy_put_result nocopy_put_commit(nocopy_put *p, bool add_only)
{
	nocopy_put_result r = commit(p->sh, p->e, add_only);
	ar_shard_unlock(p->sh);
	return r;
}

bool nocopy_delete(uint64_t h, const char *k, uint32_t klen)
{
	ar_shard *sh = &ar_shards[ar_shard_idx(h)];
	ar_shard_lock(sh);
	nocopy_entry *old;
	ar_slot *sp = locate(sh, h, k, klen, &old);
	bool found = old && !nocopy_expired(old);
	if (old) {
		ar_index_remove(ix_of(sh), sp);
		AR_HOOK("delete.unlinked");
		die(sh, old);
		AR_HOOK("delete.died");
		if (sh->log.hint) {
			ar_log_reclaim(&sh->log);
		}
	}
	ar_shard_unlock(sh);
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
	ar_shard_lock(sh);
	nocopy_entry *e = lookup(sh, h, k, klen);
	int r = e && !nocopy_expired(e) ? fn(e, arg) : NOCOPY_GONE;
	ar_shard_unlock(sh);
	return r;
}

static void cleared(ar_slot s, void *arg) { die(arg, slot_entry(s)); }

void nocopy_clear(void)
{
	lock_all();
	for (uint32_t i = 0; i < nshards; i++) {
		ar_index_clear(&ar_ix[i], cleared, &ar_shards[i]);
	}
	ar_room_count_expunge();
	AR_HOOK("clear.emptied");
	for (uint32_t i = 0; i < nshards; i++) {
		ar_shard *sh = &ar_shards[i];
		ar_log_seal_all(&sh->log); /* so they die and go back to the pool (with their deps) */
		ar_log_reclaim(&sh->log);
		ar_shard_unlock(sh);
		AR_HOOK("clear.unlocked");
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

/* ---- invariants (tests) ---- */

static char why[200];
#define BROKEN(...) do { snprintf(why, sizeof why, __VA_ARGS__); return why; } while (0)

/* Each slot: a live entry of this shard, in a segment it holds, that a probe for its key reaches first. */
static const char *verify_index(ar_shard *sh)
{
	ar_index *ix = ix_of(sh);
	uint32_t n = 0, i = sh->idx;
	if (ix->hdr->mseq & 1) {
		BROKEN("shard %u: mseq odd with no delete running", i);
	}
	for (uint32_t j = 0; j <= ix->mask; j++) {
		ar_slot s = ix->t[j];
		if (!s) {
			continue;
		}
		n++;
		nocopy_entry *e = slot_entry(s);
		if ((char *) e < ar_seg_base || (char *) e >= ar_base + ar_size) {
			BROKEN("shard %u slot %u: points outside the segments", i, j);
		}
		uint32_t g = ar_seg_of(e);
		if (!ar_log_owns(&sh->log, g) || ar_pool_tenant(g) != sh->log.tenant) {
			BROKEN("shard %u slot %u: entry in segment %u, which the shard does not hold", i, j, g);
		}
		if (ar_segs[g].state != SEG_OPEN && ar_segs[g].state != SEG_SEALED) {
			BROKEN("shard %u slot %u: entry in segment %u of state %u", i, j, g, ar_segs[g].state);
		}
		if (!e->live || (uint32_t) (s >> 32) != ar_slot_tag(e->h) || ar_shard_idx(e->h) != i) {
			BROKEN("shard %u slot %u: entry not live, or not this slot's tag or shard", i, j);
		}
		if ((char *) e + nocopy_entry_size(e) > ar_seg_start(g) + ar_segs[g].used || nocopy_entry_size(e) < sizeof *e + e->klen) {
			BROKEN("shard %u slot %u: entry size out of its segment", i, j);
		}
		nocopy_entry *f;
		if (locate(sh, e->h, nocopy_key(e), e->klen, &f) != &ix->t[j] || f != e) {
			BROKEN("shard %u slot %u: its key is not reached here first (duplicate, or past an empty slot)", i, j);
		}
	}
	if (n != ix->hdr->nlive || n > ix->hdr->cap) {
		BROKEN("shard %u: %u slots used, nlive %u, cap %u", i, n, ix->hdr->nlive, ix->hdr->cap);
	}
	return NULL;
}

/* s's entries end exactly at used, and live counts exactly the linked ones. */
static const char *verify_entries(ar_shard *sh, uint32_t s)
{
	const ar_seg *g = &ar_segs[s];
	size_t sum = 0;
	AR_SEG_FOREACH(s, e) {
		size_t off = (size_t) ((char *) e - ar_seg_start(s));
		if (nocopy_entry_size(e) < sizeof *e || off + nocopy_entry_size(e) > g->used) {
			BROKEN("shard %u segment %u: an entry at %zu does not end inside used", sh->idx, s, off);
		}
		if (e->live) {
			if (!ar_index_find(ix_of(sh), slot_for(e))) {
				BROKEN("shard %u segment %u: a live entry at %zu no slot holds", sh->idx, s, off);
			}
			sum += nocopy_entry_size(e);
		}
	}
	if (sum != g->live) {
		BROKEN("shard %u segment %u: live %zu, its linked entries %zu", sh->idx, s, g->live, sum);
	}
	return NULL;
}

/* Each bucket's open segment, if any, is the shard's, open, and of that bucket. */
static const char *verify_open_segments(const ar_shard *sh)
{
	const ar_log *lg = &sh->log;
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		int32_t c = lg->cur[b];
		if (c >= 0 && (!ar_log_owns(lg, (uint32_t) c) || ar_segs[c].state != SEG_OPEN || ar_segs[c].bucket != b)) {
			BROKEN("shard %u: bucket %d's open segment %d is not open, its own or of that bucket", sh->idx, b, c);
		}
	}
	return NULL;
}

/* Each segment the pool gives the shard: in its index, set up, its entries walked to exactly `used`, live bytes those of linked entries. */
static const char *verify_log(ar_shard *sh)
{
	ar_log *lg = &sh->log;
	uint32_t i = sh->idx, own = 0, ndead = 0, cont = 0;
	uint64_t live = 0;
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		bool mine = ar_pool_tenant(s) == lg->tenant;
		if (mine != ar_log_owns(lg, s)) {
			BROKEN("shard %u segment %u: owner[] and the shard's index disagree", i, s);
		}
		ar_seg *g = &ar_segs[s];
		if (!mine) {
			if (cont) {
				BROKEN("shard %u segment %u: a run's segment it does not hold", i, s);
			}
			continue;
		}
		own++;
		if (g->pending) {
			BROKEN("shard %u segment %u: %u entries pending with the lock free", i, s, g->pending);
		}
		if (cont) {
			cont--;
			if (g->state != SEG_CONT) {
				BROKEN("shard %u segment %u: inside a run but state %u", i, s, g->state);
			}
			continue;
		}
		if (g->state == SEG_FREE || g->state == SEG_CONT || !g->run || g->used > (size_t) g->run << ar_seg_shift || s + g->run > ar_nsegs) {
			BROKEN("shard %u segment %u: state %u, run %u, used %zu", i, s, g->state, g->run, g->used);
		}
		if (g->state == SEG_OPEN && (g->run != 1 || lg->cur[g->bucket] != (int32_t) s)) {
			BROKEN("shard %u segment %u: open but not its bucket's open segment", i, s);
		}
		cont = g->run - 1;
		if (g->state == SEG_DEAD) {
			ndead++;
			if (g->live) {
				BROKEN("shard %u segment %u: dead with live bytes", i, s);
			}
			continue;
		}
		const char *bad = verify_entries(sh, s);
		if (bad) {
			return bad;
		}
		if (g->state == SEG_SEALED && !g->live) {
			BROKEN("shard %u segment %u: sealed and empty but not dead (ar_log_check)", i, s);
		}
		live += g->live;
	}
	if (cont || own != lg->nown || ndead != lg->ndead || live != lg->live_bytes) {
		BROKEN("shard %u: run cut short, or nown %u/%u, ndead %u/%u, live %llu/%llu", i, lg->nown, own, lg->ndead, ndead,
			(unsigned long long) lg->live_bytes, (unsigned long long) live);
	}
	return verify_open_segments(sh);
}

/* Each segment no shard holds is zeroed; each held one belongs to a shard that exists. */
static const char *verify_pool(void)
{
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		uint8_t t = ar_pool_tenant(s);
		if (t > nshards || (!t && (ar_segs[s].state != SEG_FREE || ar_segs[s].used || ar_segs[s].live))) {
			BROKEN("segment %u: tenant %u, state %u: a free segment not zeroed, or no such shard", s, t, ar_segs[s].state);
		}
	}
	return NULL;
}

static const char *verify_shard(ar_shard *sh)
{
	const char *bad = verify_index(sh);
	return bad ? bad : verify_log(sh);
}

const char *ar_shards_verify(void)
{
	const char *bad = NULL;
	lock_all();
	for (uint32_t i = 0; i < nshards && !bad; i++) {
		bad = verify_shard(&ar_shards[i]);
	}
	bad = bad ? bad : verify_pool();
	unlock_all();
	return bad;
}
