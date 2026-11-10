/* nocopy: zero-copy shared-memory store. This file: nocopy_*, the module. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#if PHP_VERSION_ID < 80400
# error "nocopy needs PHP 8.4 or later"
#endif
#include "ext/standard/info.h"
#include "SAPI.h"
#include "zend_system_id.h"
#include "zend_extensions.h"
#include "zend_closures.h"
#include "nocopy_arginfo.h"
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#ifdef __APPLE__
# include <mach-o/loader.h>
#elif defined(__linux__)
# include <link.h>
#endif

/* One translation unit, so nocopy_* inlines the store and value layers (split: miss -5%, failing add -6%); each also compiles alone. */
#include "nocopy_class.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_serial.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_plan.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_write.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_read.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_value.c" /* NOLINT(bugprone-suspicious-include) */ /* last: the phases defined before the entry points call them */
#include "nocopy_kept.c" /* NOLINT(bugprone-suspicious-include) */
#include "nocopy_store.c" /* NOLINT(bugprone-suspicious-include) */

ZEND_DECLARE_MODULE_GLOBALS(nocopy)

static size_t arena_size = 32 * 1024 * 1024;
static zend_long ini_hint = 4096;
static char *shm_name; /* nocopy.shm_name: required, the arena every process attaches to */
static char shm_attached[64]; /* the object mapped; empty: none (nocopy_attached() false: unavailable) */
static char off_why[400] = "the module is not started"; /* why this process has no arena */
static const nocopy_detour_ops *proc_route = &nocopy_no_arena; /* this process's: the local store (NULL), Valkey, or off */
static const char no_slot[] = "no worker slot: 1024 processes and threads use this cache, or this process had no file descriptor left for its liveness lock";
static bool sapi_zero_copy;
static zend_long zero_copy_seconds = 3; /* a zero-copy request older than this copies: a slow backend must not pin the cache away */
static bool open_deferred; /* fpm master as root: each worker opens the arena on its first request */
static bool want_igbinary; /* nocopy.serializer=igbinary: in effect if igbinary is loaded (MINIT) */
static zif_handler orig_handle_request; /* FrankenPHP's frankenphp_handle_request, wrapped */

/* ---- nocopy_* ---- */

/* false (thrown): a negative ttl in argument arg. */
static bool write_ok(uint32_t arg, zend_long ttl)
{
	if (ttl < 0) {
		zend_argument_value_error(arg, "must be greater than or equal to 0");
		return false;
	}
	return true;
}

static void put_impl(zend_string *key, zval *val, zend_long ttl, bool add_only, zval *return_value)
{
	if (!write_ok(3, ttl)) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .add_only = add_only};
	nocopy_status st = nocopy_store(key, val, &o);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(st == NOCOPY_OK);
}

/* nocopy_put / nocopy_add (string $key, mixed $value, int $ttl = 0) */
static void put_fn(INTERNAL_FUNCTION_PARAMETERS, bool add_only)
{
	zend_string *key;
	zval *val;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	put_impl(key, val, ttl, add_only, return_value);
}

PHP_FUNCTION(nocopy_put) { put_fn(INTERNAL_FUNCTION_PARAM_PASSTHRU, false); }
PHP_FUNCTION(nocopy_add) { put_fn(INTERNAL_FUNCTION_PARAM_PASSTHRU, true); }

PHP_FUNCTION(nocopy_inc)
{
	zend_string *key;
	zend_long step = 1, ttl = 0, nv;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(step)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (!write_ok(3, ttl)) {
		RETURN_THROWS();
	}
	if (!nocopy_add_long(key, step, ttl, &nv)) {
		RETURN_FALSE;
	}
	RETURN_LONG(nv);
}

PHP_FUNCTION(nocopy_cas)
{
	zend_string *key;
	zend_long old, nw;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(old)
		Z_PARAM_LONG(nw)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(nocopy_cas_long(key, old, nw));
}

PHP_FUNCTION(nocopy_clear)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(nocopy_clear_all());
}

PHP_FUNCTION(nocopy_get)
{
	zend_string *key;
	zval *hit = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(hit)
	ZEND_PARSE_PARAMETERS_END();

	bool found = nocopy_fetch(key, return_value);
	if (hit) {
		ZEND_TRY_ASSIGN_REF_BOOL(hit, found);
	}
}

/* Frameless variants (PHP 8.4+): a call the compiler resolved skips the VM's call frame. */
ZEND_FRAMELESS_FUNCTION(nocopy_get, 1)
{
	zval tmp;
	zend_string *key;
	Z_FLF_PARAM_STR(1, key, tmp);
	nocopy_fetch(key, return_value);
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}

ZEND_FRAMELESS_FUNCTION(nocopy_put, 2)
{
	zval tmp;
	zend_string *key;
	Z_FLF_PARAM_STR(1, key, tmp);
	put_impl(key, arg2, 0, false, return_value);
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}

ZEND_FRAMELESS_FUNCTION(nocopy_put, 3)
{
	zval tmp;
	zend_string *key;
	zend_long ttl;
	Z_FLF_PARAM_STR(1, key, tmp);
	Z_FLF_PARAM_LONG(3, ttl);
	put_impl(key, arg2, ttl, false, return_value);
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}

PHP_FUNCTION(nocopy_delete)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(nocopy_drop(key));
}

PHP_FUNCTION(nocopy_delete_multiple)
{
	HashTable *keys;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(keys)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(nocopy_drop_multi(keys));
}

PHP_FUNCTION(nocopy_get_multiple)
{
	HashTable *keys;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(keys)
	ZEND_PARSE_PARAMETERS_END();

	array_init_size(return_value, zend_hash_num_elements(keys));
	nocopy_fetch_multi(keys, Z_ARRVAL_P(return_value));
}

PHP_FUNCTION(nocopy_put_multiple)
{
	HashTable *vals;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ARRAY_HT(vals)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (!write_ok(2, ttl)) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl};
	bool ok = nocopy_store_multi(vals, &o);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(ok);
}

PHP_FUNCTION(nocopy_remember)
{
	zend_string *key;
	zval *fn;
	zend_long ttl = 0;
	double beta = 1.0;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(fn) /* not Z_PARAM_FUNC: resolving it was ~25% of a hit (perf), and a hit never calls it */
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
		Z_PARAM_DOUBLE(beta)
	ZEND_PARSE_PARAMETERS_END();

	if (!write_ok(3, ttl)) {
		RETURN_THROWS();
	}
	if (Z_TYPE_P(fn) != IS_OBJECT || Z_OBJCE_P(fn) != zend_ce_closure) {
		char *err = NULL;
		bool ok = zend_is_callable_ex(fn, NULL, 0, NULL, NULL, &err);
		if (!ok) {
			zend_argument_type_error(2, "must be a valid callback, %s", err);
		}
		if (err) {
			efree(err);
		}
		if (!ok) {
			RETURN_THROWS();
		}
	}
	const nocopy_opts o = {.ttl = ttl};
	nocopy_remember(key, fn, &o, beta, return_value);
	if (EG(exception)) {
		RETURN_THROWS();
	}
}

PHP_FUNCTION(nocopy_keys)
{
	zend_string *prefix = ZSTR_EMPTY_ALLOC();

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR(prefix)
	ZEND_PARSE_PARAMETERS_END();

	array_init(return_value);
	nocopy_list_keys(prefix, Z_ARRVAL_P(return_value));
}

PHP_FUNCTION(nocopy_shared_with)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING(NOCOPY_G(detour) ? NOCOPY_G(detour)->name : "host");
}

PHP_FUNCTION(nocopy_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();
	nocopy_stats_t s = {0};
	if (nocopy_attached()) {
		nocopy_stats(&s);
	}
	array_init(return_value);
	add_assoc_long(return_value, "size", (zend_long) nocopy_arena_size());
	add_assoc_long(return_value, "used", s.used);
	add_assoc_long(return_value, "segment_size", s.segment_size);
	add_assoc_long(return_value, "segments", s.segments);
	add_assoc_long(return_value, "free_segments", s.free_segments);
	add_assoc_long(return_value, "dead_segments", s.dead_segments);
	add_assoc_long(return_value, "pinned_segments", s.pinned_segments);
	add_assoc_long(return_value, "kept_segments", s.kept_segments);
	add_assoc_long(return_value, "entries", s.entries);
	add_assoc_long(return_value, "hits", s.hits);
	add_assoc_long(return_value, "misses", s.misses);
	add_assoc_long(return_value, "evicted", s.evicted);
	add_assoc_long(return_value, "expired", s.expired);
	add_assoc_long(return_value, "ghost_hits", s.ghost_hits);
	add_assoc_long(return_value, "evacuated_bytes", s.evacuated);
	add_assoc_long(return_value, "shards", s.shards);
	add_assoc_long(return_value, "slots", s.slots);
	add_assoc_long(return_value, "expunges", s.expunges);
	add_assoc_long(return_value, "lock_steals", s.lock_steals);
	add_assoc_long(return_value, "leases", s.leases);
	add_assoc_long(return_value, "shared_strings", s.atoms);
	add_assoc_long(return_value, "shared_strings_used", s.atoms_used);
	add_assoc_long(return_value, "shared_strings_size", s.atoms_size);
	add_assoc_long(return_value, "shared_strings_rotations", s.atom_rotations);
	if (shm_attached[0]) {
		add_assoc_string(return_value, "shm_name", shm_attached);
	} else {
		add_assoc_null(return_value, "shm_name");
	}
	add_assoc_long(return_value, "valkey_listener", nocopy_vk_listener());
	if (NOCOPY_G(off)) {
		add_assoc_string(return_value, "off", (char *) NOCOPY_G(off));
	} else {
		add_assoc_null(return_value, "off");
	}
}

/* ---- FrankenPHP worker mode ---- */

/* frankenphp_handle_request() serves many requests inside one worker script, which may keep values across them: copy mode. */
static ZEND_NAMED_FUNCTION(nocopy_handle_request)
{
	NOCOPY_G(zero_copy) = false; /* values fetched while booting stay pinned until the worker ends */
	nocopy_vk_new_request();
	nocopy_drop_idle(&NOCOPY_G(ctx)); /* the worker now waits for a request */
	orig_handle_request(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* ---- module ---- */

static uint64_t abi_id; /* layout_id(), once in MINIT */

#ifdef __linux__
typedef struct {
	uintptr_t at;
	uint64_t id;
} build_find;

/* The GNU build-id note of the loaded object that holds f->at. */
static int build_note(struct dl_phdr_info *info, size_t size, void *arg)
{
	(void) size;
	build_find *f = arg;
	bool ours = false;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		ours |= ph->p_type == PT_LOAD && f->at - (info->dlpi_addr + ph->p_vaddr) < ph->p_memsz;
	}
	for (int i = 0; ours && i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		const char *p = (const char *) (info->dlpi_addr + ph->p_vaddr), *end = p + ph->p_memsz;
		while (ph->p_type == PT_NOTE && p + sizeof(ElfW(Nhdr)) <= end) {
			const ElfW(Nhdr) *n = (const ElfW(Nhdr) *) p;
			const char *name = p + sizeof *n, *desc = name + ((n->n_namesz + 3) & ~3u);
			if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && memcmp(name, "GNU", 4) == 0 && desc + n->n_descsz <= end) {
				f->id = zend_hash_func(desc, n->n_descsz);
				return 1;
			}
			p = desc + ((n->n_descsz + 3) & ~3u);
		}
	}
	return ours;
}
#endif

/* The id the linker gave the binary that holds this code (in memory, not the file on disk now): 0 if it has none. */
static uint64_t build_id(void)
{
#ifdef __linux__
	build_find f = {.at = (uintptr_t) build_id};
	dl_iterate_phdr(build_note, &f);
	return f.id;
#elif defined(__APPLE__)
	Dl_info dl;
	if (!dladdr((void *) build_id, &dl) || !dl.dli_fbase) {
		return 0;
	}
	const struct mach_header_64 *h = dl.dli_fbase;
	const struct load_command *lc = (const struct load_command *) (h + 1);
	for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *) ((const char *) lc + lc->cmdsize)) {
		if (lc->cmd == LC_UUID) {
			return zend_hash_func((const char *) ((const struct uuid_command *) lc)->uuid, 16);
		}
	}
	return 0;
#else
	return 0;
#endif
}

/* What processes sharing a named arena must agree on: the code that lays out and reads it. The same binary gives the same id; any other build gets its own arena. */
static uint64_t layout_id(void)
{
	uint64_t id = zend_hash_func(ZEND_STRL(PHP_NOCOPY_VERSION " " ZEND_EXTENSION_BUILD_ID)), b = build_id();
	if (b) {
		return id ^ b;
	}
#ifdef COMPILE_DL_NOCOPY
	/* no build id: hash the .so, now, while it is the file just loaded */
	Dl_info dl;
	int fd = dladdr((void *) layout_id, &dl) && dl.dli_fname ? open(dl.dli_fname, O_RDONLY | O_CLOEXEC) : -1;
	struct stat st;
	void *m = fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	if (fd >= 0) {
		close(fd);
	}
	if (m != MAP_FAILED) {
		id ^= zend_hash_func(m, (size_t) st.st_size);
		munmap(m, (size_t) st.st_size);
		return id;
	}
#endif
	return id ^ zend_hash_func(ZEND_STRL(__DATE__ " " __TIME__)); /* built into PHP without a build id, or our .so unreadable */
}

static ZEND_INI_MH(OnShmName)
{
	shm_name = ZSTR_LEN(new_value) ? ZSTR_VAL(new_value) : NULL;
	return SUCCESS;
}

/* Zero-copy only under SAPIs whose requests are short: values cannot outlive the request. */
static bool short_lived_sapi(void)
{
	static const char *const names[] = {"fpm-fcgi", "cgi-fcgi", "apache2handler", "apache2filter", "litespeed", "fcgi", "cli-server", "frankenphp", NULL};
	for (int i = 0; names[i]; i++) {
		if (strcmp(sapi_module.name, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* A byte size (K, M, G) into the size_t at mh_arg1. */
static ZEND_INI_MH(OnSize)
{
	*(size_t *) mh_arg1 = (size_t) zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnHint)
{
	ini_hint = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnZeroCopySeconds)
{
	zero_copy_seconds = ZEND_STRTOL(ZSTR_VAL(new_value), NULL, 10);
	return SUCCESS;
}

static ZEND_INI_MH(OnSerializer)
{
	want_igbinary = zend_string_equals_literal_ci(new_value, "igbinary");
	if (!want_igbinary && !zend_string_equals_literal_ci(new_value, "php") && !zend_string_equals_literal_ci(new_value, "default")) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: unknown nocopy.serializer \"%s\", using php", ZSTR_VAL(new_value));
	}
	return SUCCESS;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY1("nocopy.shm_size", "32M", PHP_INI_SYSTEM, OnSize, &arena_size)
	PHP_INI_ENTRY("nocopy.entries_hint", "4096", PHP_INI_SYSTEM, OnHint)
	PHP_INI_ENTRY("nocopy.serializer", "php", PHP_INI_SYSTEM, OnSerializer)
	PHP_INI_ENTRY1("nocopy.local_size", "8M", PHP_INI_SYSTEM, OnSize, &nocopy_kept_size)
	PHP_INI_ENTRY("nocopy.shm_name", "", PHP_INI_SYSTEM, OnShmName)
	PHP_INI_ENTRY("nocopy.zero_copy_seconds", "3", PHP_INI_SYSTEM, OnZeroCopySeconds)
PHP_INI_END()

/* Read by apcu_* callers (Symfony's ApcuAdapter, Composer) before they use the apcu polyfill; nocopy itself ignores them. */
static const zend_ini_entry_def ini_apc[] = {
	PHP_INI_ENTRY("apc.enabled", "1", PHP_INI_SYSTEM, NULL)
	PHP_INI_ENTRY("apc.enable_cli", "0", PHP_INI_SYSTEM, NULL)
	{0}
};

/* MINIT only: set an entry's value (and apply it) without zend_alter_ini_entry's runtime bookkeeping, which a request then undoes. */
static void ini_put(const char *name, zend_string *value, bool apply)
{
	zend_ini_entry *e = zend_hash_str_find_ptr(EG(ini_directives), name, strlen(name));
	if (apply && e->on_modify) {
		e->on_modify(e, value, e->mh_arg1, e->mh_arg2, e->mh_arg3, ZEND_INI_STAGE_STARTUP);
	}
	zend_string *old = e->value;
	e->value = zend_string_copy(value);
	if (old) {
		zend_string_release_ex(old, 1);
	}
}

/* ini_get() then shows the serializer in effect. */
static void resolve_serializer(void)
{
	bool igb = nocopy_serial_setup(want_igbinary);
	if (want_igbinary && !igb) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: igbinary is not loaded, using nocopy.serializer=php");
	}
	zend_string *v = zend_string_init(igb ? "igbinary" : "php", igb ? 8 : 3, 1);
	ini_put("nocopy.serializer", v, false);
	zend_string_release_ex(v, 1);
}

static PHP_GINIT_FUNCTION(nocopy)
{
#if defined(COMPILE_DL_NOCOPY) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(nocopy_globals, 0, sizeof(*nocopy_globals));
	nocopy_globals->detour = &nocopy_no_arena; /* off until a worker slot is claimed */
	nocopy_globals->off = off_why;
	nocopy_ctx_init(&nocopy_globals->ctx);
	nocopy_env_init(&nocopy_globals->env);
}

/* A thread ending (ZTS) gives its worker slot back. */
static PHP_GSHUTDOWN_FUNCTION(nocopy)
{
	if (nocopy_attached()) {
		nocopy_release(&nocopy_globals->ctx);
	}
	nocopy_env_free(&nocopy_globals->env);
	nocopy_vk_close(&nocopy_globals->vk);
}

/* Opens the arena of nocopy.shm_name. late: in a worker's first request, problems go to the log. */
static void arena_open(bool late)
{
	nocopy_err err = {.code = NOCOPY_ERR_NONE};
	nocopy_named named;
	char msg[512] = "";
	if (shm_name) {
		const nocopy_config cfg = {.name = shm_name, .size = arena_size, .hint = ini_hint > 0 ? (uint64_t) ini_hint : 0, .abi = abi_id, .rebase = nocopy_value_rebase};
		err = nocopy_arena_open(&cfg, &named);
	}
	off_why[0] = 0;
	if (!shm_name) {
		snprintf(off_why, sizeof off_why, "nocopy.shm_name is not set (e.g. /nocopy)");
	} else if (err.code == NOCOPY_ERR_TOO_SMALL) {
		snprintf(off_why, sizeof off_why, "%zu bytes needed for the slot tables and a few segments: nocopy.shm_size is too small or nocopy.entries_hint too large", err.need);
	} else if (err.code != NOCOPY_ERR_NONE) {
		/* not a private cache instead: its writes and deletes would never reach the processes sharing the named one */
		snprintf(off_why, sizeof off_why, "nocopy.shm_name %s: %s", shm_name, err.msg);
	} else {
		memcpy(shm_attached, named.name, sizeof shm_attached);
		if (named.other[0]) {
			snprintf(msg, sizeof msg, "nocopy: %s, the nocopy.shm_name cache of another PHP or nocopy build, is in use: its processes do not share this cache", named.other);
		} else if (named.size != arena_size || named.slots_differ) {
			snprintf(msg, sizeof msg, "nocopy: nocopy.shm_size or nocopy.entries_hint ignored: %s was created with %zu bytes and other slot tables by a process still using it. Stop every process using it (CLI included) to apply them", named.name, named.size);
		}
	}
	if (off_why[0]) {
		snprintf(msg, sizeof msg, "nocopy: %s. The cache is off in this process: fetches miss, writes and clears fail", off_why);
	}
	if (msg[0]) {
		if (late) {
			php_log_err_with_severity(msg, LOG_WARNING);
		} else {
			php_error_docref(NULL, E_CORE_WARNING, "%s", msg);
		}
	}
	if (nocopy_attached()) {
		nocopy_value_attach(&NOCOPY_SHARED->value);
	}
}

/* This thread's route: the process's, or off without a worker slot. */
static void route_thread(void)
{
	bool on = nocopy_attached() && nocopy_ctx_slot(&NOCOPY_G(ctx)) >= 0;
	NOCOPY_G(detour) = on ? proc_route : &nocopy_no_arena;
	NOCOPY_G(off) = on ? NULL : nocopy_attached() ? no_slot : off_why;
}

/* Where the store ops go: the local store (NULL), else no arena, else Valkey. */
static void route(void)
{
	proc_route = nocopy_attached() ? nocopy_vk_route() : &nocopy_no_arena;
	route_thread();
}

static void log_no_slot(void)
{
	char msg[300];
	snprintf(msg, sizeof msg, "nocopy: %s. The cache is off in this process: fetches miss, writes and clears fail", no_slot);
	php_log_err_with_severity(msg, LOG_WARNING);
}

/* A worker slot for this thread, tried again each request; one log line when there is none. */
static void claim(void)
{
	if (nocopy_attached() && !nocopy_claim(&NOCOPY_G(ctx)) && NOCOPY_G(off) != no_slot) {
		log_no_slot();
	}
	route_thread();
}

static void atfork_child(void)
{
	if (nocopy_attached() && !nocopy_after_fork(&NOCOPY_G(ctx)) && NOCOPY_G(off) != no_slot) {
		log_no_slot();
	}
	route_thread();
	nocopy_vk_close(&NOCOPY_G(vk));
}

PHP_MINIT_FUNCTION(nocopy)
{
	REGISTER_INI_ENTRIES();
	abi_id = layout_id(); /* now: a root fpm master's workers open the arena later, maybe after nocopy.so was replaced */
	sapi_zero_copy = short_lived_sapi();
	if (!zend_hash_str_exists(&module_registry, ZEND_STRL("apcu"))) {
		zend_register_ini_entries(ini_apc, module_number);
	}
	resolve_serializer();

	/* fpm as root: workers would share root's mapping, and root would read what they wrote; open it in the worker */
	open_deferred = strcmp(sapi_module.name, "fpm-fcgi") == 0 && geteuid() == 0;
	if (!open_deferred) {
		arena_open(false);
	}
	nocopy_vk_minit(module_number);
	route();
	pthread_atfork(NULL, NULL, atfork_child);

	zend_function *hr = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("frankenphp_handle_request"));
	if (hr && hr->type == ZEND_INTERNAL_FUNCTION) {
		orig_handle_request = hr->internal_function.handler;
		hr->internal_function.handler = nocopy_handle_request;
	}
	/* opcache's file cache stores frameless handler indices: another extension set must not reuse them */
	size_t n = 0; /* zend_flf_count is not exported */
	while (zend_flf_handlers && zend_flf_handlers[n]) {
		n++;
	}
	zend_add_system_entropy("nocopy", "frameless", &n, sizeof(n));
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(nocopy)
{
	nocopy_release(&NOCOPY_G(ctx));
	nocopy_arena_close();
	UNREGISTER_INI_ENTRIES();
	nocopy_vk_mshutdown();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(nocopy)
{
#if defined(COMPILE_DL_NOCOPY) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	if (UNEXPECTED(open_deferred)) {
		open_deferred = false;
		arena_open(true);
		route();
	}
	if (nocopy_ctx_slot(&NOCOPY_G(ctx)) < 0) {
		claim();
	}
	NOCOPY_G(zero_copy) = sapi_zero_copy;
	nocopy_keep_until(&NOCOPY_G(ctx), sapi_zero_copy && zero_copy_seconds > 0 ? nocopy_mono_ms() + zero_copy_seconds * 1000 : 0);
	nocopy_kept_rinit(&NOCOPY_G(kept));
	nocopy_vk_new_request();
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(nocopy)
{
	NOCOPY_G(zero_copy) = false;
	nocopy_kept_rshutdown(&NOCOPY_G(kept));
	return SUCCESS;
}

/* Pins go only after zend_deactivate: built-in modules (session) still read user values in their RSHUTDOWN, which runs after ours. */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(nocopy)
{
	nocopy_leases_drop();
	nocopy_request_end(&NOCOPY_G(ctx));
	nocopy_env_request_end(&NOCOPY_G(env));
	return SUCCESS;
}

PHP_MINFO_FUNCTION(nocopy)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "nocopy support", "enabled");
#ifdef ZTS
	php_info_print_table_row(2, "thread safety", "enabled");
#endif
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

static const zend_module_dep nocopy_deps[] = {
	ZEND_MOD_OPTIONAL("igbinary") /* starts first, so its symbols resolve in our MINIT */
	ZEND_MOD_END
};

zend_module_entry nocopy_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	nocopy_deps,
	"nocopy",
	ext_functions,
	PHP_MINIT(nocopy),
	PHP_MSHUTDOWN(nocopy),
	PHP_RINIT(nocopy),
	PHP_RSHUTDOWN(nocopy),
	PHP_MINFO(nocopy),
	PHP_NOCOPY_VERSION,
	PHP_MODULE_GLOBALS(nocopy),
	PHP_GINIT(nocopy),
	PHP_GSHUTDOWN(nocopy),
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(nocopy),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_NOCOPY
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nocopy)
#endif
