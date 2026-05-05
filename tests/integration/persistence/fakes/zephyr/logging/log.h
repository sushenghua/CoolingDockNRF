#ifndef SHIM_ZEPHYR_LOGGING_LOG_H
#define SHIM_ZEPHYR_LOGGING_LOG_H

#include <stdio.h>

#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_INF  3

/* The real Zephyr macro emits a registration struct in a linker
 * section. We just need the symbol name to disappear from the warnings
 * and stay out of the way. */
#define LOG_MODULE_REGISTER(_name, ...) \
	__attribute__((unused)) \
	static const char *_log_module_##_name = #_name

/* Quiet by default; set TEST_LOG_VERBOSE=1 in env to surface them. */
#ifdef TEST_LOG_VERBOSE
#define LOG_INF(fmt, ...) fprintf(stderr, "  [inf] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) fprintf(stderr, "  [wrn] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) fprintf(stderr, "  [err] " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) fprintf(stderr, "  [dbg] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#endif

#endif
