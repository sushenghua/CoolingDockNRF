#ifndef APP_BLE_SVC_H_
#define APP_BLE_SVC_H_

/* Bring up the Bluetooth host, register the GATT service, start
 * advertising, and spawn the status-notify and command-dispatch threads.
 *
 * Must be called *after* settings_subsys_init() — CONFIG_BT_SETTINGS
 * persists bonds via the same settings backend, and bt_enable() loads
 * them. */
int  ble_svc_start(void);

#endif /* APP_BLE_SVC_H_ */
