/* nocopy's Valkey tier: a RESP3 command connection per worker, and per arena one listener process with client-side tracking (BCAST)
 * whose pushes (a change under the prefix) drop the local copies. Every Valkey command and the tier's routing live here; nocopy.c only calls nocopy_vk_*. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "zend_smart_str.h"
#include "nocopy_resp.h"
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
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#ifdef __linux__
# include <sys/prctl.h>
#endif
#include <arpa/inet.h>
#include <limits.h>
#ifdef HAVE_NOCOPY_TLS
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/x509v3.h>
#endif

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

bool nocopy_vk_on;
static char *vk_host, *vk_port, *vk_path, *vk_user, *vk_pass, *vk_prefix;
static size_t vk_plen;
static int vk_timeout_ms = 500;
static bool vk_tls; /* tls:// or rediss:// */
char *nocopy_vk_tls_ca, *nocopy_vk_tls_cert, *nocopy_vk_tls_key;
bool nocopy_vk_tls_verify = true;

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

/* tcp://[user:pass@]host:port, tls://.. or rediss://.. (TLS), host:port, [::1]:port, unix:///path */
ZEND_INI_MH(nocopy_vk_on_url)
{
	pfree(&vk_host), pfree(&vk_port), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass);
	const char *s = ZSTR_VAL(new_value);
	nocopy_vk_on = *s != 0;
	vk_tls = false;
	if (!nocopy_vk_on) {
		return SUCCESS;
	}
	if (strncmp(s, "unix://", 7) == 0) {
		vk_path = pdup(s + 7, strlen(s + 7));
		return SUCCESS;
	}
	if (strncmp(s, "tcp://", 6) == 0) {
		s += 6;
	} else if (strncmp(s, "tls://", 6) == 0 || strncmp(s, "rediss://", 9) == 0) {
		s = strstr(s, "://") + 3;
		vk_tls = true;
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

/* nocopy.valkey_tls_*: a path, or empty */
ZEND_INI_MH(nocopy_vk_on_path)
{
	char **p = (char **) mh_arg1;
	pfree(p);
	if (ZSTR_LEN(new_value)) {
		*p = pdup(ZSTR_VAL(new_value), ZSTR_LEN(new_value));
	}
	return SUCCESS;
}

ZEND_INI_MH(nocopy_vk_on_verify)
{
	nocopy_vk_tls_verify = zend_ini_parse_bool(new_value);
	return SUCCESS;
}

ZEND_INI_MH(nocopy_vk_on_timeout)
{
	double t = zend_strtod(ZSTR_VAL(new_value), NULL);
	vk_timeout_ms = t > 0 ? (int) (t * 1000) : 500;
	return SUCCESS;
}


/* ---- connection ---- */

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void vk_close_fd(nocopy_vk *v);

static bool vk_fail(nocopy_vk *v)
{
	vk_close_fd(v);
	v->len = v->pos = 0;
	v->synced = false;
	int64_t now = now_ms();
	v->retry_at = now + 500 + (int64_t) (((uint64_t) now * 2654435761u ^ (uint64_t) getpid()) % 1001); /* jitter: no reconnects in step */
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

/* ---- TLS ---- */

#ifdef HAVE_NOCOPY_TLS
static SSL_CTX *tls_ctx;
static const char *tls_ctx_err; /* why there is no context */
static BIO_METHOD *tls_bio;

/* Our own socket BIO: OpenSSL's writes without MSG_NOSIGNAL, and a peer gone mid-write would SIGPIPE the worker. */
static int bio_write(BIO *b, const char *d, int n)
{
	BIO_clear_retry_flags(b);
	ssize_t w = send((int) (intptr_t) BIO_get_data(b), d, (size_t) n, MSG_NOSIGNAL);
	if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
		BIO_set_retry_write(b);
	}
	return (int) w;
}

static int bio_read(BIO *b, char *d, int n)
{
	BIO_clear_retry_flags(b);
	ssize_t r = recv((int) (intptr_t) BIO_get_data(b), d, (size_t) n, 0);
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
		BIO_set_retry_read(b);
	}
	return (int) r;
}

static long bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
	(void) b, (void) num, (void) ptr;
	return cmd == BIO_CTRL_FLUSH;
}

static int bio_create(BIO *b)
{
	BIO_set_init(b, 1);
	return 1;
}

static const char *tls_error(void)
{
	static char buf[256];
	unsigned long e = ERR_peek_last_error();
	ERR_error_string_n(e, buf, sizeof buf);
	ERR_clear_error(); /* PHP's openssl functions share this thread's error queue */
	return e ? buf : "TLS failure";
}

/* Once at startup (forks and threads share it): the files are read here, and an error shows at once. */
static void tls_init(void)
{
	tls_bio = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "nocopy");
	BIO_meth_set_write(tls_bio, bio_write);
	BIO_meth_set_read(tls_bio, bio_read);
	BIO_meth_set_ctrl(tls_bio, bio_ctrl);
	BIO_meth_set_create(tls_bio, bio_create);
	SSL_CTX *c = SSL_CTX_new(TLS_client_method());
	if (!c) {
		tls_ctx_err = tls_error();
		return;
	}
	SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
	SSL_CTX_set_verify(c, nocopy_vk_tls_verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
	const char *what = NULL;
	if (nocopy_vk_tls_ca ? !SSL_CTX_load_verify_locations(c, nocopy_vk_tls_ca, NULL) : !SSL_CTX_set_default_verify_paths(c)) {
		what = "nocopy.valkey_tls_ca_file";
	} else if (nocopy_vk_tls_cert && !SSL_CTX_use_certificate_chain_file(c, nocopy_vk_tls_cert)) {
		what = "nocopy.valkey_tls_cert_file";
	} else if ((nocopy_vk_tls_key || nocopy_vk_tls_cert) && !SSL_CTX_use_PrivateKey_file(c, nocopy_vk_tls_key ? nocopy_vk_tls_key : nocopy_vk_tls_cert, SSL_FILETYPE_PEM)) {
		what = "nocopy.valkey_tls_key_file";
	}
	if (what) {
		static char buf[400];
		snprintf(buf, sizeof buf, "%s: %s", what, tls_error());
		tls_ctx_err = buf;
		php_error_docref(NULL, E_WARNING, "nocopy: %s", buf);
		SSL_CTX_free(c);
		return;
	}
	tls_ctx = c;
}

/* poll for what OpenSSL wants after a non-blocking call; false: an error (msg set) or the timeout */
static bool tls_want(nocopy_vk *v, int r, const char **msg)
{
	int e = SSL_get_error((SSL *) v->ssl, r);
	if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
		if (vk_wait(v->fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT)) {
			return true;
		}
		*msg = "TLS: timeout";
		return false;
	}
	*msg = e == SSL_ERROR_SYSCALL && !ERR_peek_last_error() ? "TLS: connection closed" : tls_error();
	return false;
}

/* Handshake on the connected v->fd: SNI and the name (or IP) checked against the certificate. */
static bool tls_connect(nocopy_vk *v, const char **msg)
{
	if (!tls_ctx) {
		*msg = tls_ctx_err ? tls_ctx_err : "TLS not initialized";
		return false;
	}
	SSL *s = SSL_new(tls_ctx);
	BIO *b = BIO_new(tls_bio);
	if (!s || !b) {
		SSL_free(s), BIO_free(b);
		*msg = tls_error();
		return false;
	}
	BIO_set_data(b, (void *) (intptr_t) v->fd);
	SSL_set_bio(s, b, b);
	v->ssl = s;
	unsigned char ip[16];
	bool is_ip = inet_pton(AF_INET, vk_host, ip) == 1 || inet_pton(AF_INET6, vk_host, ip) == 1;
	if (!is_ip) {
		SSL_set_tlsext_host_name(s, vk_host);
	}
	if (nocopy_vk_tls_verify) {
		X509_VERIFY_PARAM *p = SSL_get0_param(s);
		if (is_ip ? !X509_VERIFY_PARAM_set1_ip_asc(p, vk_host) : !X509_VERIFY_PARAM_set1_host(p, vk_host, 0)) {
			*msg = tls_error();
			return false;
		}
	}
	int r;
	while ((r = SSL_connect(s)) != 1) {
		if (!tls_want(v, r, msg)) {
			long vr = SSL_get_verify_result(s);
			if (vr != X509_V_OK) {
				*msg = X509_verify_cert_error_string(vr);
				ERR_clear_error();
			}
			return false;
		}
	}
	return true;
}
#endif

static void vk_close_fd(nocopy_vk *v)
{
#ifdef HAVE_NOCOPY_TLS
	if (v->ssl) {
		SSL_free((SSL *) v->ssl); /* no close_notify: in a forked child the parent still uses the session */
		v->ssl = NULL;
		ERR_clear_error();
	}
#endif
	if (v->fd >= 0) {
		close(v->fd);
	}
	v->fd = -1;
}

void nocopy_vk_mshutdown(void)
{
	pfree(&vk_host), pfree(&vk_port), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass), pfree(&vk_prefix);
	pfree(&nocopy_vk_tls_ca), pfree(&nocopy_vk_tls_cert), pfree(&nocopy_vk_tls_key);
#ifdef HAVE_NOCOPY_TLS
	if (tls_ctx) {
		SSL_CTX_free(tls_ctx);
		tls_ctx = NULL;
	}
	if (tls_bio) {
		BIO_meth_free(tls_bio);
		tls_bio = NULL;
	}
#endif
}

/* >0: bytes, 0: would block (*want: what to poll for), -1: the connection is gone */
static ssize_t io_read(nocopy_vk *v, char *b, size_t n, short *want)
{
#ifdef HAVE_NOCOPY_TLS
	if (v->ssl) {
		int r = SSL_read((SSL *) v->ssl, b, n > INT_MAX ? INT_MAX : (int) n);
		if (r > 0) {
			return r;
		}
		int e = SSL_get_error((SSL *) v->ssl, r);
		*want = e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
			return 0;
		}
		snprintf(v->err, sizeof v->err, "%s", ERR_peek_last_error() ? tls_error() : "connection closed");
		return -1;
	}
#endif
	ssize_t r;
	while ((r = recv(v->fd, b, n, MSG_DONTWAIT)) < 0 && errno == EINTR) {
	}
	*want = POLLIN;
	if (r > 0 || (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
		return r > 0 ? r : 0;
	}
	snprintf(v->err, sizeof v->err, "%s", r == 0 ? "connection closed" : strerror(errno));
	return -1;
}

static ssize_t io_write(nocopy_vk *v, const char *b, size_t n, short *want)
{
#ifdef HAVE_NOCOPY_TLS
	if (v->ssl) {
		int r = SSL_write((SSL *) v->ssl, b, n > INT_MAX ? INT_MAX : (int) n);
		if (r > 0) {
			return r;
		}
		int e = SSL_get_error((SSL *) v->ssl, r);
		*want = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
			return 0;
		}
		snprintf(v->err, sizeof v->err, "%s", ERR_peek_last_error() ? tls_error() : "connection closed");
		return -1;
	}
#endif
	ssize_t w;
	while ((w = send(v->fd, b, n, MSG_NOSIGNAL)) < 0 && errno == EINTR) {
	}
	*want = POLLOUT;
	if (w > 0 || (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
		return w > 0 ? w : 0;
	}
	snprintf(v->err, sizeof v->err, "%s", strerror(errno));
	return -1;
}

/* Connected (and the TLS handshake done) on v->fd. false: *msg says why. */
static bool vk_open(nocopy_vk *v, const char **msg)
{
	if ((v->fd = vk_dial()) < 0) {
		*msg = strerror(errno);
		return false;
	}
	if (!vk_tls) {
		return true;
	}
#ifdef HAVE_NOCOPY_TLS
	return tls_connect(v, msg);
#else
	*msg = "this nocopy build has no TLS (built without OpenSSL)";
	return false;
#endif
}

/* ---- RESP3 ---- */

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
		short want = POLLIN;
		ssize_t n = io_read(v, v->buf + v->len, v->cap - v->len, &want);
		if (n > 0) {
			v->len += (size_t) n;
			return 1;
		}
		if (n < 0) {
			return -1;
		}
		if (!block && want == POLLIN) {
			return 0;
		}
		if (!vk_wait(v->fd, want)) {
			snprintf(v->err, sizeof v->err, "no reply within nocopy.valkey_timeout");
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
		short want = POLLOUT;
		ssize_t w = io_write(v, b, n, &want);
		if (w > 0) {
			b += w, n -= (size_t) w;
		} else if (w == 0 && vk_wait(v->fd, want)) {
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

/* HELLO 3 (AUTH), tracking for the listener, and SUBSCRIBE: subscribed clients are exempt from Valkey's idle `timeout`.
 * ponytail: the connection counts as pubsub for client-output-buffer-limit (default 32mb, or 8mb for 60 s) of unread pushes. */
static bool vk_handshake(nocopy_vk *v, bool track, vk_reply *r)
{
	smart_str s = {0};
	head(&s, vk_pass ? 7 : 4);
	ARG(&s, "HELLO"), ARG(&s, "3");
	if (vk_pass) {
		ARG(&s, "AUTH"), arg(&s, vk_user, strlen(vk_user)), arg(&s, vk_pass, strlen(vk_pass));
	}
	ARG(&s, "SETNAME"), track ? ARG(&s, "nocopy-listener") : ARG(&s, "nocopy");
	if (track) {
		head(&s, 7);
		ARG(&s, "CLIENT"), ARG(&s, "TRACKING"), ARG(&s, "ON"), ARG(&s, "BCAST"), ARG(&s, "PREFIX"), arg(&s, vk_prefix, vk_plen), ARG(&s, "NOLOOP");
	}
	head(&s, 2);
	ARG(&s, "SUBSCRIBE"), ARG(&s, "__nocopy_keepalive__");
	smart_str_0(&s);
	*r = (vk_reply) {0};
	return vk_send(v, &s) && vk_read(v, r) && r->t != '-' && r->t != '!' && (!track || (vk_read(v, r) && r->t == '+'));
}

/* A worker's command connection: no tracking, so a reconnect needs no local clear (the listener tracks). */
static bool vk_connect(nocopy_vk *v)
{
	v->err[0] = 0;
	if (now_ms() < v->retry_at) {
		return false;
	}
	const char *msg;
	if (!vk_open(v, &msg)) {
		php_error_docref(NULL, E_WARNING, "nocopy: cannot connect to Valkey (%s): %s", vk_path ? vk_path : vk_host, msg);
		return vk_fail(v);
	}
	vk_reply r;
	if (!vk_handshake(v, false, &r)) {
		if (r.t == '-') {
			php_error_docref(NULL, E_WARNING, "nocopy: Valkey handshake failed: %.*s", (int) r.len, r.s);
		} else {
			php_error_docref(NULL, E_WARNING, "nocopy: Valkey handshake failed: %s", v->err[0] ? v->err : "unexpected reply");
		}
		return vk_fail(v);
	}
	return true;
}

/* ---- the listener: one tracking connection per arena, in a process of its own (C only, no PHP state) ---- */

#define BEAT_MS 100   /* the listener's heartbeat */
#define STALE_MS 1000 /* no heartbeat for this long: the listener is gone, the next worker starts one */

void nocopy_vk_minit(void)
{
#ifdef HAVE_NOCOPY_TLS
	if (nocopy_vk_on && vk_tls) {
		tls_init();
	}
#endif
}

/* Reads and applies what arrived. -1: the connection is gone or out of step, 0: nothing came, 1: something did. */
static int vk_listen_read(nocopy_vk *v)
{
	int got = vk_recv(v, false);
	if (got < 0) {
		return -1;
	}
	for (;;) {
		vk_reply r;
		const char *end = v->buf + v->len, *q = resp_parse(v->buf + v->pos, end, &r);
		if (!q) {
			return r.t == '?' ? -1 : got;
		}
		v->pos = (size_t) (q - v->buf);
		if (r.t == '>') {
			vk_apply(&r, end);
		} else if (r.t == '-' || r.t == '!') {
			return -1;
		}
	}
}

static ZEND_NORETURN void vk_listen(void)
{
	nocopy_arena *a = nocopy_a;
	uint32_t me = (uint32_t) getpid();
	nocopy_vk v = {.fd = -1};
	struct pollfd p = {.fd = -1, .events = POLLIN};
	int64_t ping_at = 0, heard_by = 0;
	for (;;) {
		int64_t now = now_ms();
		if (__atomic_load_n(&a->vk_pid, __ATOMIC_ACQUIRE) != me || !nocopy_arena_current()) {
			_exit(0); /* replaced, or the arena is gone */
		}
		__atomic_store_n(&a->vk_beat, now, __ATOMIC_RELEASE);
		vk_reply r;
		if (v.fd < 0 && now >= v.retry_at) {
			const char *msg;
			if (!vk_open(&v, &msg) || !vk_handshake(&v, true, &r)) {
				vk_fail(&v);
			} else {
				__atomic_add_fetch(&a->inval_gen, 1, __ATOMIC_SEQ_CST); /* the fill guard: fills in flight drop their copy */
				nocopy_clear(); /* what changed before this connection is unknown */
				__atomic_store_n(&a->vk_live, 1, __ATOMIC_RELEASE);
				ping_at = now + 1000, heard_by = 0;
			}
		}
		p.fd = v.fd;
		int wait = BEAT_MS;
#ifdef HAVE_NOCOPY_TLS
		if (v.ssl && SSL_pending((SSL *) v.ssl) > 0) {
			wait = 0; /* decrypted already, the socket may stay quiet */
		}
#endif
		if (poll(&p, 1, wait) <= 0) {
			p.revents = 0;
		}
		if (v.fd < 0) {
			continue;
		}
		int got = p.revents || !wait ? vk_listen_read(&v) : 0;
		bool ok = got >= 0;
		now = now_ms();
		if (got > 0) {
			heard_by = 0; /* anything from Valkey answers the PING */
		}
		if (ok && heard_by && now > heard_by) {
			ok = false; /* a silent connection: PING unanswered */
		} else if (ok && now >= ping_at) {
			smart_str s = {0};
			head(&s, 1);
			ARG(&s, "PING");
			smart_str_0(&s);
			ok = vk_send(&v, &s);
			ping_at = now + 1000;
			heard_by = heard_by ? heard_by : now + (vk_timeout_ms > 1000 ? vk_timeout_ms : 1000);
		}
		if (!ok) {
			__atomic_store_n(&a->vk_live, 0, __ATOMIC_RELEASE);
			vk_fail(&v);
			v.retry_at = 0; /* the first reconnect at once, then with the pause */
		}
	}
}

/* Nothing of the worker stays open in the listener: a server's listening socket would block its restart. */
static void close_inherited(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d) {
		d = opendir("/dev/fd");
	}
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			int fd = atoi(e->d_name);
			if (fd > 2 && fd != dirfd(d)) {
				close(fd);
			}
		}
		closedir(d);
	} else {
		for (int fd = 3; fd < 4096; fd++) {
			close(fd);
		}
	}
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, 0), dup2(null, 1), dup2(null, 2);
		if (null > 2) {
			close(null);
		}
	}
}

/* Double fork: the listener belongs to no worker (init reaps it), in a session of its own. */
static void vk_spawn(void)
{
	pid_t c = fork();
	if (c != 0) {
		while (c > 0 && waitpid(c, NULL, 0) < 0 && errno == EINTR) {
		}
		return;
	}
	if (fork() != 0) {
		_exit(0);
	}
	setsid();
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (int sig = 1; sig < NSIG; sig++) {
		signal(sig, SIG_DFL);
	}
#ifdef __linux__
	prctl(PR_SET_NAME, "nocopy-valkey");
#endif
	nocopy_arena_unhold();
	close_inherited();
	__atomic_store_n(&nocopy_a->vk_pid, (uint32_t) getpid(), __ATOMIC_RELEASE);
	vk_listen();
}

/* The listener is connected; started (and waited for, up to the timeout) when it is not running. */
static bool listener_live(void)
{
	nocopy_arena *a = nocopy_a;
	int64_t until = 0;
	for (;;) {
		int64_t now = now_ms(), beat = __atomic_load_n(&a->vk_beat, __ATOMIC_ACQUIRE);
		bool fresh = beat && now - beat < STALE_MS;
		if (fresh && __atomic_load_n(&a->vk_live, __ATOMIC_ACQUIRE)) {
			return true;
		}
		if (!fresh && __atomic_compare_exchange_n(&a->vk_beat, &beat, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			__atomic_store_n(&a->vk_live, 0, __ATOMIC_RELEASE); /* the dead one's */
			vk_spawn();
		}
		if (!until) {
			until = now + vk_timeout_ms;
		} else if (now >= until) {
			php_error_docref(NULL, E_WARNING, "nocopy: the Valkey listener is not connected: no invalidations, local copies are not served");
			return false;
		}
		usleep(1000);
	}
}

uint32_t nocopy_vk_listener(void)
{
	nocopy_arena *a = nocopy_a;
	int64_t beat = a ? __atomic_load_n(&a->vk_beat, __ATOMIC_ACQUIRE) : 0;
	return beat && now_ms() - beat < STALE_MS && __atomic_load_n(&a->vk_live, __ATOMIC_ACQUIRE) ? __atomic_load_n(&a->vk_pid, __ATOMIC_ACQUIRE) : 0;
}

static bool vk_ready(void)
{
	nocopy_vk *v = &NOCOPY_G(vk);
	if (EXPECTED(v->synced)) {
		return true;
	}
	if (v->fd >= 0 && !vk_drain(v)) {
		v->retry_at = 0; /* found closed with no command in flight: reconnect now, not after the pause */
	}
	if ((v->fd < 0 && !vk_connect(v)) || !listener_live()) {
		return false;
	}
	v->synced = true;
	return true;
}

void nocopy_vk_new_request(void)
{
	NOCOPY_G(vk).synced = false;
}

void nocopy_vk_close(nocopy_vk *v)
{
	vk_close_fd(v); /* in a forked child only its copy */
	v->len = v->pos = 0;
	v->synced = false;
	if (v->buf) {
		pefree(v->buf, 1);
		v->buf = NULL;
		v->cap = 0;
	}
}

/* ---- commands: keys without the prefix. -1: Valkey unreachable or failed (the connection is reset). ---- */

typedef struct {
	const char *k, *v;
	size_t kl, vl;
} vk_kv;

#define READY(v) nocopy_vk *v = &NOCOPY_G(vk); if (v->fd < 0 && !vk_ready()) return -1

static int vk_mget(uint32_t n, zend_string *const *keys, zend_string **vals)
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

static int vk_mset(uint32_t n, const vk_kv *kv, int64_t pxat, int8_t *res)
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

static int vk_set(const char *k, size_t kl, const char *val, size_t vl, int64_t pxat, bool nx)
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

static int vk_unlink(const char *k, size_t kl)
{
	READY(v);
	smart_str s = {0};
	vk_reply r;
	head(&s, 2), ARG(&s, "UNLINK"), key_arg(&s, k, kl);
	return vk_call(v, &s, &r) && r.t == ':' ? (int) (r.n > 0) : -1;
}

static int vk_unlink_n(uint32_t n, zend_string *const *keys, int64_t *gone)
{
	READY(v);
	smart_str s = {0};
	vk_reply r;
	head(&s, (int) n + 1), ARG(&s, "UNLINK");
	for (uint32_t i = 0; i < n; i++) {
		key_arg(&s, ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
	}
	if (!vk_call(v, &s, &r) || r.t != ':') {
		return -1;
	}
	*gone = r.n;
	return 1;
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
static int vk_incr(const char *k, size_t kl, int64_t delta, int64_t ttl_ms, int64_t *out)
{
	static const char lua[] = "if ARGV[2] ~= '0' and redis.call('EXISTS', KEYS[1]) == 0 then redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) return tonumber(ARGV[1]) end return redis.call('INCRBY', KEYS[1], ARGV[1])";
	READY(v);
	return vk_eval_long(v, lua, sizeof lua - 1, k, kl, delta, ttl_ms, out);
}

static int vk_cas(const char *k, size_t kl, int64_t old, int64_t nw)
{
	static const char lua[] = "if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('SET', KEYS[1], ARGV[2], 'KEEPTTL') return 1 end return 0";
	READY(v);
	int64_t ok = 0;
	int r = vk_eval_long(v, lua, sizeof lua - 1, k, kl, old, nw, &ok);
	return r < 0 ? -1 : r && ok;
}

/* Every key under the prefix. */
static int vk_clear(void)
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

/* ---- the tier: the arena holds local copies of what Valkey holds ---- */

/* After a change in Valkey: a sibling's fill that read the old value sees the guard move and drops its copy. */
static void vk_forget(zend_string *key)
{
	__atomic_add_fetch(&nocopy_a->inval_gen, 1, __ATOMIC_SEQ_CST);
	nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

#define VK_HDR 14 /* 0x00, flags (ENTRY_IGBINARY), int64 expires, uint32 compute ms; then the serialized value. Integers: decimal, for INCRBY */

static zend_string *vk_encode(zval *val, time_t expires, uint32_t user)
{
	if (Z_TYPE_P(val) == IS_LONG) {
		return zend_long_to_str(Z_LVAL_P(val));
	}
	zval ser;
	bool igb;
	if (!nocopy_serialize(val, &ser, &igb)) {
		return NULL;
	}
	zend_string *s = zend_string_alloc(VK_HDR + Z_STRLEN(ser), 0);
	char *b = ZSTR_VAL(s);
	int64_t x = expires;
	b[0] = 0;
	b[1] = igb ? ENTRY_IGBINARY : 0;
	memcpy(b + 2, &x, 8);
	memcpy(b + 10, &user, 4);
	memcpy(b + VK_HDR, Z_STRVAL(ser), Z_STRLEN(ser) + 1);
	zval_ptr_dtor(&ser);
	return s;
}

/* raw: a private copy, so unserialize may run user code that calls Valkey again */
static bool vk_decode(const zend_string *raw, zval *rv, time_t *expires, uint32_t *user)
{
	const char *b = ZSTR_VAL(raw);
	size_t n = ZSTR_LEN(raw);
	*expires = 0;
	*user = 0;
	if (n && b[0] != 0) {
		zend_long l;
		if (is_numeric_string(b, n, &l, NULL, false) != IS_LONG) {
			return false; /* not written by nocopy */
		}
		ZVAL_LONG(rv, l);
		return true;
	}
	if (n < VK_HDR) {
		return false;
	}
	int64_t x;
	memcpy(&x, b + 2, 8);
	memcpy(user, b + 10, 4);
	*expires = (time_t) x;
	if (x && x < time(NULL)) {
		return false;
	}
	return nocopy_unserialize(rv, b + VK_HDR, n - VK_HDR, ENTRY_SERIALIZED | (uint8_t) b[1]);
}

/* Decodes a value read from Valkey into rv and keeps a local copy. Add-only: a local write since our miss is newer. */
static bool vk_fill(zend_string *key, const zend_string *raw, zval *rv, time_t *exp, uint32_t *delta)
{
	time_t expires;
	uint32_t user;
	if (!vk_decode(raw, rv, &expires, &user)) {
		return false;
	}
	const nocopy_opts o = {.ttl = expires ? MAX(1, expires - time(NULL)) : 0, .objects = NOCOPY_SERIALIZE_OBJECTS};
	nocopy_put p;
	if (nocopy_build(&p, key, rv, &o) == NOCOPY_OK) {
		p.e->user = user;
		nocopy_publish(&p, true);
	}
	if (exp) {
		*exp = expires;
		*delta = user;
	}
	return true;
}

/* Local miss: GET, then keep a local copy, unless an invalidation was applied meanwhile (it may have been for this key). */
bool nocopy_vk_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (!vk_ready()) {
		ZVAL_NULL(rv);
		return false; /* no invalidations: local data may be stale */
	}
	if (nocopy_fetch_local(key, rv, exp, delta)) {
		return true;
	}
	uint64_t gen = __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST);
	zend_string *raw = NULL;
	if (vk_mget(1, &key, &raw) != 1 || !raw) {
		return false;
	}
	bool ok = vk_fill(key, raw, rv, exp, delta);
	zend_string_release(raw);
	if (ok && __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST) != gen) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
	}
	return ok;
}

/* Valkey first for add (NX decides), else local first: a sibling's invalidation can then only remove our copy, never outlive it. */
nocopy_status nocopy_vk_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	if (!vk_ready()) {
		return NOCOPY_NO_ROOM;
	}
	nocopy_put p;
	nocopy_status s = NOCOPY_OK;
	if (!o->add_only && (s = nocopy_build(&p, key, val, o)) == NOCOPY_OK) {
		p.e->user = user;
		nocopy_publish(&p, false);
	}
	if (s == NOCOPY_REFUSED) {
		return s;
	}
	time_t expires = o->ttl ? time(NULL) + o->ttl : 0;
	zend_string *enc = vk_encode(val, expires, user);
	if (!enc) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return NOCOPY_REFUSED;
	}
	int r = vk_set(ZSTR_VAL(key), ZSTR_LEN(key), ZSTR_VAL(enc), ZSTR_LEN(enc), expires ? ((int64_t) expires + 1) * 1000 : 0, o->add_only);
	zend_string_release(enc);
	if (r != 1) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return r == 0 ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
	}
	if (o->add_only && (s = nocopy_build(&p, key, val, o)) == NOCOPY_OK) {
		p.e->user = user;
		nocopy_publish(&p, false);
	} else if (s == NOCOPY_REFUSED) {
		vk_unlink(ZSTR_VAL(key), ZSTR_LEN(key)); /* a resource: APCu refuses it */
		return s;
	}
	return NOCOPY_OK;
}

/* nocopy_vk_store for many keys: local copies first, then every SET in one pipeline. */
bool nocopy_vk_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed)
{
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	if (!vk_ready()) {
		ZEND_HASH_FOREACH_KEY(vals, h, k) {
			zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
			nocopy_mark_failed(failed, key);
			zend_string_release(key);
		} ZEND_HASH_FOREACH_END();
		return false;
	}
	uint32_t cap = zend_hash_num_elements(vals), m = 0;
	zend_string **keys = safe_emalloc(cap, sizeof *keys, 0), **enc = safe_emalloc(cap, sizeof *enc, 0);
	vk_kv *kv = safe_emalloc(cap, sizeof *kv, 0);
	time_t expires = o->ttl ? time(NULL) + o->ttl : 0;
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		nocopy_put p;
		nocopy_status s = nocopy_build(&p, key, v, o);
		if (s == NOCOPY_OK) {
			p.e->user = 0;
			nocopy_publish(&p, false);
		}
		zend_string *e = s == NOCOPY_REFUSED ? NULL : vk_encode(v, expires, 0);
		if (!e) {
			nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
			all = false;
			nocopy_mark_failed(failed, key);
			zend_string_release(key);
			if (EG(exception)) {
				break;
			}
			continue;
		}
		keys[m] = key;
		enc[m] = e;
		kv[m] = (vk_kv) {.k = ZSTR_VAL(key), .kl = ZSTR_LEN(key), .v = ZSTR_VAL(e), .vl = ZSTR_LEN(e)};
		m++;
	} ZEND_HASH_FOREACH_END();
	int8_t *res = emalloc(m + 1);
	if (m) {
		vk_mset(m, kv, expires ? ((int64_t) expires + 1) * 1000 : 0, res);
	}
	for (uint32_t i = 0; i < m; i++) {
		if (res[i] != 1) {
			nocopy_drop_local(ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
			all = false;
			nocopy_mark_failed(failed, keys[i]);
		}
		zend_string_release(keys[i]);
		zend_string_release(enc[i]);
	}
	efree(res), efree(kv), efree(enc), efree(keys);
	return all;
}

void nocopy_vk_fetch_multi(HashTable *keys, HashTable *out)
{
	if (!vk_ready()) {
		return;
	}
	zval *k, t;
	zend_string **miss = safe_emalloc(zend_hash_num_elements(keys), sizeof *miss, 0);
	uint32_t n = 0;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		if (Z_TYPE_P(k) != IS_STRING) {
			continue;
		}
		if (nocopy_fetch_local(Z_STR_P(k), &t, NULL, NULL)) {
			zend_symtable_update(out, Z_STR_P(k), &t);
		} else {
			miss[n++] = Z_STR_P(k); /* borrowed: keys outlives this call */
			ZVAL_NULL(&t);
			zend_symtable_update(out, Z_STR_P(k), &t); /* keeps the requested order; removed if not found */
		}
	} ZEND_HASH_FOREACH_END();
	if (n) {
		zend_string **raw = ecalloc(n, sizeof *raw);
		uint64_t gen = __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST);
		bool got = vk_mget(n, miss, raw) == 1;
		for (uint32_t i = 0; i < n; i++) {
			if (raw[i] && !EG(exception) && vk_fill(miss[i], raw[i], &t, NULL, NULL)) {
				zend_symtable_update(out, miss[i], &t);
			} else {
				zend_symtable_del(out, miss[i]);
			}
			if (raw[i]) {
				zend_string_release(raw[i]);
			}
		}
		if (got && __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST) != gen) {
			for (uint32_t i = 0; i < n; i++) {
				nocopy_drop_local(ZSTR_VAL(miss[i]), ZSTR_LEN(miss[i]));
			}
		}
		efree(raw);
	}
	efree(miss);
}

bool nocopy_vk_drop(zend_string *key)
{
	int r = vk_ready() ? vk_unlink(ZSTR_VAL(key), ZSTR_LEN(key)) : -1;
	vk_forget(key);
	return r == 1;
}

zend_long nocopy_vk_drop_multi(HashTable *keys)
{
	zend_string **ks = safe_emalloc(zend_hash_num_elements(keys), sizeof *ks, 0);
	uint32_t n = 0;
	zval *k;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		if (Z_TYPE_P(k) == IS_STRING) {
			ks[n++] = Z_STR_P(k);
		}
	} ZEND_HASH_FOREACH_END();
	int64_t gone = 0;
	if (n && vk_ready()) {
		vk_unlink_n(n, ks, &gone);
	}
	for (uint32_t i = 0; i < n; i++) {
		vk_forget(ks[i]);
	}
	efree(ks);
	return (zend_long) gone;
}

bool nocopy_vk_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out)
{
	int64_t v = 0;
	int r = vk_ready() ? vk_incr(ZSTR_VAL(key), ZSTR_LEN(key), delta, (int64_t) ttl * 1000, &v) : -1;
	vk_forget(key); /* the next read fetches it */
	*out = (zend_long) v;
	return r == 1;
}

bool nocopy_vk_cas_long(zend_string *key, zend_long old, zend_long nw)
{
	int r = vk_ready() ? vk_cas(ZSTR_VAL(key), ZSTR_LEN(key), old, nw) : -1;
	vk_forget(key);
	return r == 1;
}

void nocopy_vk_clear(void)
{
	if (vk_ready()) {
		vk_clear();
	}
}
