/* libFuzzer target for nocopy_resp.h: run by tests/c/fuzz.sh (./dev fuzz). */
#include "../../nocopy_resp.h"
#include <assert.h>

/* Like vk_apply: children read from body, every string touched. */
static const char *walk(const char *p, const char *end)
{
	vk_reply r;
	const char *q = resp_parse(p, end, &r);
	if (!q) {
		return NULL;
	}
	volatile char sink = 0;
	for (size_t i = 0; i < r.len; i++) {
		sink ^= r.s[i];
	}
	if (strchr("*~>%|", r.t)) {
		int64_t k = r.t == '%' || r.t == '|' ? 2 * r.n : r.n;
		const char *c = r.body;
		for (int64_t i = 0; i < k; i++) {
			c = walk(c, end);
			assert(c); /* complete parent, complete children */
		}
		assert(c == q);
	}
	return q;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *b = malloc(size ? size : 1);
	memcpy(b, data, size);
	for (const char *p = b, *end = b + size; p && p < end;) {
		p = walk(p, end);
	}
	free(b);
	return 0;
}
