/* nocopy's Valkey tier: a RESP3 command connection per worker, and per arena one listener process with client-side tracking (BCAST)
 * whose pushes (a change under the prefix) drop the local copies. Every Valkey command and the tier's routing live here; the store reaches it only through
 * nocopy_detour, the module through nocopy_valkey.h.
 * The connections are libvalkey's (third_party/libvalkey), blocking with nocopy.valkey_timeout; pushes come back as replies and are applied here. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "zend_smart_str.h"
#include "ext/random/php_random.h"
#include <valkey/valkey.h>
#include <time.h>
#include <sys/socket.h>
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
#ifdef HAVE_NOCOPY_TLS
# include <valkey/tls.h>
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/x509v3.h>
#endif

static bool vk_on;
static char *vk_host, *vk_path, *vk_user, *vk_pass, *vk_prefix;
static int vk_port;
static size_t vk_plen;
static int vk_timeout_ms = 500;
static bool vk_tls; /* tls:// or rediss:// */
static char *vk_tls_ca, *vk_tls_cert, *vk_tls_key;
static bool vk_tls_verify = true;

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
static ZEND_INI_MH(on_url)
{
	pfree(&vk_host), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass);
	const char *s = ZSTR_VAL(new_value);
	vk_on = *s != 0;
	vk_tls = false;
	if (!vk_on) {
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
	vk_port = *hend == ':' ? atoi(hend + 1) : 6379;
	return SUCCESS;
}

/* phpinfo(): the URL without its credentials */
static ZEND_INI_DISP(display_url)
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

static ZEND_INI_MH(on_prefix)
{
	pfree(&vk_prefix);
	vk_plen = ZSTR_LEN(new_value);
	vk_prefix = pdup(ZSTR_VAL(new_value), vk_plen);
	return SUCCESS;
}

/* nocopy.valkey_tls_*: a path, or empty */
static ZEND_INI_MH(on_path)
{
	char **p = (char **) mh_arg1;
	pfree(p);
	if (ZSTR_LEN(new_value)) {
		*p = pdup(ZSTR_VAL(new_value), ZSTR_LEN(new_value));
	}
	return SUCCESS;
}

static ZEND_INI_MH(on_verify)
{
	vk_tls_verify = zend_ini_parse_bool(new_value);
	return SUCCESS;
}

static ZEND_INI_MH(on_timeout)
{
	double t = zend_strtod(ZSTR_VAL(new_value), NULL);
	vk_timeout_ms = t > 0 ? (int) (t * 1000) : 500;
	return SUCCESS;
}


/* ---- connection ---- */

static void vk_close_c(nocopy_vk *v)
{
	if (v->c) {
		valkeyFree(v->c); /* with TLS: SSL_free, no close_notify: in a forked child the parent still uses the session */
		v->c = v->ssl = NULL;
	}
#ifdef HAVE_NOCOPY_TLS
	ERR_clear_error(); /* PHP's openssl functions share this thread's error queue */
#endif
}

static bool vk_fail(nocopy_vk *v)
{
	valkeyContext *c = v->c;
	if (c && c->err) {
		snprintf(v->err, sizeof v->err, "%s", c->errstr);
	}
	vk_close_c(v);
	v->synced = false;
	int64_t now = nocopy_mono_ms();
	v->retry_at = now + 500 + (int64_t) (((uint64_t) now * 2654435761u ^ (uint64_t) getpid()) % 1001); /* jitter: no reconnects in step */
	return false;
}

/* ---- TLS ---- */

#ifdef HAVE_NOCOPY_TLS
static SSL_CTX *tls_ctx;
static const char *tls_ctx_err; /* why there is no context */

static const char *tls_error(void)
{
	static char buf[256];
	unsigned long e = ERR_peek_last_error();
	ERR_error_string_n(e, buf, sizeof buf);
	ERR_clear_error();
	return e ? buf : "TLS failure";
}

/* Once at startup (forks and threads share it): the files are read here, and an error shows at once. */
static void tls_init(void)
{
	SSL_CTX *c = SSL_CTX_new(TLS_client_method());
	if (!c) {
		tls_ctx_err = tls_error();
		return;
	}
	SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
	SSL_CTX_set_verify(c, vk_tls_verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
	const char *what = NULL;
	if (vk_tls_ca ? !SSL_CTX_load_verify_locations(c, vk_tls_ca, NULL) : !SSL_CTX_set_default_verify_paths(c)) {
		what = "nocopy.valkey_tls_ca_file";
	} else if (vk_tls_cert && !SSL_CTX_use_certificate_chain_file(c, vk_tls_cert)) {
		what = "nocopy.valkey_tls_cert_file";
	} else if ((vk_tls_key || vk_tls_cert) && !SSL_CTX_use_PrivateKey_file(c, vk_tls_key ? vk_tls_key : vk_tls_cert, SSL_FILETYPE_PEM)) {
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

/* Handshake on the connected context: SNI and the name (or IP) checked against the certificate. */
static bool tls_connect(nocopy_vk *v, const char **msg)
{
	if (!tls_ctx) {
		*msg = tls_ctx_err ? tls_ctx_err : "TLS not initialized";
		return false;
	}
	SSL *s = SSL_new(tls_ctx);
	if (!s) {
		*msg = tls_error();
		return false;
	}
	unsigned char ip[16];
	bool is_ip = inet_pton(AF_INET, vk_host, ip) == 1 || inet_pton(AF_INET6, vk_host, ip) == 1;
	if (!is_ip) {
		SSL_set_tlsext_host_name(s, vk_host);
	}
	if (vk_tls_verify) {
		X509_VERIFY_PARAM *p = SSL_get0_param(s);
		if (is_ip ? !X509_VERIFY_PARAM_set1_ip_asc(p, vk_host) : !X509_VERIFY_PARAM_set1_host(p, vk_host, 0)) {
			*msg = tls_error();
			SSL_free(s);
			return false;
		}
	}
	if (valkeyInitiateTLS(v->c, s) != VALKEY_OK) {
		long vr = SSL_get_verify_result(s);
		*msg = vr != X509_V_OK ? X509_verify_cert_error_string(vr) : ERR_peek_last_error() ? tls_error() : ((valkeyContext *) v->c)->errstr;
		SSL_free(s); /* not taken over on failure */
		ERR_clear_error();
		return false;
	}
	v->ssl = s;
	return true;
}
#endif

void nocopy_vk_mshutdown(void)
{
	pfree(&vk_host), pfree(&vk_path), pfree(&vk_user), pfree(&vk_pass), pfree(&vk_prefix);
	pfree(&vk_tls_ca), pfree(&vk_tls_cert), pfree(&vk_tls_key);
#ifdef HAVE_NOCOPY_TLS
	if (tls_ctx) {
		SSL_CTX_free(tls_ctx);
		tls_ctx = NULL;
	}
#endif
}

/* Connected (and the TLS handshake done) in v->c, which vk_fail closes. false: *msg says why. */
static bool vk_open(nocopy_vk *v, const char **msg)
{
	struct timeval tv = {.tv_sec = vk_timeout_ms / 1000, .tv_usec = (vk_timeout_ms % 1000) * 1000};
	valkeyOptions o = {.options = VALKEY_OPT_NO_PUSH_AUTOFREE | VALKEY_OPT_PREFER_IP_UNSPEC, .connect_timeout = &tv, .command_timeout = &tv};
	if (vk_path) {
		VALKEY_OPTIONS_SET_UNIX(&o, vk_path);
	} else {
		VALKEY_OPTIONS_SET_TCP(&o, vk_host, vk_port);
	}
	valkeyContext *c = valkeyConnectWithOptions(&o);
	v->c = c;
	if (!c || c->err) {
		*msg = c ? c->errstr : "out of memory";
		return false;
	}
	int one = 1;
	fcntl(c->fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* macOS; PHP's SAPIs ignore SIGPIPE elsewhere */
#endif
	if (!vk_path) {
		int idle = 60, intvl = 10, cnt = 3;
		/* keepalive: load balancers and NAT drop idle TCP (AWS: 350 s); a dead peer is noticed in ~90 s */
		setsockopt(c->fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
		setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#elif defined(TCP_KEEPALIVE)
		setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle); /* macOS */
#endif
#ifdef TCP_KEEPINTVL
		setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
		setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
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

/* ---- replies ---- */

static bool is_str(const valkeyReply *r)
{
	return r->type == VALKEY_REPLY_STRING || r->type == VALKEY_REPLY_STATUS || r->type == VALKEY_REPLY_VERB;
}

/* A push whose first element is name. */
static bool push_is(const valkeyReply *r, const char *name, size_t n) { return r->type == VALKEY_REPLY_PUSH && r->elements && is_str(r->element[0]) && r->element[0]->len == n && memcmp(r->element[0]->str, name, n) == 0; }

/* The fill guard: a fill that read Valkey before an invalidation was applied sees the generation move and drops its copy (nocopy_vk_fetch). */
static uint64_t vk_gen(void) { return __atomic_load_n(&NOCOPY_SHARED->vk.inval_gen, __ATOMIC_SEQ_CST); }
static void vk_bump(void) { __atomic_add_fetch(&NOCOPY_SHARED->vk.inval_gen, 1, __ATOMIC_SEQ_CST); }

static void vk_apply(const valkeyReply *push)
{
	if (push->elements < 2 || !push_is(push, ZEND_STRL("invalidate"))) {
		return;
	}
	const valkeyReply *keys = push->element[1];
	vk_bump(); /* before the drops */
	if (keys->type == VALKEY_REPLY_NIL) {
		nocopy_clear(); /* FLUSHALL / FLUSHDB */
		return;
	}
	for (size_t i = 0; i < keys->elements; i++) {
		const valkeyReply *k = keys->element[i];
		if (is_str(k) && k->len >= vk_plen && memcmp(k->str, vk_prefix, vk_plen) == 0) {
			nocopy_drop_local(k->str + vk_plen, k->len - vk_plen);
		}
	}
}

/* The next reply, or push named stop (freeReplyObject it); other pushes in front of it are applied. NULL: the connection failed and is closed. */
static valkeyReply *vk_get_until(nocopy_vk *v, const char *stop)
{
	for (;;) {
		valkeyReply *r;
		if (valkeyGetReply(v->c, (void **) &r) != VALKEY_OK) {
			vk_fail(v);
			return NULL;
		}
		if (r->type != VALKEY_REPLY_PUSH || (stop && push_is(r, stop, strlen(stop)))) {
			return r;
		}
		vk_apply(r);
		freeReplyObject(r);
	}
}

static valkeyReply *vk_get(nocopy_vk *v) { return vk_get_until(v, NULL); }

/* Applies the complete pushes read so far. false: a protocol error, an error reply, or (strict) any other reply. */
static bool vk_pushes(valkeyContext *c, bool strict)
{
	valkeyReply *r;
	while (valkeyGetReplyFromReader(c, (void **) &r) == VALKEY_OK && r) {
		bool push = r->type == VALKEY_REPLY_PUSH, bad = r->type == VALKEY_REPLY_ERROR || (strict && !push);
		if (push) {
			vk_apply(r);
		}
		freeReplyObject(r);
		if (bad) {
			return false;
		}
	}
	return !c->err;
}

/* Applies the pushes waiting on the socket; no command is outstanding. */
static bool vk_drain(nocopy_vk *v)
{
	valkeyContext *c = v->c;
	struct pollfd p = {.fd = c->fd, .events = POLLIN};
	while (poll(&p, 1, 0) > 0) {
		if (valkeyBufferRead(c) != VALKEY_OK || !vk_pushes(c, true)) {
			return vk_fail(v);
		}
	}
	return true;
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

static void arg_long(smart_str *s, int64_t v)
{
	char b[24];
	arg(s, b, (size_t) snprintf(b, sizeof b, "%" PRId64, v));
}

/* SET key val [NX] [PX px]: relative, so Valkey's clock is never compared with ours */
static void set_cmd(smart_str *s, const char *k, size_t kl, const char *val, size_t vl, int64_t px, bool nx)
{
	head(s, 3 + nx + (px ? 2 : 0)), ARG(s, "SET"), key_arg(s, k, kl), arg(s, val, vl);
	if (nx) {
		ARG(s, "NX");
	}
	if (px) {
		ARG(s, "PX"), arg_long(s, px);
	}
}

/* cmd key... */
static void keys_cmd(smart_str *s, const char *cmd, size_t cl, uint32_t n, zend_string *const *keys)
{
	head(s, (int) n + 1), arg(s, cmd, cl);
	for (uint32_t i = 0; i < n; i++) {
		key_arg(s, ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
	}
}

/* Queues the commands in s (freed); vk_get sends them and reads the replies. */
static bool vk_send(nocopy_vk *v, smart_str *s)
{
	smart_str_0(s);
	bool ok = valkeyAppendFormattedCommand(v->c, ZSTR_VAL(s->s), ZSTR_LEN(s->s)) == VALKEY_OK;
	smart_str_free(s);
	return ok || vk_fail(v);
}

/* Sends s (freed) and reads one reply (freeReplyObject it). NULL: the connection failed. */
static valkeyReply *vk_call(nocopy_vk *v, smart_str *s)
{
	return vk_send(v, s) ? vk_get(v) : NULL;
}

/* SUBSCRIBE's reply: a push, or an error (NOPERM) that is read here so later replies stay in step. Refused: only the idle exemption is lost. */
static bool vk_subscribed(nocopy_vk *v)
{
	valkeyReply *r = vk_get_until(v, "subscribe");
	bool ok = r != NULL;
	freeReplyObject(r);
	return ok;
}

/* HELLO 3 (AUTH), tracking for the listener, and SUBSCRIBE: subscribed clients are exempt from Valkey's idle `timeout`.
 * ponytail: the connection counts as pubsub for client-output-buffer-limit (default 32mb, or 8mb for 60 s) of unread pushes. */
static bool vk_handshake(nocopy_vk *v, bool track)
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
	if (!vk_send(v, &s)) {
		return false;
	}
	valkeyReply *r = vk_get(v);
	bool ok = r && r->type != VALKEY_REPLY_ERROR;
	if (r && !ok) {
		snprintf(v->err, sizeof v->err, "%.*s", (int) r->len, r->str);
	}
	freeReplyObject(r);
	if (ok && track) {
		r = vk_get(v);
		ok = r && r->type == VALKEY_REPLY_STATUS;
		freeReplyObject(r);
	}
	return ok && vk_subscribed(v);
}

/* A worker's command connection: no tracking, so a reconnect needs no local clear (the listener tracks). */
static bool vk_connect(nocopy_vk *v)
{
	v->err[0] = 0;
	if (nocopy_mono_ms() < v->retry_at) {
		return false;
	}
	const char *msg;
	if (!vk_open(v, &msg)) {
		php_error_docref(NULL, E_WARNING, "nocopy: cannot connect to Valkey (%s): %s", vk_path ? vk_path : vk_host, msg);
		return vk_fail(v);
	}
	if (!vk_handshake(v, false)) {
		php_error_docref(NULL, E_WARNING, "nocopy: Valkey handshake failed: %s", v->err[0] ? v->err : "unexpected reply");
		return vk_fail(v);
	}
	return true;
}

/* ---- the listener: one tracking connection per arena, in a process of its own (C only, no PHP state) ---- */

#define BEAT_MS 100   /* the listener's heartbeat */
#define STALE_MS 1000 /* no heartbeat for this long: the listener is gone, the next worker starts one */

static const zend_ini_entry_def vk_ini[] = {
	PHP_INI_ENTRY_EX("nocopy.valkey", "", PHP_INI_SYSTEM, on_url, display_url)
	PHP_INI_ENTRY("nocopy.valkey_prefix", "nocopy:", PHP_INI_SYSTEM, on_prefix)
	PHP_INI_ENTRY("nocopy.valkey_timeout", "0.5", PHP_INI_SYSTEM, on_timeout)
	PHP_INI_ENTRY1("nocopy.valkey_tls_ca_file", "", PHP_INI_SYSTEM, on_path, &vk_tls_ca)
	PHP_INI_ENTRY1("nocopy.valkey_tls_cert_file", "", PHP_INI_SYSTEM, on_path, &vk_tls_cert)
	PHP_INI_ENTRY1("nocopy.valkey_tls_key_file", "", PHP_INI_SYSTEM, on_path, &vk_tls_key)
	PHP_INI_ENTRY("nocopy.valkey_tls_verify", "1", PHP_INI_SYSTEM, on_verify)
	{0}
};

void nocopy_vk_minit(int module_number)
{
	zend_register_ini_entries(vk_ini, module_number);
#ifdef HAVE_NOCOPY_TLS
	if (vk_on && vk_tls) {
		tls_init();
	}
#endif
}

/* Writes what vk_send queued. */
static bool vk_flush(valkeyContext *c)
{
	int done = 0;
	while (!done) {
		if (valkeyBufferWrite(c, &done) != VALKEY_OK) {
			return false;
		}
	}
	return true;
}

static ZEND_NORETURN void vk_listen(void)
{
	nocopy_vk_shared *a = &NOCOPY_SHARED->vk;
	uint32_t me = (uint32_t) getpid();
	nocopy_vk v = {0};
	struct pollfd p = {.fd = -1, .events = POLLIN};
	int64_t ping_at = 0, heard_by = 0;
	for (;;) {
		int64_t now = nocopy_mono_ms();
		if (__atomic_load_n(&a->vk_pid, __ATOMIC_ACQUIRE) != me || !nocopy_arena_current()) {
			_exit(0); /* replaced, or the arena is gone */
		}
		__atomic_store_n(&a->vk_beat, now, __ATOMIC_RELEASE);
		if (!v.c && now >= v.retry_at) {
			const char *msg;
			if (!vk_open(&v, &msg) || !vk_handshake(&v, true)) {
				vk_fail(&v);
			} else {
				vk_bump(); /* fills in flight drop their copy */
				nocopy_clear(); /* what changed before this connection is unknown */
				__atomic_store_n(&a->vk_live, 1, __ATOMIC_RELEASE);
				ping_at = now + 1000, heard_by = 0;
			}
		}
		valkeyContext *c = v.c;
		p.fd = c ? c->fd : -1;
		int wait = BEAT_MS;
#ifdef HAVE_NOCOPY_TLS
		if (v.ssl && SSL_pending((SSL *) v.ssl) > 0) {
			wait = 0; /* decrypted already, the socket may stay quiet */
		}
#endif
		if (poll(&p, 1, wait) <= 0) {
			p.revents = 0;
		}
		if (!c) {
			continue;
		}
		bool got = p.revents || !wait, ok = !got || (valkeyBufferRead(c) == VALKEY_OK && vk_pushes(c, false));
		now = nocopy_mono_ms();
		if (got) {
			heard_by = 0; /* anything from Valkey answers the PING */
		}
		if (ok && heard_by && now > heard_by) {
			ok = false; /* a silent connection: PING unanswered */
		} else if (ok && now >= ping_at) {
			ok = valkeyAppendCommand(c, "PING") == VALKEY_OK && vk_flush(c);
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
	signal(SIGPIPE, SIG_IGN); /* a write to a closed connection fails instead */
#ifdef __linux__
	prctl(PR_SET_NAME, "nocopy-valkey");
#endif
	nocopy_arena_unhold();
	close_inherited();
	__atomic_store_n(&NOCOPY_SHARED->vk.vk_pid, (uint32_t) getpid(), __ATOMIC_RELEASE);
	vk_listen();
}

/* A heartbeat under STALE_MS old: the listener runs. */
static bool beat_fresh(int64_t beat, int64_t now) { return beat && now - beat < STALE_MS; }

/* The listener is connected; started (and waited for, up to the timeout) when it is not running. */
static bool listener_live(int wait_ms)
{
	nocopy_vk_shared *a = &NOCOPY_SHARED->vk;
	int64_t until = nocopy_mono_ms() + wait_ms;
	for (;;) {
		int64_t now = nocopy_mono_ms(), beat = __atomic_load_n(&a->vk_beat, __ATOMIC_ACQUIRE);
		bool fresh = beat_fresh(beat, now);
		if (fresh && __atomic_load_n(&a->vk_live, __ATOMIC_ACQUIRE)) {
			return true;
		}
		if (!fresh && __atomic_compare_exchange_n(&a->vk_beat, &beat, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			__atomic_store_n(&a->vk_live, 0, __ATOMIC_RELEASE); /* the dead one's */
			vk_spawn();
		}
		if (now >= until) {
			if (wait_ms) {
				php_error_docref(NULL, E_WARNING, "nocopy: the Valkey listener is not connected: no invalidations, local copies are not served");
			}
			return false;
		}
		usleep(1000);
	}
}

uint32_t nocopy_vk_listener(void)
{
	nocopy_vk_shared *a = nocopy_attached() ? &NOCOPY_SHARED->vk : NULL;
	int64_t beat = a ? __atomic_load_n(&a->vk_beat, __ATOMIC_ACQUIRE) : 0;
	return beat_fresh(beat, nocopy_mono_ms()) && __atomic_load_n(&a->vk_live, __ATOMIC_ACQUIRE) ? __atomic_load_n(&a->vk_pid, __ATOMIC_ACQUIRE) : 0;
}

static bool vk_ready(void)
{
	nocopy_vk *v = &NOCOPY_G(vk);
	if (EXPECTED(v->synced)) {
		return true;
	}
	if (v->c && !vk_drain(v)) {
		v->retry_at = 0; /* found closed with no command in flight: reconnect now, not after the pause */
	}
	if (!v->c && !vk_connect(v)) {
		return false;
	}
	bool wait = nocopy_mono_ms() >= v->retry_at;
	if (!listener_live(wait ? vk_timeout_ms : 0)) {
		if (wait) {
			v->retry_at = nocopy_mono_ms() + 500; /* the next calls check without waiting */
		}
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
	vk_close_c(v); /* in a forked child only its copy */
	v->synced = false;
}

/* ---- commands: keys without the prefix. -1: Valkey unreachable or failed (the connection is reset). ---- */

typedef struct {
	const char *k, *v;
	size_t kl, vl;
} vk_kv;

/* This worker's connection, (re)connected if it dropped; NULL: Valkey unreachable. */
static nocopy_vk *vk_conn(void)
{
	nocopy_vk *v = &NOCOPY_G(vk);
	return v->c || vk_ready() ? v : NULL;
}

/* vk_call on this worker's connection; NULL also when Valkey is unreachable. s is freed either way. */
static valkeyReply *vk_cmd(smart_str *s)
{
	nocopy_vk *v = vk_conn();
	if (!v) {
		smart_str_free(s);
		return NULL;
	}
	return vk_call(v, s);
}

static int vk_mget(uint32_t n, zend_string *const *keys, zend_string **vals)
{
	smart_str s = {0};
	keys_cmd(&s, ZEND_STRL("MGET"), n, keys);
	valkeyReply *r = vk_cmd(&s);
	bool ok = r && r->type == VALKEY_REPLY_ARRAY && r->elements == n;
	for (uint32_t i = 0; ok && i < n; i++) {
		const valkeyReply *c = r->element[i];
		vals[i] = c->type == VALKEY_REPLY_STRING ? zend_string_init(c->str, c->len, 0) : NULL;
	}
	freeReplyObject(r);
	return ok ? 1 : -1;
}

static int vk_mset(uint32_t n, const vk_kv *kv, int64_t px, int8_t *res)
{
	memset(res, -1, n);
	nocopy_vk *v = vk_conn();
	if (!v) {
		return -1;
	}
	smart_str s = {0};
	for (uint32_t i = 0; i < n; i++) {
		set_cmd(&s, kv[i].k, kv[i].kl, kv[i].v, kv[i].vl, px, false);
	}
	if (!vk_send(v, &s)) {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		valkeyReply *r = vk_get(v);
		if (!r) {
			return -1;
		}
		res[i] = r->type == VALKEY_REPLY_STATUS ? 1 : -1;
		freeReplyObject(r);
	}
	return 1;
}

static int vk_set(const char *k, size_t kl, const char *val, size_t vl, int64_t px, bool nx)
{
	smart_str s = {0};
	set_cmd(&s, k, kl, val, vl, px, nx);
	valkeyReply *r = vk_cmd(&s);
	int ret = !r ? -1 : r->type == VALKEY_REPLY_STATUS ? 1 : r->type == VALKEY_REPLY_NIL ? 0 : -1;
	freeReplyObject(r);
	return ret;
}

/* UNLINK of n keys; *gone: how many existed. */
static int vk_unlink_n(uint32_t n, zend_string *const *keys, int64_t *gone)
{
	smart_str s = {0};
	keys_cmd(&s, ZEND_STRL("UNLINK"), n, keys);
	valkeyReply *r = vk_cmd(&s);
	bool ok = r && r->type == VALKEY_REPLY_INTEGER;
	if (ok) {
		*gone = r->integer;
	}
	freeReplyObject(r);
	return ok ? 1 : -1;
}

static int vk_eval_long(const char *script, size_t sl, const char *k, size_t kl, int64_t a1, int64_t a2, int64_t *out)
{
	smart_str s = {0};
	head(&s, 6), ARG(&s, "EVAL"), arg(&s, script, sl), ARG(&s, "1"), key_arg(&s, k, kl), arg_long(&s, a1), arg_long(&s, a2);
	valkeyReply *r = vk_cmd(&s);
	if (!r) {
		return -1;
	}
	int ret = r->type == VALKEY_REPLY_INTEGER; /* else an error reply: not an integer */
	if (ret) {
		*out = r->integer;
	}
	freeReplyObject(r);
	return ret;
}

/* Created as delta with ttl_ms when missing (APCu), else INCRBY. 0: not an integer. */
static int vk_incr(const char *k, size_t kl, int64_t delta, int64_t ttl_ms, int64_t *out)
{
	static const char lua[] = "if ARGV[2] ~= '0' and redis.call('EXISTS', KEYS[1]) == 0 then redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) return tonumber(ARGV[1]) end return redis.call('INCRBY', KEYS[1], ARGV[1])";
	return vk_eval_long(lua, sizeof lua - 1, k, kl, delta, ttl_ms, out);
}

static int vk_cas(const char *k, size_t kl, int64_t old, int64_t nw)
{
	static const char lua[] = "if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('SET', KEYS[1], ARGV[2], 'KEEPTTL') return 1 end return 0";
	int64_t ok = 0;
	int r = vk_eval_long(lua, sizeof lua - 1, k, kl, old, nw, &ok);
	return r < 0 ? -1 : r && ok;
}

/* Every key under the prefix. */
static int vk_clear(void)
{
	nocopy_vk *v = vk_conn();
	if (!v) {
		return -1;
	}
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
	int ret = 1;
	do {
		smart_str s = {0};
		head(&s, 6), ARG(&s, "SCAN"), arg(&s, cursor, strlen(cursor)), ARG(&s, "MATCH"), arg(&s, ZSTR_VAL(pat.s), ZSTR_LEN(pat.s)), ARG(&s, "COUNT"), ARG(&s, "1000");
		valkeyReply *r = vk_call(v, &s);
		if (!r || r->type != VALKEY_REPLY_ARRAY || r->elements != 2 || !is_str(r->element[0])) {
			freeReplyObject(r);
			ret = -1;
			break;
		}
		const valkeyReply *keys = r->element[1];
		snprintf(cursor, sizeof cursor, "%.*s", (int) r->element[0]->len, r->element[0]->str);
		size_t n = keys->type == VALKEY_REPLY_ARRAY ? keys->elements : 0;
		if (n) {
			head(&s, (int) n + 1), ARG(&s, "UNLINK");
			for (size_t i = 0; i < n; i++) {
				arg(&s, keys->element[i]->str, keys->element[i]->len);
			}
		}
		freeReplyObject(r);
		if (n) {
			r = vk_call(v, &s);
			bool failed = !r;
			freeReplyObject(r);
			if (failed) {
				ret = -1;
				break;
			}
		}
	} while (strcmp(cursor, "0") != 0);
	smart_str_free(&pat);
	return ret;
}

/* ---- the tier: the arena holds local copies of what Valkey holds ---- */

/* After a change in Valkey: a sibling's fill that read the old value sees the guard move and drops its copy. */
static void vk_forget(zend_string *key)
{
	vk_bump(); /* before the drop */
	nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

/* A ttl as the last unix second it lives (0: never; the payload's, other hosts compare it), and the ms to that second's end for SET PX. */
static time_t vk_expires(zend_long ttl) { return nocopy_expiry(time(NULL), ttl); }
static int64_t vk_px(time_t expires)
{
	if (!expires) {
		return 0;
	}
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	int64_t ms = ((int64_t) expires + 1 - ts.tv_sec) * 1000 - ts.tv_nsec / 1000000;
	return ms > 0 ? ms : 1;
}

#define VK_HDR 14 /* 0x00, flags (VK_IGB), int64 expires, uint32 compute ms; then the serialized value. Integers: decimal, for INCRBY */
#define VK_IGB 2  /* the value is igbinary's (a wire format: Valkey may hold values older builds wrote) */

static zend_string *vk_encode(zval *val, time_t expires, uint32_t user)
{
	if (Z_TYPE_P(val) == IS_LONG) {
		return zend_long_to_str(Z_LVAL_P(val));
	}
	zval ser;
	nocopy_fmt fmt;
	if (!nocopy_serialize(val, &ser, &fmt)) {
		return NULL;
	}
	zend_string *s = zend_string_alloc(VK_HDR + Z_STRLEN(ser), 0);
	char *b = ZSTR_VAL(s);
	int64_t x = expires;
	b[0] = 0;
	b[1] = fmt == NOCOPY_FMT_IGB ? VK_IGB : 0;
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
	return nocopy_unserialize(rv, b + VK_HDR, n - VK_HDR, b[1] & VK_IGB ? NOCOPY_FMT_IGB : NOCOPY_FMT_PHP);
}

/* Decodes a value read from Valkey into rv and keeps a local copy. Add-only: a local write since our miss is newer. */
static bool vk_fill(zend_string *key, const zend_string *raw, zval *rv, time_t *exp, uint32_t *delta)
{
	time_t expires;
	uint32_t user;
	if (!vk_decode(raw, rv, &expires, &user)) {
		return false;
	}
	const nocopy_opts o = {.expires = nocopy_from_wall(expires), .add_only = true};
	if (!expires || expires >= time(NULL)) { /* Valkey's PX decides it lives; the writer's second only bounds our copy */
		nocopy_put_local(key, rv, &o, user);
	}
	if (exp) {
		*exp = o.expires;
		*delta = user;
	}
	return true;
}

/* Local miss: GET, then keep a local copy, unless an invalidation was applied meanwhile (it may have been for this key). */
static bool nocopy_vk_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (!vk_ready()) {
		ZVAL_NULL(rv);
		return false; /* no invalidations: local data may be stale */
	}
	if (nocopy_fetch_local(key, rv, exp, delta)) {
		return true;
	}
	uint64_t gen = vk_gen();
	zend_string *raw = NULL;
	if (vk_mget(1, &key, &raw) != 1 || !raw) {
		return false;
	}
	bool ok = vk_fill(key, raw, rv, exp, delta);
	zend_string_release(raw);
	if (ok && vk_gen() != gen) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
	}
	return ok;
}

/* Valkey first for add (NX decides), else local first: a sibling's invalidation can then only remove our copy, never outlive it. */
static nocopy_status nocopy_vk_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	if (!vk_ready()) {
		return NOCOPY_NO_ROOM;
	}
	const nocopy_opts local = {.ttl = o->ttl}; /* over the local copy: Valkey decides add */
	if (!o->add_only && nocopy_put_local(key, val, &local, user) == NOCOPY_REFUSED) {
		return NOCOPY_REFUSED;
	}
	time_t expires = vk_expires(o->ttl);
	zend_string *enc = vk_encode(val, expires, user);
	if (!enc) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return NOCOPY_REFUSED;
	}
	int r = vk_set(ZSTR_VAL(key), ZSTR_LEN(key), ZSTR_VAL(enc), ZSTR_LEN(enc), vk_px(expires), o->add_only);
	zend_string_release(enc);
	if (r != 1) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return r == 0 ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
	}
	int64_t gone;
	if (o->add_only && nocopy_put_local(key, val, &local, user) == NOCOPY_REFUSED) {
		vk_unlink_n(1, &key, &gone); /* a resource: APCu refuses it */
		return NOCOPY_REFUSED;
	}
	return NOCOPY_OK;
}

/* nocopy_vk_store for many keys: local copies first, then every SET in one pipeline. */
static bool nocopy_vk_store_multi(HashTable *vals, const nocopy_opts *o)
{
	if (o->add_only) {
		return nocopy_store_each(vals, o); /* ponytail: add stays one round trip per key (NX decides first) */
	}
	if (!vk_ready()) {
		return false;
	}
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	uint32_t cap = zend_hash_num_elements(vals), m = 0;
	zend_string **keys = safe_emalloc(cap, sizeof *keys, 0), **enc = safe_emalloc(cap, sizeof *enc, 0);
	vk_kv *kv = safe_emalloc(cap, sizeof *kv, 0);
	time_t expires = vk_expires(o->ttl);
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = nocopy_key_of(k, h);
		zend_string *e = nocopy_put_local(key, v, o, 0) == NOCOPY_REFUSED ? NULL : vk_encode(v, expires, 0);
		if (!e) {
			nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
			all = false;
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
		vk_mset(m, kv, vk_px(expires), res);
	}
	for (uint32_t i = 0; i < m; i++) {
		if (res[i] != 1) {
			nocopy_drop_local(ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
			all = false;
		}
		zend_string_release(keys[i]);
		zend_string_release(enc[i]);
	}
	efree(res), efree(kv), efree(enc), efree(keys);
	return all;
}

static void nocopy_vk_fetch_multi(HashTable *keys, HashTable *out)
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
		uint64_t gen = vk_gen();
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
		if (got && vk_gen() != gen) {
			for (uint32_t i = 0; i < n; i++) {
				nocopy_drop_local(ZSTR_VAL(miss[i]), ZSTR_LEN(miss[i]));
			}
		}
		efree(raw);
	}
	efree(miss);
}

static bool nocopy_vk_drop(zend_string *key)
{
	int64_t gone = 0;
	int r = vk_ready() ? vk_unlink_n(1, &key, &gone) : -1;
	vk_forget(key);
	return r == 1 && gone > 0;
}

static zend_long nocopy_vk_drop_multi(HashTable *keys)
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

static bool nocopy_vk_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out)
{
	int64_t v = 0;
	int r = vk_ready() ? vk_incr(ZSTR_VAL(key), ZSTR_LEN(key), delta, (int64_t) MIN(ttl, NOCOPY_TTL_MAX) * 1000, &v) : -1;
	vk_forget(key); /* the next read fetches it */
	*out = (zend_long) v;
	return r == 1;
}

static bool nocopy_vk_cas_long(zend_string *key, zend_long old, zend_long nw)
{
	int r = vk_ready() ? vk_cas(ZSTR_VAL(key), ZSTR_LEN(key), old, nw) : -1;
	vk_forget(key);
	return r == 1;
}

static bool nocopy_vk_clear(void)
{
	bool ok = vk_ready() && vk_clear() >= 0; /* Valkey down: the other servers keep theirs */
	nocopy_clear();
	return ok;
}

static const nocopy_detour_ops vk_detour = {
	nocopy_vk_store, nocopy_vk_fetch, nocopy_vk_fetch_multi, nocopy_vk_store_multi, nocopy_vk_drop, nocopy_vk_drop_multi,
	nocopy_vk_add_long, nocopy_vk_cas_long, nocopy_vk_clear, true, "valkey",
};

const nocopy_detour_ops *nocopy_vk_route(void) { return vk_on ? &vk_detour : NULL; }
