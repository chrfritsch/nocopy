/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: bb7181be4835ce4b5d492a56cd5d6a49cd264f12 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_put, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, serialize, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_get, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(1, hit, IS_MIXED, 0, "null")
ZEND_END_ARG_INFO()

#define arginfo_nocopy_add arginfo_nocopy_put

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_nocopy_inc, 0, 1, MAY_BE_LONG|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, step, IS_LONG, 0, "1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_cas, 0, 3, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, old, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, new, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_clear, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_delete, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_get_multiple, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, keys, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_put_multiple, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, values, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, serialize, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_remember, 0, 2, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, beta, IS_DOUBLE, 0, "1.0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_shared_with, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()


ZEND_FRAMELESS_FUNCTION(nocopy_put, 2);
ZEND_FRAMELESS_FUNCTION(nocopy_put, 3);
static const zend_frameless_function_info frameless_function_infos_nocopy_put[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_put, 2), 2 },
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_put, 3), 3 },
	{ 0 },
};

ZEND_FRAMELESS_FUNCTION(nocopy_get, 1);
static const zend_frameless_function_info frameless_function_infos_nocopy_get[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_get, 1), 1 },
	{ 0 },
};

ZEND_FUNCTION(nocopy_put);
ZEND_FUNCTION(nocopy_get);
ZEND_FUNCTION(nocopy_add);
ZEND_FUNCTION(nocopy_inc);
ZEND_FUNCTION(nocopy_cas);
ZEND_FUNCTION(nocopy_clear);
ZEND_FUNCTION(nocopy_delete);
ZEND_FUNCTION(nocopy_get_multiple);
ZEND_FUNCTION(nocopy_put_multiple);
ZEND_FUNCTION(nocopy_remember);
ZEND_FUNCTION(nocopy_shared_with);
ZEND_FUNCTION(nocopy_stats);

static const zend_function_entry ext_functions[] = {
	ZEND_RAW_FENTRY("nocopy_put", zif_nocopy_put, arginfo_nocopy_put, 0, frameless_function_infos_nocopy_put, NULL)
	ZEND_RAW_FENTRY("nocopy_get", zif_nocopy_get, arginfo_nocopy_get, 0, frameless_function_infos_nocopy_get, NULL)
	ZEND_FE(nocopy_add, arginfo_nocopy_add)
	ZEND_FE(nocopy_inc, arginfo_nocopy_inc)
	ZEND_FE(nocopy_cas, arginfo_nocopy_cas)
	ZEND_FE(nocopy_clear, arginfo_nocopy_clear)
	ZEND_FE(nocopy_delete, arginfo_nocopy_delete)
	ZEND_FE(nocopy_get_multiple, arginfo_nocopy_get_multiple)
	ZEND_FE(nocopy_put_multiple, arginfo_nocopy_put_multiple)
	ZEND_FE(nocopy_remember, arginfo_nocopy_remember)
	ZEND_FE(nocopy_shared_with, arginfo_nocopy_shared_with)
	ZEND_FE(nocopy_stats, arginfo_nocopy_stats)
	ZEND_FE_END
};
