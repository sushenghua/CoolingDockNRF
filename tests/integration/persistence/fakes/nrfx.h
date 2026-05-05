#ifndef SHIM_NRFX_H
#define SHIM_NRFX_H

#include <stdint.h>

/* cmd_interpreter.c reads the FICR DEVICEID to format the BLE UID
 * string. Provide a fixed fake value so the test output is stable. */
typedef struct {
	uint32_t DEVICEID[2];
} _shim_ficr_t;

extern _shim_ficr_t _shim_ficr;
#define NRF_FICR (&_shim_ficr)

#endif
