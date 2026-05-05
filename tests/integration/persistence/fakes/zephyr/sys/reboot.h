#ifndef SHIM_ZEPHYR_SYS_REBOOT_H
#define SHIM_ZEPHYR_SYS_REBOOT_H

#define SYS_REBOOT_COLD 0

static inline void sys_reboot(int type) { (void)type; /* tests don't actually reboot */ }

#endif
