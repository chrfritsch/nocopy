/* nocopy extension for PHP */

#ifndef PHP_NOCOPY_H
# define PHP_NOCOPY_H

extern zend_module_entry nocopy_module_entry;
# define phpext_nocopy_ptr &nocopy_module_entry

# define PHP_NOCOPY_VERSION "0.1.0"

# if defined(ZTS) && defined(COMPILE_DL_NOCOPY)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

#endif	/* PHP_NOCOPY_H */
