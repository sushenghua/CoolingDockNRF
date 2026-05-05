/* Implementations for the shimmed Zephyr APIs and the FICR. */

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "zephyr/settings/settings.h"
#include "nrfx.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ----------------------------- FICR fake ----------------------------- */

_shim_ficr_t _shim_ficr = { .DEVICEID = { 0xCAFEBABE, 0xDEADBEEF } };

/* ------------------------- settings handlers ----------------------- */

#define MAX_HANDLERS 4
static struct settings_handler_static *handlers[MAX_HANDLERS];
static int handler_count;

void _shim_register_settings_handler(struct settings_handler_static *h)
{
	if (handler_count < MAX_HANDLERS) {
		handlers[handler_count++] = h;
	}
}

/* --------------------- in-memory settings store --------------------- */

#define MAX_ENTRIES   32
#define MAX_KEY_LEN   64
#define MAX_VAL_LEN   128

struct entry {
	int      used;
	char     key[MAX_KEY_LEN];
	uint8_t  val[MAX_VAL_LEN];
	size_t   val_len;
};
static struct entry store[MAX_ENTRIES];

void _shim_settings_reset(void)
{
	memset(store, 0, sizeof(store));
}

static struct entry *find(const char *key)
{
	for (int i = 0; i < MAX_ENTRIES; i++) {
		if (store[i].used && strcmp(store[i].key, key) == 0) {
			return &store[i];
		}
	}
	return NULL;
}

static struct entry *find_or_alloc(const char *key)
{
	struct entry *e = find(key);
	if (e) return e;
	for (int i = 0; i < MAX_ENTRIES; i++) {
		if (!store[i].used) {
			store[i].used = 1;
			strncpy(store[i].key, key, sizeof(store[i].key) - 1);
			store[i].key[sizeof(store[i].key) - 1] = '\0';
			return &store[i];
		}
	}
	return NULL;
}

/* settings_read_cb implementation used during settings_load_*: copies
 * up to `len` bytes from the entry into the caller's buffer. */
static ssize_t fake_read_cb(void *cb_arg, void *data, size_t len)
{
	struct entry *e = cb_arg;
	size_t take = MIN(len, e->val_len);
	memcpy(data, e->val, take);
	return (ssize_t)take;
}
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ----------------------------- API surface --------------------------- */

int settings_subsys_init(void) { return 0; }

int settings_load(void) { return settings_load_subtree(NULL); }

int settings_load_subtree(const char *subtree)
{
	for (int h = 0; h < handler_count; h++) {
		struct settings_handler_static *handler = handlers[h];
		size_t prefix_len = strlen(handler->name);

		for (int i = 0; i < MAX_ENTRIES; i++) {
			if (!store[i].used) continue;
			/* Match keys under "<handler->name>/..." */
			if (strncmp(store[i].key, handler->name, prefix_len) != 0) continue;
			if (store[i].key[prefix_len] != '/') continue;
			(void)subtree;   /* honor handler routing only */

			const char *sub = store[i].key + prefix_len + 1;
			(void)handler->h_set(sub, store[i].val_len,
					     fake_read_cb, &store[i]);
		}
	}
	return 0;
}

int settings_save_one(const char *key, const void *value, size_t val_len)
{
	if (!key || val_len > MAX_VAL_LEN) return -EINVAL;
	struct entry *e = find_or_alloc(key);
	if (!e) return -ENOMEM;
	memcpy(e->val, value, val_len);
	e->val_len = val_len;
	return 0;
}

int settings_delete(const char *key)
{
	struct entry *e = find(key);
	if (!e) return 0;
	memset(e, 0, sizeof(*e));
	return 0;
}

int settings_name_next(const char *name, const char **next)
{
	if (!name) return 0;
	const char *slash = strchr(name, '/');
	int len;
	if (slash) {
		len = (int)(slash - name);
		if (next) *next = slash + 1;
	} else {
		len = (int)strlen(name);
		if (next) *next = NULL;
	}
	return len;
}
