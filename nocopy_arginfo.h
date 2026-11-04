/* This is a generated file, edit nocopy.stub.php instead.
 * Stub hash: 85a391d69956ff149e83b503e270409a3590eba4 */

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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_delete, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_remember, 0, 2, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, beta, IS_DOUBLE, 0, "1.0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nocopy_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()


#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(nocopy_put, 2);
ZEND_FRAMELESS_FUNCTION(nocopy_put, 3);
static const zend_frameless_function_info frameless_function_infos_nocopy_put[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_put, 2), 2 },
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_put, 3), 3 },
	{ 0 },
};
#endif

#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(nocopy_get, 1);
static const zend_frameless_function_info frameless_function_infos_nocopy_get[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(nocopy_get, 1), 1 },
	{ 0 },
};
#endif

ZEND_FUNCTION(nocopy_put);
ZEND_FUNCTION(nocopy_get);
ZEND_FUNCTION(nocopy_delete);
ZEND_FUNCTION(nocopy_remember);
ZEND_FUNCTION(nocopy_stats);

static const zend_function_entry ext_functions[] = {
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("nocopy_put", zif_nocopy_put, arginfo_nocopy_put, 0, frameless_function_infos_nocopy_put, NULL)
#else
	ZEND_FE(nocopy_put, arginfo_nocopy_put)
#endif
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("nocopy_get", zif_nocopy_get, arginfo_nocopy_get, 0, frameless_function_infos_nocopy_get, NULL)
#else
	ZEND_FE(nocopy_get, arginfo_nocopy_get)
#endif
	ZEND_FE(nocopy_delete, arginfo_nocopy_delete)
	ZEND_FE(nocopy_remember, arginfo_nocopy_remember)
	ZEND_FE(nocopy_stats, arginfo_nocopy_stats)
	ZEND_FE_END
};
