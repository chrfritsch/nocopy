/* RESP3 reply parser without PHP. Used by nocopy_valkey.c and tests/c/fuzz_resp.c. */

#ifndef NOCOPY_RESP_H
#define NOCOPY_RESP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A reply in the read buffer: valid until the next call that reads. Aggregates: n children (maps: 2n) from body. */
typedef struct {
	char t; /* RESP type byte; '_' for every null, '?' protocol error */
	int64_t n;
	const char *s, *body;
	size_t len;
} vk_reply;

/* The integer in [s, s + len), clamped; strtoll would skip \r\n and read past the line. */
static int64_t resp_int(const char *s, size_t len)
{
	bool neg = len && *s == '-';
	int64_t v = 0;
	for (size_t i = neg; i < len && s[i] >= '0' && s[i] <= '9' && v < INT64_MAX / 10; i++) {
		v = v * 10 + (s[i] - '0');
	}
	return neg ? -v : v;
}

/* The end of the value at p; NULL if incomplete (r->t '?' if malformed). */
static const char *resp_parse(const char *p, const char *end, vk_reply *r)
{
	const char *nl = p < end ? memchr(p, '\n', end - p) : NULL;
	if (!nl) {
		r->t = 0;
		return NULL;
	}
	if (nl - p < 2 || nl[-1] != '\r') {
		r->t = '?';
		return NULL;
	}
	r->t = *p;
	r->s = p + 1;
	r->len = nl - 1 - r->s;
	r->n = 0;
	const char *q = nl + 1;
	switch (r->t) {
		case '+': case '-': case ',': case '(': case '#': case '_':
			return q;
		case ':':
			r->n = resp_int(r->s, r->len);
			return q;
		case '$': case '!': case '=':
			r->n = resp_int(r->s, r->len);
			if (r->n < 0) {
				r->t = '_';
				return q;
			}
			if (r->n > end - q - 2) {
				r->t = 0;
				return NULL;
			}
			r->s = q;
			r->len = (size_t) r->n;
			return q + r->n + 2;
		case '*': case '~': case '>': case '%': case '|': {
			r->n = resp_int(r->s, r->len);
			if (r->n < 0) {
				r->t = '_';
				return q;
			}
			if (r->n > end - q) { /* every child takes 3 bytes or more */
				r->t = 0;
				return NULL;
			}
			r->body = q;
			int64_t k = r->t == '%' || r->t == '|' ? 2 * r->n : r->n;
			for (int64_t i = 0; i < k; i++) {
				vk_reply c;
				if (!(q = resp_parse(q, end, &c))) {
					r->t = c.t == '?' ? '?' : 0;
					return NULL;
				}
			}
			return q;
		}
		default:
			r->t = '?';
			return NULL;
	}
}

#endif
