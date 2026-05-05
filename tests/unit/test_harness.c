#include "test_harness.h"

test_entry_t *_test_list      = NULL;
int           _test_failures  = 0;
jmp_buf       _test_jmp;
const char   *_test_current   = "";

void _test_fail(const char *file, int line, const char *expr,
		const char *fmt, ...)
{
	fprintf(stderr, "  FAIL  [%s] %s:%d: %s",
		_test_current, file, line, expr);
	if (fmt && fmt[0]) {
		fputs(" — ", stderr);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	fputc('\n', stderr);
	_test_failures++;
	longjmp(_test_jmp, 1);
}

int main(void)
{
	/* Constructor registration walks the list in reverse declaration
	 * order. Reverse it once so output mirrors source order. */
	test_entry_t *prev = NULL, *cur = _test_list, *next;
	while (cur) {
		next = cur->next;
		cur->next = prev;
		prev = cur;
		cur = next;
	}
	_test_list = prev;

	int total = 0;
	for (test_entry_t *t = _test_list; t; t = t->next) total++;

	printf("Running %d tests...\n\n", total);

	int passed = 0;
	for (test_entry_t *t = _test_list; t; t = t->next) {
		_test_current = t->name;
		int before = _test_failures;
		printf("  %-58s ", t->name);
		fflush(stdout);
		if (setjmp(_test_jmp) == 0) {
			t->fn();
		}
		if (_test_failures == before) {
			printf("PASS\n");
			passed++;
		} else {
			printf("FAIL\n");
		}
	}

	printf("\n%d/%d passed", passed, total);
	if (_test_failures) {
		printf(", %d failure%s",
		       _test_failures, _test_failures == 1 ? "" : "s");
	}
	printf("\n");

	return _test_failures == 0 ? 0 : 1;
}
