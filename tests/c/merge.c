/* Compaction loses nothing: a third of the arena live, spread over every TTL bucket, overwritten until its segments have been
 * compacted many times over. Survivors go to their own bucket's open segment, so a round over several buckets needs more
 * than the one segment kept free for merges: the victims it has emptied must give theirs back meanwhile. */
#include "../../arena/nocopy_arena.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define KEYS 500
#define PUTS 300000

static time_t fake_now(void) { return 100000; } /* nothing expires */

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) e, (void) from;
	return 0;
}

static uint64_t hash(const char *k)
{
	uint64_t h = 5381;
	while (*k) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
}

int main(void)
{
	static const time_t ttls[NOCOPY_TTL_BUCKETS] = {0, 3, 30, 300, 3000, 30000, 300000};
	static uint32_t version[KEYS];
	char name[48], k[24];
	snprintf(name, sizeof name, "/nocopy-merge-%d", (int) getpid());
	nocopy_named out;
	nocopy_config cfg = {.name = name, .size = 4u << 20, .hint = 1000, .abi = 1, .rebase = rebase};
	nocopy_err err = nocopy_arena_open(&cfg, &out);
	assert(err.code == NOCOPY_ERR_NONE);
	nocopy_set_clock(fake_now);
	nocopy_ctx c;
	nocopy_ctx_init(&c);
	assert(nocopy_claim(&c));

	uint64_t r = 88172645463325252ULL;
	for (uint32_t n = 1; n <= PUTS; n++) {
		r ^= r << 13, r ^= r >> 7, r ^= r << 17;
		int i = (int) (r % KEYS);
		size_t klen = (size_t) snprintf(k, sizeof k, "k%d", i);
		nocopy_put_req q = {.h = hash(k), .key = k, .klen = (uint32_t) klen, .koff = HDR, .ttl = ttls[i % NOCOPY_TTL_BUCKETS],
			.size = HDR + 16 + NOCOPY_ALIGN(1000 + (r >> 40) % 4096)}; /* 1-5 KB: ~1.5 of the arena's 3.4 MB of segments at most */
		nocopy_put p;
		nocopy_entry *e = nocopy_put_begin(&p, &c, &q);
		assert(e);
		memcpy((char *) e + HDR + 8, &n, sizeof n);
		assert(nocopy_put_commit(&p, false) == NOCOPY_PUT_OK);
		version[i] = n;
	}

	const char *why = nocopy_arena_check();
	if (why) {
		printf("merge: arena: %s\n", why);
		return 1;
	}
	nocopy_stats_t st;
	nocopy_stats(&st);
	assert(st.evacuated); /* it did compact */
	if (st.evicted || st.expunges) {
		printf("merge: %llu live entries evicted in %llu rounds\n", (unsigned long long) st.evicted, (unsigned long long) st.expunges);
		return 1;
	}
	nocopy_borrow_begin(&c);
	for (int i = 0; i < KEYS; i++) {
		size_t klen = (size_t) snprintf(k, sizeof k, "k%d", i);
		nocopy_entry *e = nocopy_find(&c, hash(k), k, (uint32_t) klen);
		uint32_t n = 0;
		if (version[i]) {
			assert(e);
			memcpy(&n, (const char *) e + HDR + 8, sizeof n);
		}
		assert(n == version[i]);
	}
	nocopy_borrow_end(&c);
	const char *bad = ar_shards_verify();
	if (bad) {
		printf("merge: %s\n", bad);
		return 1;
	}
	printf("merge: ok (%llu MB moved, nothing evicted)\n", (unsigned long long) (st.evacuated >> 20));
	nocopy_release(&c);
	nocopy_arena_close();
	shm_unlink(out.name);
	return 0;
}
