#!/usr/bin/env python3
"""Generate autoconf.h for Cortex-M0+ (Nucleo-L031K6) build.

M0+ has only 8KB RAM, so many features are disabled:
- No networking (lwIP needs too much RAM)
- No Lua VM (needs significant memory)
- No LittleFS (flash too small)
- No OTA (needs networking)
- No watchdog (resource constrained)
- No ELF loader (not needed for minimal build)

Enabled features:
- Scheduler with 4 tasks
- VFS with RamFS and ProcFS
- UART driver
- Metrics (software cycle counter)
"""

import os

CONFIG_CONTENT = """\
#ifndef AUTOCONF_H
#define AUTOCONF_H

/* Board: ST Nucleo-L031K6 (Cortex-M0+, 64KB Flash, 8KB RAM) */
#define CONFIG_BOARD_NUCLEO_L031K6 1
#define CONFIG_BOARD "nucleo_l031k6"

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

/* Features disabled for M0+ (8KB RAM limit) */
/* CONFIG_NETWORKING not defined — no lwIP */
/* CONFIG_NET_LWIP not defined */
/* CONFIG_LUA_VM not defined */
/* CONFIG_OTA_DEV_MODE not defined */
/* CONFIG_SECURE_BOOT_DEV_MODE not defined */
/* CONFIG_WATCHDOG not defined */
/* CONFIG_ELF_LOADER not defined */

#endif /* AUTOCONF_H */
"""


def main():
    config_dir = os.path.join(os.path.dirname(__file__), '..', 'config')
    config_path = os.path.join(config_dir, 'autoconf.h')

    os.makedirs(config_dir, exist_ok=True)

    with open(config_path, 'w') as f:
        f.write(CONFIG_CONTENT)

    print(f"Generated {config_path}")


if __name__ == '__main__':
    main()
