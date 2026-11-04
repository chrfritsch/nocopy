/* This is a generated file, edit stash.stub.php instead.
 * Stub hash: fb829f9e6ccc0a6d7320e586ffb0ac33f35cb495 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stash_put, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, serialize, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stash_get, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(1, hit, IS_MIXED, 0, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stash_delete, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stash_remember, 0, 2, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, beta, IS_DOUBLE, 0, "1.0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stash_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()


#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(stash_put, 2);
ZEND_FRAMELESS_FUNCTION(stash_put, 3);
static const zend_frameless_function_info frameless_function_infos_stash_put[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(stash_put, 2), 2 },
	{ ZEND_FRAMELESS_FUNCTION_NAME(stash_put, 3), 3 },
	{ 0 },
};
#endif

#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(stash_get, 1);
static const zend_frameless_function_info frameless_function_infos_stash_get[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(stash_get, 1), 1 },
	{ 0 },
};
#endif

ZEND_FUNCTION(stash_put);
ZEND_FUNCTION(stash_get);
ZEND_FUNCTION(stash_delete);
ZEND_FUNCTION(stash_remember);
ZEND_FUNCTION(stash_stats);

static const zend_function_entry ext_functions[] = {
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("stash_put", zif_stash_put, arginfo_stash_put, 0, frameless_function_infos_stash_put, NULL)
#else
	ZEND_FE(stash_put, arginfo_stash_put)
#endif
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("stash_get", zif_stash_get, arginfo_stash_get, 0, frameless_function_infos_stash_get, NULL)
#else
	ZEND_FE(stash_get, arginfo_stash_get)
#endif
	ZEND_FE(stash_delete, arginfo_stash_delete)
	ZEND_FE(stash_remember, arginfo_stash_remember)
	ZEND_FE(stash_stats, arginfo_stash_stats)
	ZEND_FE_END
};
