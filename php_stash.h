/* stash extension for PHP */

#ifndef PHP_STASH_H
# define PHP_STASH_H

extern zend_module_entry stash_module_entry;
# define phpext_stash_ptr &stash_module_entry

# define PHP_STASH_VERSION "0.1.0"

# if defined(ZTS) && defined(COMPILE_DL_STASH)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

#endif	/* PHP_STASH_H */
