/* nocopy: classes of graph nodes. How their objects are stored, what must not change between writer and reader, dates. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

/* ---- fingerprints ---- */

/* As serialize() treats the class; K_NONE: not as a node. */
static nc_kind class_kind(const zend_class_entry *ce)
{
	if (ce->ce_flags & ZEND_ACC_ENUM) {
		return K_ENUM;
	}
	bool data = ce->__serialize || ce->__unserialize;
	if ((ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS
			| ZEND_ACC_ANON_CLASS | ZEND_ACC_NOT_SERIALIZABLE))
		|| (data && (!ce->__serialize || !ce->__unserialize))
		|| (!data && (ce->serialize || ce->unserialize
			|| (ce->type != ZEND_USER_CLASS && ce != zend_standard_class_def) || ce->create_object || ce->default_object_handlers != &std_object_handlers))) {
		return K_NONE;
	}
	if (ce == php_date_get_date_ce() || ce == php_date_get_immutable_ce()) {
		return K_DATE;
	}
	if (data) {
		return K_DATA;
	}
	bool wake = zend_hash_exists(&ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP));
	if (zend_hash_str_exists(&ce->function_table, ZEND_STRL("__sleep"))) {
		return wake ? K_SLEEPW : K_SLEEP;
	}
	return wake ? K_WAKEUP : K_SLOTS;
}

/* Parents and interfaces: a typed property holding the object stays valid only if they did not change. */
static void fp_lineage(smart_str *b, const zend_class_entry *ce)
{
	for (const zend_class_entry *p = ce->parent; p; p = p->parent) {
		smart_str_appendc(b, 'P');
		smart_str_append(b, p->name);
	}
	for (uint32_t i = 0; i < ce->num_interfaces; i++) {
		smart_str_appendc(b, 'I');
		smart_str_append(b, ce->ce_flags & ZEND_ACC_RESOLVED_INTERFACES ? ce->interfaces[i]->name : ce->interface_names[i].name);
	}
}

/* The slots' names, visibility, readonly and types; false: their table is missing. */
static bool fp_slots(smart_str *b, const zend_class_entry *ce)
{
	if (ce->default_properties_count && !ce->properties_info_table) {
		return false;
	}
	for (int i = 0; i < ce->default_properties_count; i++) {
		zend_property_info *pi = ce->properties_info_table[i];
		smart_str_appendc(b, 0);
		if (!pi) { /* a hole: an inherited property redeclared moves to the parent's slot, its own stays UNDEF */
			smart_str_appendc(b, 1);
			continue;
		}
		smart_str_append(b, pi->name);
		smart_str_append_long(b, pi->flags & (ZEND_ACC_PPP_MASK | ZEND_ACC_PPP_SET_MASK | ZEND_ACC_READONLY));
		if (ZEND_TYPE_IS_SET(pi->type)) {
			zend_string *t = zend_type_to_string(pi->type);
			smart_str_append(b, t);
			zend_string_release(t);
		}
	}
	return true;
}

static uint64_t class_fp_compute(const zend_class_entry *ce)
{
	nc_kind kind = class_kind(ce);
	if (kind == K_NONE) {
		return 0;
	}
	smart_str b = {0};
	smart_str_append(&b, ce->name);
	smart_str_append_long(&b, kind);
	smart_str_append_long(&b, ce->default_properties_count);
	fp_lineage(&b, ce);
	if (kind_slots(kind) && !fp_slots(&b, ce)) {
		smart_str_free(&b);
		return 0;
	}
	smart_str_0(&b);
	uint64_t fp = (zend_hash_func(ZSTR_VAL(b.s), ZSTR_LEN(b.s)) & ~(uint64_t) K_MASK) | kind;
	smart_str_free(&b);
	return fp;
}

uint64_t nc_class_fp(nocopy_env *env, zend_class_entry *ce)
{
	zval *f = zend_hash_index_find(&env->classes, (zend_ulong) (uintptr_t) ce);
	if (f) {
		return (uint64_t) Z_LVAL_P(f);
	}
	zval z;
	ZVAL_LONG(&z, (zend_long) class_fp_compute(ce));
	zend_hash_index_add_new(&env->classes, (zend_ulong) (uintptr_t) ce, &z);
	return (uint64_t) Z_LVAL(z);
}

zend_class_entry *nc_class_resolve(nocopy_env *env, const nc_class *c)
{
	zend_class_entry *ce = zend_hash_find_ptr(EG(class_table), c->lc);
	if (!ce) {
		zend_string *n = zend_string_init(ZSTR_VAL(c->name), ZSTR_LEN(c->name), 0); /* autoloaders may keep it */
		ce = zend_lookup_class(n);
		zend_string_release(n);
	}
	if (!ce || EG(exception) || nc_class_fp(env, ce) != c->fp || kind_nprops(ce, fp_kind(c->fp)) != c->nprops) {
		return NULL;
	}
	if (!(ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED) && zend_update_class_constants(ce) != SUCCESS) {
		return NULL;
	}
	return ce;
}

bool nc_call_magic(zend_function *f, zend_object *o, zval *rv)
{
	BG(serialize_lock)++;
	zend_call_known_instance_method_with_0_params(f, o, rv);
	BG(serialize_lock)--;
	if (EG(exception)) {
		zval_ptr_dtor(rv);
		return false;
	}
	return true;
}

/* ---- dates: timestamp and zone, not __serialize()'s string (formatting and parsing were ~95% of their cost) ---- */

bool nc_date_native(zend_object *o)
{
	const timelib_time *t = php_date_obj_from_obj(o)->time;
	if (!t || !t->is_localtime || t->zone_type < TIMELIB_ZONETYPE_OFFSET || t->zone_type > TIMELIB_ZONETYPE_ID) {
		return false;
	}
	if (o->properties) {
		zval *v;
		ZEND_HASH_FOREACH_VAL(o->properties, v) {
			if (Z_TYPE_P(v) != IS_INDIRECT) {
				return false; /* dynamic properties: __serialize() would add them */
			}
		} ZEND_HASH_FOREACH_END();
	}
	return true;
}

bool nc_date_zone_name(zend_object *o, zval *z)
{
	const timelib_time *t = php_date_obj_from_obj(o)->time;
	const char *s = t->zone_type == TIMELIB_ZONETYPE_ID ? t->tz_info->name : t->zone_type == TIMELIB_ZONETYPE_ABBR ? t->tz_abbr : NULL;
	if (!s) {
		ZVAL_NULL(z);
		return false;
	}
	ZVAL_STRING(z, s);
	return true;
}

/* The zone as ext/date resolves it (its tzdb, its cache), cached by name per request. NULL: not in the tzdb anymore. */
static timelib_tzinfo *date_tz(nocopy_env *env, const zend_string *name)
{
	timelib_tzinfo *tz = zend_hash_str_find_ptr(&env->zones, ZSTR_VAL(name), ZSTR_LEN(name));
	if (tz) {
		return tz;
	}
	zval o, n;
	object_init_ex(&o, php_date_get_timezone_ce());
	ZVAL_STRINGL(&n, ZSTR_VAL(name), ZSTR_LEN(name));
	zend_call_known_instance_method_with_1_params(Z_OBJCE(o)->constructor, Z_OBJ(o), NULL, &n);
	zval_ptr_dtor(&n);
	if (EG(exception)) {
		zend_clear_exception();
	} else if (Z_PHPTIMEZONE_P(&o)->type == TIMELIB_ZONETYPE_ID) {
		tz = Z_PHPTIMEZONE_P(&o)->tzi.tz;
		zend_hash_str_add_new_ptr(&env->zones, ZSTR_VAL(name), ZSTR_LEN(name), tz);
	}
	zval_ptr_dtor(&o);
	return tz;
}

/* Built by ext/date's own calls (timelib is not exported): UTC from the timestamp, then date_timezone_set() with the stored zone. */
bool nc_date_out(nocopy_env *env, zend_object *obj, const nc_obj *n)
{
	static zend_function *tz_set;
	uint64_t w = (uint64_t) Z_LVAL(n->props[1]);
	int type = date_word_type(w);
	int32_t z = date_word_offset(w);
	zend_object *d = obj; /* date_timezone_set() takes only a DateTime: an immutable one is built in one and takes its time */
	if (obj->ce != php_date_get_date_ce()) {
		zval t;
		object_init_ex(&t, php_date_get_date_ce());
		d = Z_OBJ(t);
	}
	php_date_initialize_from_ts_long(php_date_obj_from_obj(d), Z_LVAL(n->props[0]), date_word_us(w));
	bool ok = true;
	if (type != TIMELIB_ZONETYPE_OFFSET || z) {
		zval args[2], rv;
		object_init_ex(&args[1], php_date_get_timezone_ce());
		php_timezone_obj *tz = Z_PHPTIMEZONE_P(&args[1]);
		if (type == TIMELIB_ZONETYPE_ID) {
			ok = (tz->tzi.tz = date_tz(env, Z_STR(n->props[2]))) != NULL;
		} else if (type == TIMELIB_ZONETYPE_ABBR) {
			tz->tzi.z.utc_offset = z;
			tz->tzi.z.dst = date_word_dst(w);
			tz->tzi.z.abbr = timelib_strdup(Z_STRVAL(n->props[2]));
		} else {
			tz->tzi.utc_offset = z;
		}
		tz->type = type;
		tz->initialized = ok;
		if (ok) {
			if (!tz_set) {
				tz_set = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("date_timezone_set"));
			}
			ZVAL_OBJ(&args[0], d);
			zend_call_known_function(tz_set, NULL, NULL, &rv, 2, args, NULL);
			zval_ptr_dtor(&rv);
			ok = !EG(exception);
		}
		zval_ptr_dtor(&args[1]);
	}
	if (d != obj) {
		php_date_obj_from_obj(obj)->time = php_date_obj_from_obj(d)->time;
		php_date_obj_from_obj(d)->time = NULL;
		OBJ_RELEASE(d);
	}
	return ok;
}
