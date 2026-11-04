/* This is a generated file, edit stash_apcu.stub.php instead.
 * Stub hash: 59db899583b8ffeced7ac73c8172427e86a2cc52 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_apcu_enabled, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_apcu_store, 0, 1, MAY_BE_ARRAY|MAY_BE_BOOL)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

#define arginfo_apcu_add arginfo_apcu_store

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_apcu_fetch, 0, 1, IS_MIXED, 0)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(1, success, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_apcu_exists, 0, 1, MAY_BE_ARRAY|MAY_BE_BOOL)
	ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()

#define arginfo_apcu_delete arginfo_apcu_exists

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_apcu_inc, 0, 1, MAY_BE_LONG|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, step, IS_LONG, 0, "1")
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(1, success, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

#define arginfo_apcu_dec arginfo_apcu_inc

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_apcu_cas, 0, 3, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, old, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, new, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_apcu_clear_cache arginfo_apcu_enabled

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_apcu_cache_info, 0, 0, MAY_BE_ARRAY|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limited, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_apcu_key_info, 0, 1, IS_ARRAY, 1)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_apcu_sma_info arginfo_apcu_cache_info

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_apcu_entry, 0, 2, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_APCUIterator___construct, 0, 0, 0)
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, search, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, format, IS_LONG, 0, "APC_ITER_ALL")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunk_size, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, list, IS_LONG, 0, "APC_LIST_ACTIVE")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_APCUIterator_rewind, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_APCUIterator_next arginfo_class_APCUIterator_rewind

#define arginfo_class_APCUIterator_valid arginfo_apcu_enabled

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_class_APCUIterator_key, 0, 0, MAY_BE_STRING|MAY_BE_LONG)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_APCUIterator_current, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_APCUIterator_getTotalHits, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_APCUIterator_getTotalSize arginfo_class_APCUIterator_getTotalHits

#define arginfo_class_APCUIterator_getTotalCount arginfo_class_APCUIterator_getTotalHits


#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(apcu_store, 1);
ZEND_FRAMELESS_FUNCTION(apcu_store, 2);
ZEND_FRAMELESS_FUNCTION(apcu_store, 3);
static const zend_frameless_function_info frameless_function_infos_apcu_store[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_store, 1), 1 },
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_store, 2), 2 },
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_store, 3), 3 },
	{ 0 },
};
#endif

#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(apcu_fetch, 1);
static const zend_frameless_function_info frameless_function_infos_apcu_fetch[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_fetch, 1), 1 },
	{ 0 },
};
#endif

#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(apcu_exists, 1);
static const zend_frameless_function_info frameless_function_infos_apcu_exists[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_exists, 1), 1 },
	{ 0 },
};
#endif

#if (PHP_VERSION_ID >= 80400)
ZEND_FRAMELESS_FUNCTION(apcu_delete, 1);
static const zend_frameless_function_info frameless_function_infos_apcu_delete[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(apcu_delete, 1), 1 },
	{ 0 },
};
#endif

ZEND_FUNCTION(apcu_enabled);
ZEND_FUNCTION(apcu_store);
ZEND_FUNCTION(apcu_add);
ZEND_FUNCTION(apcu_fetch);
ZEND_FUNCTION(apcu_exists);
ZEND_FUNCTION(apcu_delete);
ZEND_FUNCTION(apcu_inc);
ZEND_FUNCTION(apcu_dec);
ZEND_FUNCTION(apcu_cas);
ZEND_FUNCTION(apcu_clear_cache);
ZEND_FUNCTION(apcu_cache_info);
ZEND_FUNCTION(apcu_key_info);
ZEND_FUNCTION(apcu_sma_info);
ZEND_FUNCTION(apcu_entry);
ZEND_METHOD(APCUIterator, __construct);
ZEND_METHOD(APCUIterator, rewind);
ZEND_METHOD(APCUIterator, next);
ZEND_METHOD(APCUIterator, valid);
ZEND_METHOD(APCUIterator, key);
ZEND_METHOD(APCUIterator, current);
ZEND_METHOD(APCUIterator, getTotalHits);
ZEND_METHOD(APCUIterator, getTotalSize);
ZEND_METHOD(APCUIterator, getTotalCount);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(apcu_enabled, arginfo_apcu_enabled)
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("apcu_store", zif_apcu_store, arginfo_apcu_store, 0, frameless_function_infos_apcu_store, NULL)
#else
	ZEND_FE(apcu_store, arginfo_apcu_store)
#endif
	ZEND_FE(apcu_add, arginfo_apcu_add)
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("apcu_fetch", zif_apcu_fetch, arginfo_apcu_fetch, 0, frameless_function_infos_apcu_fetch, NULL)
#else
	ZEND_FE(apcu_fetch, arginfo_apcu_fetch)
#endif
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("apcu_exists", zif_apcu_exists, arginfo_apcu_exists, 0, frameless_function_infos_apcu_exists, NULL)
#else
	ZEND_FE(apcu_exists, arginfo_apcu_exists)
#endif
#if (PHP_VERSION_ID >= 80400)
	ZEND_RAW_FENTRY("apcu_delete", zif_apcu_delete, arginfo_apcu_delete, 0, frameless_function_infos_apcu_delete, NULL)
#else
	ZEND_FE(apcu_delete, arginfo_apcu_delete)
#endif
	ZEND_FE(apcu_inc, arginfo_apcu_inc)
	ZEND_FE(apcu_dec, arginfo_apcu_dec)
	ZEND_FE(apcu_cas, arginfo_apcu_cas)
	ZEND_FE(apcu_clear_cache, arginfo_apcu_clear_cache)
	ZEND_FE(apcu_cache_info, arginfo_apcu_cache_info)
	ZEND_FE(apcu_key_info, arginfo_apcu_key_info)
	ZEND_FE(apcu_sma_info, arginfo_apcu_sma_info)
	ZEND_FE(apcu_entry, arginfo_apcu_entry)
	ZEND_FE_END
};

static const zend_function_entry class_APCUIterator_methods[] = {
	ZEND_ME(APCUIterator, __construct, arginfo_class_APCUIterator___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, rewind, arginfo_class_APCUIterator_rewind, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, next, arginfo_class_APCUIterator_next, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, valid, arginfo_class_APCUIterator_valid, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, key, arginfo_class_APCUIterator_key, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, current, arginfo_class_APCUIterator_current, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, getTotalHits, arginfo_class_APCUIterator_getTotalHits, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, getTotalSize, arginfo_class_APCUIterator_getTotalSize, ZEND_ACC_PUBLIC)
	ZEND_ME(APCUIterator, getTotalCount, arginfo_class_APCUIterator_getTotalCount, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_APCUIterator(zend_class_entry *class_entry_Iterator)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "APCUIterator", class_APCUIterator_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, 0);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
#endif
	zend_class_implements(class_entry, 1, class_entry_Iterator);

	return class_entry;
}
