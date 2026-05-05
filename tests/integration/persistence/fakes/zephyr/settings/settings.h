/* Shim for the slice of Zephyr's settings subsystem that sys_data
 * touches. Backed by an in-memory key→bytes table (see settings_fake.c).
 * Same API surface, no NVS — the test code under exam can't tell the
 * difference. */

#ifndef SHIM_ZEPHYR_SETTINGS_H
#define SHIM_ZEPHYR_SETTINGS_H

#include <stddef.h>
#include <sys/types.h>     /* ssize_t */

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

struct settings_handler_static {
	const char *name;
	int (*h_get)(const char *key, char *val, int val_len_max);
	int (*h_set)(const char *key, size_t len,
		     settings_read_cb read_cb, void *cb_arg);
	int (*h_commit)(void);
	int (*h_export)(int (*cb)(const char *name, const void *value, size_t val_len));
};

void _shim_register_settings_handler(struct settings_handler_static *h);

/* Production code uses linker sections to register; we use a
 * constructor function to register at process startup. */
#define SETTINGS_STATIC_HANDLER_DEFINE(_hname, _tree, _get, _set, _commit, _export) \
	static struct settings_handler_static _shim_handler_##_hname = { \
		.name = _tree, \
		.h_get = _get, \
		.h_set = _set, \
		.h_commit = _commit, \
		.h_export = _export, \
	}; \
	__attribute__((constructor)) \
	static void _shim_register_##_hname(void) \
	{ \
		_shim_register_settings_handler(&_shim_handler_##_hname); \
	}

int  settings_subsys_init(void);
int  settings_load(void);
int  settings_load_subtree(const char *subtree);
int  settings_save_one(const char *key, const void *value, size_t val_len);
int  settings_delete(const char *key);

/* settings_name_next: parse the next path segment from `name` into the
 * length returned, advancing `*next` past the segment if not NULL. */
int  settings_name_next(const char *name, const char **next);

/* Test-only helpers */
void _shim_settings_reset(void);   /* clear the in-memory store */

#endif
