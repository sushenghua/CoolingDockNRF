#ifndef APP_JSON_IO_H_
#define APP_JSON_IO_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>      /* ssize_t */

#include "sys_data.h"

/* ---------------------------------------------------------- formatters
 * All return number of bytes written (excluding terminating NUL),
 * or -ENOMEM if the buffer was too small.
 */

ssize_t jsf_status(char *out, size_t cap,
		   bool master_on, bool sample_valid,
		   int16_t temp_centi_c, uint16_t humid_centi_pct,
		   const struct prf_cfg *cfg,
		   uint8_t actual_pwm, const char *status);

ssize_t jsf_devinfo(char *out, size_t cap,
		    const char *name, const char *fw,
		    const char *uid, const char *board);

ssize_t jsf_prpconf(char *out, size_t cap, const struct prf_cfg *cfg);

ssize_t jsf_updateret(char *out, size_t cap, int code, const char *val);

/* ----------------------------------------------------------- parsers
 *
 * Tiny key-lookup parser. Searches for "key" within json[] and extracts
 * the first scalar value that follows ':'. Does NOT understand nested
 * objects, escaped quotes inside string values, or whitespace inside
 * keys — all of which are absent from the frontend's command schema.
 *
 * Return 0 on success, -ENOENT if key absent, -EINVAL if value malformed.
 */

int jsp_get_str(const char *json,  const char *key, char *out, size_t cap);
int jsp_get_int(const char *json,  const char *key, int32_t *out);
int jsp_get_bool(const char *json, const char *key, bool *out);

#endif /* APP_JSON_IO_H_ */
