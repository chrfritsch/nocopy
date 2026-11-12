/* nocopy: serialized values. What the arena cannot hold as a graph, and the RESP tier's encoding: serialize() or igbinary. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include <dlfcn.h>
#ifndef RTLD_DEFAULT
# define RTLD_DEFAULT ((void *) 0)
#endif

typedef int (*igb_ser_fn)(uint8_t **ret, size_t *ret_len, zval *z);
typedef int (*igb_unser_fn)(const uint8_t *buf, size_t buf_len, zval *z);

static igb_ser_fn igb_ser;     /* NULL: serialize() */
static igb_unser_fn igb_unser; /* NULL: igbinary values are misses */

/* igbinary's C API, found at runtime: no build dependency. */
bool nocopy_serial_setup(bool igbinary)
{
	igb_ser = NULL;
	igb_unser = NULL;
	if (!igbinary) {
		return false;
	}
	zend_module_entry *m = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("igbinary"));
	if (!m) {
		return false;
	}
	void *h = m->handle ? m->handle : RTLD_DEFAULT;
	igb_ser_fn s = (igb_ser_fn) DL_FETCH_SYMBOL(h, "igbinary_serialize");
	igb_unser_fn u = (igb_unser_fn) DL_FETCH_SYMBOL(h, "igbinary_unserialize");
	if (!s || !u) {
		return false;
	}
	igb_ser = s;
	igb_unser = u;
	return true;
}

bool nocopy_serialize(zval *val, zval *out, nocopy_fmt *fmt)
{
	if (igb_ser) {
		*fmt = NOCOPY_FMT_IGB;
		uint8_t *b;
		size_t len;
		if (igb_ser(&b, &len, val) != 0 || EG(exception)) {
			return false;
		}
		ZVAL_STRINGL(out, (char *) b, len);
		efree(b);
		return true;
	}
	*fmt = NOCOPY_FMT_PHP;
	smart_str buf = {0};
	BG(serialize_lock)++;
	php_serialize_data_t h = php_var_serialize_init();
	php_var_serialize(&buf, val, &h);
	php_var_serialize_destroy(h);
	BG(serialize_lock)--;
	if (EG(exception)) {
		smart_str_free(&buf);
		return false;
	}
	smart_str_0(&buf);
	ZVAL_STR(out, buf.s);
	return true;
}

/* With a fresh state: php_unserialize_with_options() would share the outer unserialize()'s
 * reference table when called re-entrantly (e.g. from Serializable::unserialize). */
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, nocopy_fmt fmt)
{
	bool ok;
	ZVAL_NULL(rv); /* a rejected input may leave it unwritten: the failure path frees it */
	if (fmt == NOCOPY_FMT_IGB) {
		ok = igb_unser && igb_unser((const uint8_t *) buf, len, rv) == 0;
	} else {
		const unsigned char *p = (const unsigned char *) buf;
		BG(serialize_lock)++; /* forces a private state instead of the outer call's */
		php_unserialize_data_t h = php_var_unserialize_init();
		ok = php_var_unserialize(rv, &p, p + len, &h);
		php_var_unserialize_destroy(h);
		BG(serialize_lock)--;
	}
	if (!ok || EG(exception)) {
		zval_ptr_dtor(rv);
		ZVAL_NULL(rv);
		return false;
	}
	if (Z_ISREF_P(rv)) { /* a top-level cycle comes back as a reference; functions must not return one */
		zend_unwrap_reference(rv);
	}
	return true;
}
