#ifndef AUTOCONF_H
#define AUTOCONF_H

/* Board: TI LM3S6965-QB (Cortex-M3, 256KB Flash, 64KB RAM) */
#define CONFIG_BOARD_LM3S6965_QB 1
#define CONFIG_BOARD "lm3s6965-qb"

/* Scheduler */
#define CONFIG_SCHEDULER 1
#define CONFIG_MAX_TASKS 4
#define CONFIG_TICK_RATE_HZ 1000
#define CONFIG_NO_DYNAMIC_ALLOCATION 1

/* File Systems */
#define CONFIG_VFS 1
#define CONFIG_FS_RAMFS 1
#define CONFIG_FS_PROCFS 1

/* Drivers */
#define CONFIG_DEVICE_UART 1
#define CONFIG_TIMER_MANAGER 1
#define CONFIG_WORK_QUEUE 1

/* Networking */
#define CONFIG_NETWORKING 1
#define CONFIG_NET_LWIP 1
#define CONFIG_NET_DEFAULT_IP "10.0.2.15"
#define CONFIG_NET_DEFAULT_MAC "00:11:22:33:44:55"

/* Security */
#define CONFIG_OTA_DEV_MODE 1
#define CONFIG_SECURE_BOOT_DEV_MODE 1
#define CONFIG_WATCHDOG 1
#define CONFIG_WATCHDOG_TIMEOUT_MS 5000

/* Features disabled for size optimization */
/* CONFIG_LUA_VM not defined — Lua VM too large for 256KB Flash */
/* CONFIG_ELF_LOADER not defined — ELF loader too large for 256KB Flash */

#endif /* AUTOCONF_H */
