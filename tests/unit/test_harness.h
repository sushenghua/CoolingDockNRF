/* Minimal native test harness for pure-logic unit tests on macOS.
 *
 * Why this exists: NCS v3.3.0's Zephyr can't compile its `ztest`
 * framework on macOS — the iterable-sections feature it relies on
 * uses ELF section attributes that Mach-O rejects. So instead of
 * fighting that, we build the tests as plain native binaries with
 * `cc` (sanitizers + coverage included) using this header-only
 * harness. Same assertions, same auto-registration, no Zephyr.
 *
 * Usage:
 *
 *   #include "test_harness.h"
 *   #include "module_under_test.h"
 *
 *   TEST(my_thing_works)
 *   {
 *       ASSERT_EQ(2 + 2, 4);
 *       ASSERT_STR_EQ(foo(), "bar");
 *   }
 *
 *   // No main() needed — link against test_harness.c, which ships one.
 */

#ifndef APP_TEST_HARNESS_H_
#define APP_TEST_HARNESS_H_

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*test_fn_t)(void);

typedef struct test_entry {
	const char         *name;
	test_fn_t           fn;
	struct test_entry  *next;
} test_entry_t;

extern test_entry_t *_test_list;
extern int           _test_failures;
extern jmp_buf       _test_jmp;
extern const char   *_test_current;

void _test_fail(const char *file, int line, const char *expr,
		const char *fmt, ...);

/* Failure helper. The trailing `, "" __VA_ARGS__` trick makes the
 * optional printf-style context safe whether or not the caller passed
 * it: empty VA_ARGS becomes `, ""`; non-empty becomes
 * `, "" "fmt", arg` which the preprocessor concatenates into one
 * format string.  This works in C99+ on every relevant compiler. */
#define _ASSERT(cond, expr, ...) \
	do { \
		if (!(cond)) { \
			_test_fail(__FILE__, __LINE__, expr, "" __VA_ARGS__); \
		} \
	} while (0)

#define ASSERT_TRUE(x, ...)        _ASSERT((x), "expected true: " #x, ## __VA_ARGS__)
#define ASSERT_FALSE(x, ...)       _ASSERT(!(x), "expected false: " #x, ## __VA_ARGS__)
#define ASSERT_EQ(a, b, ...)       _ASSERT((a) == (b), #a " == " #b, ## __VA_ARGS__)
#define ASSERT_NE(a, b, ...)       _ASSERT((a) != (b), #a " != " #b, ## __VA_ARGS__)
#define ASSERT_NOT_NULL(x, ...)    _ASSERT((x) != NULL, "expected non-NULL: " #x, ## __VA_ARGS__)
#define ASSERT_IS_NULL(x, ...)     _ASSERT((x) == NULL, "expected NULL: " #x, ## __VA_ARGS__)
#define ASSERT_STR_EQ(a, b, ...)   _ASSERT(strcmp((a), (b)) == 0, \
			"string equality " #a " == " #b, ## __VA_ARGS__)

#define ASSERT_WITHIN(actual, expected, delta, ...) \
	do { \
		long long _a = (long long)(actual); \
		long long _e = (long long)(expected); \
		long long _d = (long long)(delta); \
		long long _diff = _a > _e ? _a - _e : _e - _a; \
		_ASSERT(_diff <= _d, \
			#actual " within " #delta " of " #expected, \
			## __VA_ARGS__); \
	} while (0)

/* Define + auto-register a test. The parameter is named `_t` rather
 * than `name` because the preprocessor substitutes everywhere — using
 * `name` would clobber the `.name = ...` designated-initializer key. */
#define TEST(_t) \
	static void test_##_t(void); \
	__attribute__((constructor)) \
	static void _register_##_t(void) \
	{ \
		static test_entry_t _entry = { .name = #_t, .fn = test_##_t }; \
		_entry.next = _test_list; \
		_test_list  = &_entry; \
	} \
	static void test_##_t(void)

#endif /* APP_TEST_HARNESS_H_ */
