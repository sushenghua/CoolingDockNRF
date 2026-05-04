#ifndef APP_CMD_INTERPRETER_H_
#define APP_CMD_INTERPRETER_H_

#include <stddef.h>
#include <sys/types.h>      /* ssize_t */

/* Dispatch a JSON command. Writes the response JSON into `out`.
 *
 * Returns the response length on success, 0 if the command produces no
 * response, or -errno on failure.
 *
 * Transport-agnostic: any caller (BLE, future serial/shell) can invoke
 * this with a complete JSON object and forward the response.
 */
ssize_t cmd_interpreter_dispatch(const char *json_in, size_t in_len,
			    char *json_out, size_t out_cap);

/* One-time setup: cache device UID, board name, etc. for GetDeviceInfo. */
void cmd_interpreter_init(void);

#endif /* APP_CMD_INTERPRETER_H_ */
