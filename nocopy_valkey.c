/* nocopy's Valkey tier: one RESP3 connection per worker with client-side tracking (BCAST), and the commands nocopy.c needs.
 * Pushes (another connection changed a key under the prefix) drop the local copy. Every Valkey command lives here. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "zend_smart_str.h"
#include "ext/random/php_random.h"
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

bool nocopy_vk_on;
static char *vk_host, *vk_port, *vk_path, *vk_user, *vk_pass, *vk_prefix;
static size_t vk_plen;
static int vk_timeout_ms = 500;

static char *pdup(const char *s, size_t n)
{
	char *d = pemalloc(n + 1, 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

static void pfree(char **p)
{
	if (*p) {
		pefree(*p, 1);
		*p = NULL;
	}
}

/* tcp://[user:pass@]host:port, host:port, [::1]:port, unix:///path */
ZEND_INI_MH(nocopy_vk_on_url)
{
	pfree(&vk_host), pfree(&vk_port), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass);
	const char *s = ZSTR_VAL(new_value);
	nocopy_vk_on = *s != 0;
	if (!nocopy_vk_on) {
		return SUCCESS;
	}
	if (strncmp(s, "unix://", 7) == 0) {
		vk_path = pdup(s + 7, strlen(s + 7));
		return SUCCESS;
	}
	if (strncmp(s, "tcp://", 6) == 0) {
		s += 6;
	}
	const char *at = strrchr(s, '@');
	if (at) {
		const char *colon = memchr(s, ':', at - s);
		vk_user = colon ? pdup(s, colon - s) : pdup("default", 7);
		vk_pass = colon ? pdup(colon + 1, at - colon - 1) : pdup(s, at - s);
		s = at + 1;
	}
	const char *hend = s[0] == '[' ? strchr(s, ']') : strrchr(s, ':');
	if (s[0] == '[' && hend) {
		vk_host = pdup(s + 1, hend - s - 1);
		hend++;
	} else {
		hend = hend ? hend : s + strlen(s);
		vk_host = pdup(s, hend - s);
	}
	vk_port = *hend == ':' ? pdup(hend + 1, strlen(hend + 1)) : pdup("6379", 4);
	return SUCCESS;
}

/* phpinfo(): the URL without its credentials */
ZEND_INI_DISP(nocopy_vk_display)
{
	zend_string *z = type == ZEND_INI_DISPLAY_ORIG && ini_entry->modified ? ini_entry->orig_value : ini_entry->value;
	const char *v = z ? ZSTR_VAL(z) : "", *at = strrchr(v, '@'), *start = strstr(v, "://");
	if (!at) {
		PHPWRITE(v, strlen(v));
		return;
	}
	start = start ? start + 3 : v;
	PHPWRITE(v, start - v);
	PUTS("***");
	PHPWRITE(at, strlen(at));
}

ZEND_INI_MH(nocopy_vk_on_prefix)
{
	pfree(&vk_prefix);
	vk_plen = ZSTR_LEN(new_value);
	vk_prefix = pdup(ZSTR_VAL(new_value), vk_plen);
	return SUCCESS;
}

ZEND_INI_MH(nocopy_vk_on_timeout)
{
	double t = zend_strtod(ZSTR_VAL(new_value), NULL);
	vk_timeout_ms = t > 0 ? (int) (t * 1000) : 500;
	return SUCCESS;
}

void nocopy_vk_mshutdown(void)
{
	pfree(&vk_host), pfree(&vk_port), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass), pfree(&vk_prefix);
}

/* ---- connection ---- */

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool vk_fail(nocopy_vk *v)
{
	if (v->fd >= 0) {
		close(v->fd);
	}
	v->fd = -1;
	v->len = v->pos = 0;
	v->synced = false;
	v->retry_at = now_ms() + php_mt_rand_range(500, 1500); /* jitter: after a Valkey restart, workers do not reconnect in step */
	return false;
}

static bool vk_wait(int fd, short ev)
{
	struct pollfd p = {.fd = fd, .events = ev};
	int r;
	while ((r = poll(&p, 1, vk_timeout_ms)) < 0 && errno == EINTR) {
	}
	return r > 0 && !(p.revents & (POLLERR | POLLNVAL));
}

static int vk_dial_one(int family, const struct sockaddr *sa, socklen_t sl)
{
	int fd = socket(family, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	if (connect(fd, sa, sl) < 0) {
		int err = errno;
		socklen_t el = sizeof err;
		if (err == EINPROGRESS) {
			struct pollfd p = {.fd = fd, .events = POLLOUT};
			err = poll(&p, 1, vk_timeout_ms) <= 0 ? ETIMEDOUT : getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 ? errno : err;
		}
		if (err) {
			close(fd);
			errno = err;
			return -1;
		}
	}
	return fd;
}

static int vk_dial(void)
{
	if (vk_path) {
		struct sockaddr_un un = {.sun_family = AF_UNIX};
		strncpy(un.sun_path, vk_path, sizeof un.sun_path - 1);
		return vk_dial_one(AF_UNIX, (struct sockaddr *) &un, sizeof un);
	}
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res, *ai;
	if (getaddrinfo(vk_host, vk_port, &hints, &res) != 0) {
		return -1;
	}
	int fd = -1;
	for (ai = res; ai && fd < 0; ai = ai->ai_next) {
		fd = vk_dial_one(ai->ai_family, ai->ai_addr, ai->ai_addrlen);
	}
	freeaddrinfo(res);
	if (fd >= 0) {
		int one = 1, idle = 60, intvl = 10, cnt = 3;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		/* keepalive: load balancers and NAT drop idle TCP (AWS: 350 s); a dead peer is noticed in ~90 s */
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#elif defined(TCP_KEEPALIVE)
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle); /* macOS */
#endif
#ifdef TCP_KEEPINTVL
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
	}
	return fd;
}

/* ---- RESP3 ---- */

/* A reply in the read buffer: valid until the next call that reads. Aggregates: n children (maps: 2n) from body. */
typedef struct {
	char t; /* RESP type byte; '_' for every null, '?' protocol error */
	int64_t n;
	const char *s, *body;
	size_t len;
} vk_reply;

/* The end of the value at p; NULL if incomplete (r->t '?' if malformed). */
static const char *resp_parse(const char *p, const char *end, vk_reply *r)
{
	const char *nl = p < end ? memchr(p, '\n', end - p) : NULL;
	if (!nl) {
		r->t = 0;
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
			r->n = strtoll(r->s, NULL, 10);
			return q;
		case '$': case '!': case '=':
			r->n = strtoll(r->s, NULL, 10);
			if (r->n < 0) {
				r->t = '_';
				return q;
			}
			if (end - q < r->n + 2) {
				r->t = 0;
				return NULL;
			}
			r->s = q;
			r->len = (size_t) r->n;
			return q + r->n + 2;
		case '*': case '~': case '>': case '%': case '|': {
			r->n = strtoll(r->s, NULL, 10);
			if (r->n < 0) {
				r->t = '_';
				return q;
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

static void vk_apply(const vk_reply *push, const char *end)
{
	vk_reply kind, keys;
	if (push->n < 2) {
		return;
	}
	const char *q = resp_parse(push->body, end, &kind);
	if (kind.len != 10 || memcmp(kind.s, "invalidate", 10) != 0) {
		return;
	}
	resp_parse(q, end, &keys);
	__atomic_add_fetch(&nocopy_a->inval_gen, 1, __ATOMIC_SEQ_CST); /* before the drops: see the fill guard in nocopy.c */
	if (keys.t == '_') {
		nocopy_clear(); /* FLUSHALL / FLUSHDB */
		return;
	}
	q = keys.body;
	for (int64_t i = 0; i < keys.n; i++) {
		vk_reply k;
		q = resp_parse(q, end, &k);
		if (k.len >= vk_plen && memcmp(k.s, vk_prefix, vk_plen) == 0) {
			nocopy_drop_local(k.s + vk_plen, k.len - vk_plen);
		}
	}
}

/* Reads what is there (block: waits up to the timeout for at least one byte). -1: connection gone, 0: nothing, 1: read. */
static int vk_recv(nocopy_vk *v, bool block)
{
	if (v->pos == v->len) {
		v->pos = v->len = 0;
		if (v->cap > 1024 * 1024) { /* a large reply is consumed: do not keep its buffer for the worker's life */
			pefree(v->buf, 1);
			v->buf = NULL;
			v->cap = 0;
		}
	}
	if (v->cap - v->len < 16384) {
		if (v->pos) {
			memmove(v->buf, v->buf + v->pos, v->len - v->pos);
			v->len -= v->pos;
			v->pos = 0;
		}
		if (v->cap - v->len < 16384) {
			v->cap = v->cap ? v->cap * 2 : 65536;
			v->buf = perealloc(v->buf, v->cap, 1);
		}
	}
	for (;;) {
		ssize_t n = recv(v->fd, v->buf + v->len, v->cap - v->len, MSG_DONTWAIT);
		if (n > 0) {
			v->len += (size_t) n;
			return 1;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		if (!block) {
			return 0;
		}
		if (!vk_wait(v->fd, POLLIN)) {
			return -1; /* timeout: the reply stream is out of step, start over */
		}
	}
}

/* The next reply; pushes in front of it are applied. */
static bool vk_read(nocopy_vk *v, vk_reply *r)
{
	for (;;) {
		const char *end = v->buf + v->len, *q = resp_parse(v->buf + v->pos, end, r);
		if (q) {
			v->pos = (size_t) (q - v->buf);
			if (r->t == '>') {
				vk_apply(r, end);
			} else if (r->t != '|') {
				return true;
			}
			continue;
		}
		if (r->t == '?' || vk_recv(v, true) < 0) {
			return vk_fail(v);
		}
	}
}

/* Applies the pushes waiting on the socket; no command is outstanding. */
static bool vk_drain(nocopy_vk *v)
{
	for (;;) {
		int got = vk_recv(v, false);
		if (got < 0) {
			return vk_fail(v);
		}
		vk_reply r;
		for (;;) {
			const char *end = v->buf + v->len, *q = resp_parse(v->buf + v->pos, end, &r);
			if (!q) {
				break;
			}
			v->pos = (size_t) (q - v->buf);
			if (r.t == '>') {
				vk_apply(&r, end);
			} else if (r.t != '|') {
				return vk_fail(v); /* a reply nobody asked for */
			}
		}
		if (r.t == '?') {
			return vk_fail(v);
		}
		if (!got) {
			return true;
		}
	}
}

static void arg(smart_str *s, const char *a, size_t n)
{
	smart_str_appendc(s, '$');
	smart_str_append_unsigned(s, n);
	smart_str_appendl(s, "\r\n", 2);
	smart_str_appendl(s, a, n);
	smart_str_appendl(s, "\r\n", 2);
}

static void key_arg(smart_str *s, const char *k, size_t n)
{
	smart_str_appendc(s, '$');
	smart_str_append_unsigned(s, vk_plen + n);
	smart_str_appendl(s, "\r\n", 2);
	smart_str_appendl(s, vk_prefix, vk_plen);
	smart_str_appendl(s, k, n);
	smart_str_appendl(s, "\r\n", 2);
}

static void head(smart_str *s, int argc)
{
	smart_str_appendc(s, '*');
	smart_str_append_unsigned(s, argc);
	smart_str_appendl(s, "\r\n", 2);
}

#define ARG(s, lit) arg(s, lit, sizeof(lit) - 1)

static bool vk_send(nocopy_vk *v, smart_str *s)
{
	const char *b = ZSTR_VAL(s->s);
	size_t n = ZSTR_LEN(s->s);
	while (n) {
		ssize_t w = send(v->fd, b, n, MSG_NOSIGNAL);
		if (w > 0) {
			b += w, n -= (size_t) w;
		} else if (w < 0 && errno == EINTR) {
		} else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && vk_wait(v->fd, POLLOUT)) {
		} else {
			smart_str_free(s);
			return vk_fail(v);
		}
	}
	smart_str_free(s);
	return true;
}

/* Sends s (freed) and reads one reply. */
static bool vk_call(nocopy_vk *v, smart_str *s, vk_reply *r)
{
	smart_str_0(s);
	return vk_send(v, s) && vk_read(v, r);
}

static bool vk_connect(nocopy_vk *v)
{
	if (now_ms() < v->retry_at) {
		return false;
	}
	if ((v->fd = vk_dial()) < 0) {
		php_error_docref(NULL, E_WARNING, "nocopy: cannot connect to Valkey (%s): %s", vk_path ? vk_path : vk_host, strerror(errno));
		return vk_fail(v);
	}
	smart_str s = {0};
	head(&s, vk_pass ? 5 : 2);
	ARG(&s, "HELLO"), ARG(&s, "3");
	if (vk_pass) {
		ARG(&s, "AUTH"), arg(&s, vk_user, strlen(vk_user)), arg(&s, vk_pass, strlen(vk_pass));
	}
	head(&s, 7);
	ARG(&s, "CLIENT"), ARG(&s, "TRACKING"), ARG(&s, "ON"), ARG(&s, "BCAST"), ARG(&s, "PREFIX"), arg(&s, vk_prefix, vk_plen), ARG(&s, "NOLOOP");
	/* Subscribed clients are exempt from Valkey's idle `timeout`, which would otherwise close an idle worker's connection and
	 * cost a full local clear on its reconnect. Its reply is a push, skipped like the others.
	 * ponytail: the connection now counts as pubsub for client-output-buffer-limit (default 32mb, or 8mb for 60 s) of
	 * unread pushes; a worker idle through that many invalidations is disconnected and clears on reconnect, as before. */
	head(&s, 2);
	ARG(&s, "SUBSCRIBE"), ARG(&s, "__nocopy_keepalive__");
	smart_str_0(&s);
	vk_reply r = {0};
	if (!vk_send(v, &s) || !vk_read(v, &r) || r.t == '-' || r.t == '!' || !vk_read(v, &r) || r.t != '+') {
		php_error_docref(NULL, E_WARNING, "nocopy: Valkey handshake failed%s%.*s", r.t == '-' ? ": " : "", r.t == '-' ? (int) r.len : 0, r.s);
		return vk_fail(v);
	}
	if (v->ever) {
		/* invalidations may have been missed while disconnected */
		__atomic_add_fetch(&nocopy_a->inval_gen, 1, __ATOMIC_SEQ_CST);
		nocopy_clear();
	}
	v->ever = true;
	return true;
}

bool nocopy_vk_ready(void)
{
	nocopy_vk *v = &NOCOPY_G(vk);
	if (EXPECTED(v->synced)) {
		return true;
	}
	if (v->fd >= 0 && !vk_drain(v)) {
		v->retry_at = 0; /* found closed with no command in flight: reconnect now, not after the pause */
	}
	if ((v->fd < 0 && !vk_connect(v)) || !vk_drain(v)) {
		return false;
	}
	v->synced = true;
	return true;
}

void nocopy_vk_new_request(void)
{
	NOCOPY_G(vk).synced = false;
}

void nocopy_vk_close(nocopy_vk *v, bool forked)
{
	if (v->fd >= 0) {
		close(v->fd); /* in a forked child only its copy */
	}
	v->fd = -1;
	v->len = v->pos = 0;
	v->synced = false;
	if (forked) {
		v->ever = false; /* the parent's connection kept the shared arena in sync */
	} else if (v->buf) {
		pefree(v->buf, 1);
		v->buf = NULL;
		v->cap = 0;
	}
}

/* ---- commands: keys without the prefix. -1: Valkey unreachable or failed (the connection is reset). ---- */

#define READY(v) nocopy_vk *v = &NOCOPY_G(vk); if (v->fd < 0 && !nocopy_vk_ready()) return -1

int nocopy_vk_mget(uint32_t n, zend_string *const *keys, zend_string **vals)
{
	READY(v);
	smart_str s = {0};
	vk_reply r, c;
	head(&s, (int) n + 1), ARG(&s, "MGET");
	for (uint32_t i = 0; i < n; i++) {
		key_arg(&s, ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
	}
	if (!vk_call(v, &s, &r) || r.t != '*' || r.n != (int64_t) n) {
		return -1;
	}
	const char *end = v->buf + v->pos, *q = r.body;
	for (uint32_t i = 0; i < n; i++) {
		q = resp_parse(q, end, &c);
		vals[i] = c.t == '$' ? zend_string_init(c.s, c.len, 0) : NULL;
	}
	return 1;
}

int nocopy_vk_mset(uint32_t n, const nocopy_vk_kv *kv, int64_t pxat, int8_t *res)
{
	memset(res, -1, n);
	READY(v);
	smart_str s = {0};
	char b[24];
	size_t bl = pxat ? (size_t) snprintf(b, sizeof b, "%" PRId64, pxat) : 0;
	for (uint32_t i = 0; i < n; i++) {
		head(&s, pxat ? 5 : 3), ARG(&s, "SET"), key_arg(&s, kv[i].k, kv[i].kl), arg(&s, kv[i].v, kv[i].vl);
		if (pxat) {
			ARG(&s, "PXAT"), arg(&s, b, bl);
		}
	}
	smart_str_0(&s);
	if (!vk_send(v, &s)) {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		vk_reply r;
		if (!vk_read(v, &r)) {
			return -1;
		}
		res[i] = r.t == '+' ? 1 : -1;
	}
	return 1;
}

int nocopy_vk_set(const char *k, size_t kl, const char *val, size_t vl, int64_t pxat, bool nx)
{
	READY(v);
	smart_str s = {0};
	vk_reply r;
	head(&s, 3 + nx + (pxat ? 2 : 0)), ARG(&s, "SET"), key_arg(&s, k, kl), arg(&s, val, vl);
	if (nx) {
		ARG(&s, "NX");
	}
	if (pxat) {
		char b[24];
		ARG(&s, "PXAT"), arg(&s, b, (size_t) snprintf(b, sizeof b, "%" PRId64, pxat));
	}
	if (!vk_call(v, &s, &r)) {
		return -1;
	}
	return r.t == '+' ? 1 : r.t == '_' ? 0 : -1;
}

int nocopy_vk_unlink(const char *k, size_t kl)
{
	READY(v);
	smart_str s = {0};
	vk_reply r;
	head(&s, 2), ARG(&s, "UNLINK"), key_arg(&s, k, kl);
	return vk_call(v, &s, &r) && r.t == ':' ? (int) (r.n > 0) : -1;
}

static int vk_eval_long(nocopy_vk *v, const char *script, size_t sl, const char *k, size_t kl, int64_t a1, int64_t a2, int64_t *out)
{
	smart_str s = {0};
	vk_reply r;
	char b1[24], b2[24];
	head(&s, 6), ARG(&s, "EVAL"), arg(&s, script, sl), ARG(&s, "1"), key_arg(&s, k, kl);
	arg(&s, b1, (size_t) snprintf(b1, sizeof b1, "%" PRId64, a1)), arg(&s, b2, (size_t) snprintf(b2, sizeof b2, "%" PRId64, a2));
	if (!vk_call(v, &s, &r)) {
		return -1;
	}
	if (r.t != ':') {
		return 0; /* an error reply: not an integer */
	}
	*out = r.n;
	return 1;
}

/* Created as delta with ttl_ms when missing (APCu), else INCRBY. 0: not an integer. */
int nocopy_vk_incr(const char *k, size_t kl, int64_t delta, int64_t ttl_ms, int64_t *out)
{
	static const char lua[] = "if ARGV[2] ~= '0' and redis.call('EXISTS', KEYS[1]) == 0 then redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) return tonumber(ARGV[1]) end return redis.call('INCRBY', KEYS[1], ARGV[1])";
	READY(v);
	return vk_eval_long(v, lua, sizeof lua - 1, k, kl, delta, ttl_ms, out);
}

int nocopy_vk_cas(const char *k, size_t kl, int64_t old, int64_t nw)
{
	static const char lua[] = "if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('SET', KEYS[1], ARGV[2], 'KEEPTTL') return 1 end return 0";
	READY(v);
	int64_t ok = 0;
	int r = vk_eval_long(v, lua, sizeof lua - 1, k, kl, old, nw, &ok);
	return r < 0 ? -1 : r && ok;
}

/* Every key under the prefix. */
int nocopy_vk_clear(void)
{
	READY(v);
	smart_str pat = {0};
	for (size_t i = 0; i < vk_plen; i++) {
		if (strchr("*?[]\\", vk_prefix[i])) {
			smart_str_appendc(&pat, '\\');
		}
		smart_str_appendc(&pat, vk_prefix[i]);
	}
	smart_str_appendc(&pat, '*');
	smart_str_0(&pat);
	char cursor[24] = "0";
	do {
		smart_str s = {0};
		vk_reply r, c, keys;
		head(&s, 6), ARG(&s, "SCAN"), arg(&s, cursor, strlen(cursor)), ARG(&s, "MATCH"), arg(&s, ZSTR_VAL(pat.s), ZSTR_LEN(pat.s)), ARG(&s, "COUNT"), ARG(&s, "1000");
		if (!vk_call(v, &s, &r) || r.t != '*' || r.n != 2) {
			smart_str_free(&pat);
			return -1;
		}
		const char *end = v->buf + v->pos, *q = resp_parse(r.body, end, &c);
		resp_parse(q, end, &keys);
		snprintf(cursor, sizeof cursor, "%.*s", (int) c.len, c.s);
		if (keys.n > 0) {
			head(&s, (int) keys.n + 1), ARG(&s, "UNLINK");
			q = keys.body;
			for (int64_t i = 0; i < keys.n; i++) {
				vk_reply k;
				q = resp_parse(q, end, &k);
				arg(&s, k.s, k.len);
			}
			if (!vk_call(v, &s, &r)) {
				smart_str_free(&pat);
				return -1;
			}
		}
	} while (strcmp(cursor, "0") != 0);
	smart_str_free(&pat);
	return 1;
}
