/* Unit tests for src/json_io.c — covers all 7 functions plus targeted
 * regressions for the three parser/formatter bugs caught in review. */

#include "../../test_harness.h"

#include <errno.h>
#include <string.h>

#include "json_io.h"

/* ---------- helpers ---------- */

static struct prf_cfg make_cfg(uint8_t mode)
{
	return (struct prf_cfg){
		.mode = mode, .pwr = false,
		.thr1_c = 25, .thr2_c = 32, .pwm_pct = 50,
		.con_sec = 10, .coff_sec = 5,
		.ont_sec = 0, .offt_sec = 0,
	};
}

/* ============================================================
 *                       jsf_status
 * ============================================================ */

TEST(jsf_status_valid_sample_running)
{
	struct prf_cfg cfg = make_cfg(PRF_MODE_SENSOR);
	char buf[256];
	ssize_t n = jsf_status(buf, sizeof(buf), true, true,
			       2543, 6512, &cfg, 75, "running");
	ASSERT_TRUE(n > 0, "jsf_status returned %zd", n);
	ASSERT_NOT_NULL(strstr(buf, "\"cmd\":\"GetStatusData\""));
	ASSERT_NOT_NULL(strstr(buf, "\"ms\":true"));
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":25.43"));
	ASSERT_NOT_NULL(strstr(buf, "\"humid\":65.12"));
	ASSERT_NOT_NULL(strstr(buf, "\"status\":\"running\""));
	ASSERT_NOT_NULL(strstr(buf, "\"pwm\":75"));
	ASSERT_NOT_NULL(strstr(buf, "\"index\":0"));
	ASSERT_NOT_NULL(strstr(buf, "\"type\":\"fan\""));
}

TEST(jsf_status_invalid_sample_emits_null)
{
	struct prf_cfg cfg = make_cfg(PRF_MODE_POWER);
	char buf[256];
	ssize_t n = jsf_status(buf, sizeof(buf), false, false,
			       0, 0, &cfg, 0, "stopped");
	ASSERT_TRUE(n > 0);
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":null"));
	ASSERT_NOT_NULL(strstr(buf, "\"humid\":null"));
	ASSERT_NOT_NULL(strstr(buf, "\"ms\":false"));
}

/* Regression for fix #2: negative temps in (-1.00, 0.00) used to lose
 * the sign because integer division truncates toward zero. */
TEST(jsf_status_negative_near_zero_keeps_sign)
{
	struct prf_cfg cfg = make_cfg(PRF_MODE_SENSOR);
	char buf[256];

	(void)jsf_status(buf, sizeof(buf), true, true, -50, 5000, &cfg, 0, "stopped");
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":-0.50"), "got: %s", buf);

	(void)jsf_status(buf, sizeof(buf), true, true, -99, 5000, &cfg, 0, "stopped");
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":-0.99"), "got: %s", buf);

	(void)jsf_status(buf, sizeof(buf), true, true, -150, 5000, &cfg, 0, "stopped");
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":-1.50"), "got: %s", buf);

	(void)jsf_status(buf, sizeof(buf), true, true, 25, 5000, &cfg, 0, "stopped");
	ASSERT_NOT_NULL(strstr(buf, "\"temp\":0.25"));
	ASSERT_IS_NULL(strstr(buf, "\"temp\":-"));
}

TEST(jsf_status_buffer_too_small_returns_enomem)
{
	struct prf_cfg cfg = make_cfg(PRF_MODE_SENSOR);
	char tiny[16];
	ssize_t n = jsf_status(tiny, sizeof(tiny), true, true,
			       2543, 6512, &cfg, 75, "running");
	ASSERT_EQ(n, -ENOMEM);
}

/* ============================================================
 *                       jsf_devinfo
 * ============================================================ */

TEST(jsf_devinfo_all_fields_present)
{
	char buf[256];
	ssize_t n = jsf_devinfo(buf, sizeof(buf),
				"CoolingDockNRF", "0.1.0",
				"deadbeefcafebabe", "nrf52dk_nrf52832");
	ASSERT_TRUE(n > 0);
	ASSERT_NOT_NULL(strstr(buf, "\"cmd\":\"GetDeviceInfo\""));
	ASSERT_NOT_NULL(strstr(buf, "\"name\":\"CoolingDockNRF\""));
	ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"0.1.0\""));
	ASSERT_NOT_NULL(strstr(buf, "\"uid\":\"deadbeefcafebabe\""));
	ASSERT_NOT_NULL(strstr(buf, "\"board\":\"nrf52dk_nrf52832\""));
}

TEST(jsf_devinfo_null_strings_become_empty)
{
	char buf[128];
	(void)jsf_devinfo(buf, sizeof(buf), NULL, NULL, NULL, NULL);
	ASSERT_NOT_NULL(strstr(buf, "\"name\":\"\""));
	ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"\""));
}

/* ============================================================
 *                       jsf_prpconf
 * ============================================================ */

TEST(jsf_prpconf_all_modes_round_trip_to_string)
{
	const struct {
		uint8_t mode; const char *str;
	} cases[] = {
		{ PRF_MODE_POWER,    "power"    },
		{ PRF_MODE_FIXED,    "fixed"    },
		{ PRF_MODE_SENSOR,   "sensor"   },
		{ PRF_MODE_CYCLE,    "cycle"    },
		{ PRF_MODE_SCHEDULE, "schedule" },
	};
	char buf[256];
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		struct prf_cfg cfg = make_cfg(cases[i].mode);
		ssize_t n = jsf_prpconf(buf, sizeof(buf), &cfg);
		ASSERT_TRUE(n > 0);
		char needle[32];
		snprintf(needle, sizeof(needle), "\"mode\":\"%s\"", cases[i].str);
		ASSERT_NOT_NULL(strstr(buf, needle), "missing %s in %s", needle, buf);
	}
}

TEST(jsf_prpconf_all_fields_emitted)
{
	struct prf_cfg cfg = make_cfg(PRF_MODE_SENSOR);
	cfg.pwr = true;
	cfg.thr1_c = 22; cfg.thr2_c = 33;
	cfg.pwm_pct = 60;
	cfg.con_sec = 7; cfg.coff_sec = 8;
	cfg.ont_sec = 100; cfg.offt_sec = 200;

	char buf[256];
	(void)jsf_prpconf(buf, sizeof(buf), &cfg);

	ASSERT_NOT_NULL(strstr(buf, "\"pwr\":true"));
	ASSERT_NOT_NULL(strstr(buf, "\"thr1\":22"));
	ASSERT_NOT_NULL(strstr(buf, "\"thr2\":33"));
	ASSERT_NOT_NULL(strstr(buf, "\"pwm\":60"));
	ASSERT_NOT_NULL(strstr(buf, "\"con\":7"));
	ASSERT_NOT_NULL(strstr(buf, "\"coff\":8"));
	ASSERT_NOT_NULL(strstr(buf, "\"ont\":100"));
	ASSERT_NOT_NULL(strstr(buf, "\"offt\":200"));
}

/* ============================================================
 *                       jsf_updateret
 * ============================================================ */

TEST(jsf_updateret_plain_code)
{
	char buf[64];
	ssize_t n = jsf_updateret(buf, sizeof(buf), 0, NULL);
	ASSERT_TRUE(n > 0);
	ASSERT_STR_EQ(buf, "{\"cmd\":\"UpdateRet\",\"code\":0}");
}

TEST(jsf_updateret_code_with_val)
{
	char buf[128];
	(void)jsf_updateret(buf, sizeof(buf), 3, "bad mode");
	ASSERT_NOT_NULL(strstr(buf, "\"code\":3"));
	ASSERT_NOT_NULL(strstr(buf, "\"val\":\"bad mode\""));
}

/* ============================================================
 *                       jsp_get_str
 * ============================================================ */

TEST(jsp_get_str_basic_extraction)
{
	char out[32];
	int rc = jsp_get_str("{\"cmd\":\"GetStatusData\"}", "cmd", out, sizeof(out));
	ASSERT_EQ(rc, 0);
	ASSERT_STR_EQ(out, "GetStatusData");
}

TEST(jsp_get_str_missing_key_returns_enoent)
{
	char out[32];
	int rc = jsp_get_str("{\"cmd\":\"x\"}", "name", out, sizeof(out));
	ASSERT_EQ(rc, -ENOENT);
}

/* Regression for fix #3 — searching for a key whose name happens to
 * also appear as a string VALUE elsewhere must still return the key's
 * value, not the value-position match. */
TEST(jsp_get_str_key_in_value_position_is_skipped)
{
	char out[32];
	int rc = jsp_get_str("{\"cmd\":\"name\",\"name\":\"foo\"}",
			     "name", out, sizeof(out));
	ASSERT_EQ(rc, 0, "got rc=%d", rc);
	ASSERT_STR_EQ(out, "foo");
}

/* Regression for fix #18 — escaped quotes inside string values used to
 * truncate the parse. */
TEST(jsp_get_str_escaped_quote_passthrough)
{
	char out[32];
	int rc = jsp_get_str("{\"name\":\"foo\\\"bar\"}",
			     "name", out, sizeof(out));
	ASSERT_EQ(rc, 0);
	ASSERT_STR_EQ(out, "foo\"bar");
}

/* When the output buffer is too small to fit the entire value, the
 * loop exits before reaching the closing quote — jsp_get_str then sees
 * *v != '"' and returns -EINVAL. This documents the actual behavior;
 * callers that want truncation must size the buffer explicitly. */
TEST(jsp_get_str_buffer_too_small_returns_einval)
{
	char out[4];
	int rc = jsp_get_str("{\"k\":\"abcdefgh\"}", "k", out, sizeof(out));
	ASSERT_EQ(rc, -EINVAL);
}

/* The exact-fit case: the buffer holds the full value plus the NUL.
 * Loop sees *v == '"' on the iteration after the last char, breaks
 * cleanly, returns 0 with a properly-terminated string. */
TEST(jsp_get_str_exact_fit_succeeds)
{
	char out[4];   /* fits "abc" + NUL */
	int rc = jsp_get_str("{\"k\":\"abc\"}", "k", out, sizeof(out));
	ASSERT_EQ(rc, 0);
	ASSERT_STR_EQ(out, "abc");
}

TEST(jsp_get_str_malformed_unterminated_string)
{
	char out[32];
	int rc = jsp_get_str("{\"k\":\"abc", "k", out, sizeof(out));
	ASSERT_EQ(rc, -EINVAL);
}

/* ============================================================
 *                       jsp_get_int
 * ============================================================ */

TEST(jsp_get_int_basic_positive)
{
	int32_t v;
	int rc = jsp_get_int("{\"index\":42}", "index", &v);
	ASSERT_EQ(rc, 0);
	ASSERT_EQ(v, 42);
}

TEST(jsp_get_int_negative)
{
	int32_t v;
	int rc = jsp_get_int("{\"thr1\":-15}", "thr1", &v);
	ASSERT_EQ(rc, 0);
	ASSERT_EQ(v, -15);
}

TEST(jsp_get_int_missing)
{
	int32_t v = 99;
	int rc = jsp_get_int("{\"x\":1}", "y", &v);
	ASSERT_EQ(rc, -ENOENT);
	ASSERT_EQ(v, 99);
}

TEST(jsp_get_int_malformed)
{
	int32_t v;
	int rc = jsp_get_int("{\"x\":\"notanumber\"}", "x", &v);
	ASSERT_EQ(rc, -EINVAL);
}

/* ============================================================
 *                       jsp_get_bool
 * ============================================================ */

TEST(jsp_get_bool_true_and_false)
{
	bool v;
	ASSERT_EQ(jsp_get_bool("{\"on\":true}",  "on", &v), 0);
	ASSERT_TRUE(v);
	ASSERT_EQ(jsp_get_bool("{\"on\":false}", "on", &v), 0);
	ASSERT_FALSE(v);
}

TEST(jsp_get_bool_missing)
{
	bool v;
	ASSERT_EQ(jsp_get_bool("{\"x\":1}", "y", &v), -ENOENT);
}

TEST(jsp_get_bool_malformed)
{
	bool v;
	ASSERT_EQ(jsp_get_bool("{\"on\":42}", "on", &v), -EINVAL);
}
