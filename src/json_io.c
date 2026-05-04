#include "json_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------ helpers */

static const char *mode_str(uint8_t m)
{
	switch (m) {
	case PRF_MODE_POWER:    return "power";
	case PRF_MODE_FIXED:    return "fixed";
	case PRF_MODE_SENSOR:   return "sensor";
	case PRF_MODE_CYCLE:    return "cycle";
	case PRF_MODE_SCHEDULE: return "schedule";
	default:                return "unknown";
	}
}

#define EMIT(...) do {                                          \
		int _n = snprintf(out + pos, cap - pos, __VA_ARGS__); \
		if (_n < 0 || (size_t)_n >= cap - pos) return -ENOMEM; \
		pos += (size_t)_n;                                       \
	} while (0)

/* ------------------------------------------------------------ formatters */

ssize_t jsf_status(char *out, size_t cap,
		   bool master_on, bool sample_valid,
		   int16_t temp_centi_c, uint16_t humid_centi_pct,
		   const struct prf_cfg *cfg,
		   uint8_t actual_pwm, const char *status)
{
	size_t pos = 0;

	EMIT("{\"cmd\":\"GetStatusData\",\"ms\":%s,\"sensors\":{\"s0\":{",
	     master_on ? "true" : "false");

	if (sample_valid) {
		int t_int = temp_centi_c / 100;
		int t_frac = (temp_centi_c < 0 ? -temp_centi_c : temp_centi_c) % 100;
		EMIT("\"temp\":%d.%02d,\"humid\":%u.%02u",
		     t_int, t_frac,
		     (unsigned)(humid_centi_pct / 100),
		     (unsigned)(humid_centi_pct % 100));
	} else {
		EMIT("\"temp\":null,\"humid\":null");
	}

	EMIT("}},\"prf\":[{\"index\":0,\"type\":\"fan\",\"status\":\"%s\",\"pwm\":%u}]}",
	     status ? status : "unknown", (unsigned)actual_pwm);

	(void)cfg;     /* status carries actual not configured */
	return (ssize_t)pos;
}

ssize_t jsf_devinfo(char *out, size_t cap,
		    const char *name, const char *fw,
		    const char *uid, const char *board)
{
	size_t pos = 0;
	EMIT("{\"cmd\":\"GetDeviceInfo\",\"name\":\"%s\",\"fw\":\"%s\","
	     "\"uid\":\"%s\",\"board\":\"%s\"}",
	     name ? name : "", fw ? fw : "", uid ? uid : "", board ? board : "");
	return (ssize_t)pos;
}

ssize_t jsf_prpconf(char *out, size_t cap, const struct prf_cfg *cfg)
{
	size_t pos = 0;
	EMIT("{\"cmd\":\"GetPeripheralSetsConfig\",\"sets\":[{"
	     "\"index\":0,\"type\":\"fan\",\"mode\":\"%s\","
	     "\"pwr\":%s,\"thr1\":%d,\"thr2\":%d,"
	     "\"pwm\":%u,\"con\":%u,\"coff\":%u,"
	     "\"ont\":%u,\"offt\":%u}]}",
	     mode_str(cfg->mode),
	     cfg->pwr ? "true" : "false",
	     (int)cfg->thr1_c, (int)cfg->thr2_c,
	     (unsigned)cfg->pwm_pct,
	     (unsigned)cfg->con_sec, (unsigned)cfg->coff_sec,
	     (unsigned)cfg->ont_sec, (unsigned)cfg->offt_sec);
	return (ssize_t)pos;
}

ssize_t jsf_updateret(char *out, size_t cap, int code, const char *val)
{
	size_t pos = 0;
	if (val) {
		EMIT("{\"cmd\":\"UpdateRet\",\"code\":%d,\"val\":\"%s\"}", code, val);
	} else {
		EMIT("{\"cmd\":\"UpdateRet\",\"code\":%d}", code);
	}
	return (ssize_t)pos;
}

/* ------------------------------------------------------------- parsers
 *
 * Find "key" at top-level (not inside a string literal value), advance
 * past the ':' and any whitespace, and return a pointer to the start
 * of the value. NULL if not found.
 */

static const char *find_value(const char *json, const char *key)
{
	if (!json || !key) return NULL;

	size_t keylen = strlen(key);
	const char *p = json;

	while ((p = strstr(p, key)) != NULL) {
		/* Require quoted key: "key": ... */
		if (p > json && p[-1] == '"' && p[keylen] == '"') {
			const char *q = p + keylen + 1;
			while (*q && (*q == ' ' || *q == '\t' || *q == ':')) q++;
			if (*q) return q;
		}
		p += keylen;
	}
	return NULL;
}

int jsp_get_str(const char *json, const char *key, char *out, size_t cap)
{
	const char *v = find_value(json, key);
	if (!v) return -ENOENT;
	if (*v != '"') return -EINVAL;
	v++;
	size_t i = 0;
	while (*v && *v != '"' && i + 1 < cap) {
		out[i++] = *v++;
	}
	if (*v != '"') return -EINVAL;
	out[i] = '\0';
	return 0;
}

int jsp_get_int(const char *json, const char *key, int32_t *out)
{
	const char *v = find_value(json, key);
	if (!v) return -ENOENT;
	if (*v != '-' && !isdigit((unsigned char)*v)) return -EINVAL;
	char *end;
	long val = strtol(v, &end, 10);
	if (end == v) return -EINVAL;
	*out = (int32_t)val;
	return 0;
}

int jsp_get_bool(const char *json, const char *key, bool *out)
{
	const char *v = find_value(json, key);
	if (!v) return -ENOENT;
	if (!strncmp(v, "true",  4)) { *out = true;  return 0; }
	if (!strncmp(v, "false", 5)) { *out = false; return 0; }
	return -EINVAL;
}
