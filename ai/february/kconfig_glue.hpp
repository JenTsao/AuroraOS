#ifndef AURORA_FEBRUARY_KCONFIG_GLUE_HPP
#define AURORA_FEBRUARY_KCONFIG_GLUE_HPP

// ============================================================
// ai/february/kconfig_glue.hpp — Kconfig 符号 → C 宏 桥
// ============================================================
//
// 存在的理由
// --------------------------------
// February 的两套命名对不上，中间一直缺一层翻译：
//
//   Kconfig 符号            config.hpp 里的 C 宏
//   ─────────────────────   ────────────────────────────────
//   CONFIG_FEBRUARY_SERVICE      FEBRUARY_ENABLE_SERVICE      ✗ 名字不同
//   CONFIG_FEBRUARY_PLANNER     FEBRUARY_ENABLE_PLANNER      ✗
//   CONFIG_FEBRUARY_SOFTBUS      FEBRUARY_ENABLE_SOFTBUS       ✗
//   CONFIG_FEBRUARY_PEER_TABLE   FEBRUARY_ENABLE_PEER_TABLE    ✗
//   CONFIG_FEBRUARY_PLANNER_MAX_STEPS  FEBRUARY_PLANNER_MAX_STEPS  ✓ 仅差前缀
//   CONFIG_FEBRUARY_SOFTBUS_PKG       FEBRUARY_SOFTBUS_PKG        ✓ 仅差前缀
//
// 根因是 ai/february/Kconfig 此前**从未被顶层 Kconfig source**，所以
// autoconf.h 里一个 FEBRUARY 符号都没有；February 一直跑在 config.hpp 的
// `#ifndef` 兜底默认值上，那套默认值与 Kconfig 是两套独立演进的真相。
//
// 用法：**必须在任何 February 头文件之前** include 本文件。
//   #include "../../ai/february/kconfig_glue.hpp"
//   #include "../../ai/february/service.hpp"
//
// 为什么不改 February 内部：`ai/february/` 是 vendored 运行时，
// AGENTS.md §26 要求「不要直接编辑第三方源码，改用 adapter」。本文件就是
// 那个 adapter —— 只做宏翻译，不碰 February 一行逻辑。
//
// 未定义即关闭：任何未在 Kconfig 里声明的符号都落到 config.hpp 的
// `#ifndef` 默认值，行为与「February 从未接线」时完全一致。
// ============================================================

#include <stdint.h>

// ------------------------------------------------------------
// 0) 引入 Kconfig 生成的真相来源（若存在）
//
// 固件构建靠 board.cmake 的 `-include autoconf.h` 把它塞进每个 TU；
// 主机测试构建**没有**这个 flag（tests/CMakeLists.txt 只 -include
// tests/stubs/host_prelude.hpp）。若本文件依赖「宏已经被预包含」，同一份
// 胶水代码就会在固件里启用、在 host 单测里却整段被 #ifdef 编译掉 ——
// 测试成了摆设。因此这里显式引入。
//
// ⚠️ 已知坑：仓库里**提交的 config/autoconf.h 与 .config 并不同源**。
//   `scripts/genconfig.py` 优先 `load_config(".config")`，而提交的 .config
//   与当初生成 autoconf.h 的那份不一致 —— 重新生成会静默丢掉
//   CONFIG_NETWORKING / CONFIG_LUA_VM / CONFIG_WATCHDOG / CONFIG_STEALTH_*，
//   并把 CONFIG_MAX_TASKS 从 16 重置为 4，连 include guard 都不再输出。
//   直接提交重新生成的 autoconf.h 会让 lm3s6965 / miband8 链接失败。
//   故本 PR **不提交** 重新生成的 autoconf.h（AGENTS.md §25 同样禁止手工
//   编辑生成文件）。下面的降级逻辑就是为了在这种「Kconfig 尚未生效」的状态下
//   仍能安全工作。
// ------------------------------------------------------------
#include "../../config/autoconf.h"

// 1) 总开关
//
// 语义：**Kconfig 没说**（陈旧的 autoconf.h / 尚未重新生成）时保持开启，
// 退回 February 自己的 config.hpp 默认值。这与 February 长期以来的行为
// 一致 —— 它一直是跑在 config.hpp 的 #ifndef 兜底上；本次只是让它终于
// 被固件引用。
//
// 想在板上真正关掉 February：跑一次干净的 `genconfig.py`（先
// `rm -f config/autoconf.h config/autoconf.cmake .config`），让
// CONFIG_FEBRUARY=n 真正进入 autoconf.h，再在 `menuconfig` 里关。
#define FEBRUARY_COMPILED 1

// 2) 以下覆盖仅在 Kconfig 真正表态时才生效，否则交回 config.hpp 默认值。
#if defined(CONFIG_FEBRUARY)

#ifdef CONFIG_FEBRUARY_SERVICE
#define FEBRUARY_ENABLE_SERVICE 1
#else
#define FEBRUARY_ENABLE_SERVICE 0
#endif
#ifdef CONFIG_FEBRUARY_PLANNER
#define FEBRUARY_ENABLE_PLANNER 1
#else
#define FEBRUARY_ENABLE_PLANNER 0
#endif

#ifdef CONFIG_FEBRUARY_SOFTBUS
#define FEBRUARY_ENABLE_SOFTBUS 1
#else
#define FEBRUARY_ENABLE_SOFTBUS 0
#endif

#ifdef CONFIG_FEBRUARY_PEER_TABLE
#define FEBRUARY_ENABLE_PEER_TABLE 1
#else
#define FEBRUARY_ENABLE_PEER_TABLE 0
#endif

#ifdef CONFIG_FEBRUARY_WORLD_MODEL
#define FEBRUARY_ENABLE_WORLD_MODEL 1
#else
#define FEBRUARY_ENABLE_WORLD_MODEL 0
#endif

#ifdef CONFIG_FEBRUARY_WORKING_MEMORY
#define FEBRUARY_ENABLE_WORKING_MEMORY 1
#else
#define FEBRUARY_ENABLE_WORKING_MEMORY 0
#endif

#ifdef CONFIG_FEBRUARY_EPISODIC_MEMORY
#define FEBRUARY_ENABLE_EPISODIC_MEMORY 1
#else
#define FEBRUARY_ENABLE_EPISODIC_MEMORY 0
#endif

// 3) 命名与 config.hpp 一致的符号：只需剥掉 CONFIG_ 前缀
#ifdef CONFIG_FEBRUARY_REMOTE_YIELD_TO_LOCAL
#define FEBRUARY_REMOTE_YIELD_TO_LOCAL 1
#endif

#ifdef CONFIG_FEBRUARY_PLANNER_MAX_STEPS
#define FEBRUARY_PLANNER_MAX_STEPS CONFIG_FEBRUARY_PLANNER_MAX_STEPS
#endif

#ifdef CONFIG_FEBRUARY_SERVICE_MAX_EVENTS
#define FEBRUARY_SERVICE_MAX_EVENTS CONFIG_FEBRUARY_SERVICE_MAX_EVENTS
#endif

#ifdef CONFIG_FEBRUARY_SOFTBUS_QUEUE_DEPTH
#define FEBRUARY_SOFTBUS_QUEUE_DEPTH CONFIG_FEBRUARY_SOFTBUS_QUEUE_DEPTH
#endif

#ifdef CONFIG_FEBRUARY_SOFTBUS_MAX_SESSIONS
#define FEBRUARY_SOFTBUS_MAX_SESSIONS CONFIG_FEBRUARY_SOFTBUS_MAX_SESSIONS
#endif

#ifdef CONFIG_FEBRUARY_PEER_TABLE_SIZE
#define FEBRUARY_PEER_TABLE_SIZE CONFIG_FEBRUARY_PEER_TABLE_SIZE
#endif

#ifdef CONFIG_FEBRUARY_SOFTBUS_PKG
#define FEBRUARY_SOFTBUS_PKG CONFIG_FEBRUARY_SOFTBUS_PKG
#endif

#ifdef CONFIG_FEBRUARY_SOFTBUS_SESSION
#define FEBRUARY_SOFTBUS_SESSION CONFIG_FEBRUARY_SOFTBUS_SESSION
#endif

// 4) 记忆子系统规模（本次新增的 Kconfig 符号，见 ai/february/Kconfig）
#ifdef CONFIG_FEBRUARY_WORLD_MODEL_SIZE
#define FEBRUARY_WORLD_MODEL_SIZE CONFIG_FEBRUARY_WORLD_MODEL_SIZE
#endif

#ifdef CONFIG_FEBRUARY_WORKING_MEMORY_SLOTS
#define FEBRUARY_WORKING_MEMORY_SLOTS CONFIG_FEBRUARY_WORKING_MEMORY_SLOTS
#endif

#ifdef CONFIG_FEBRUARY_WORKING_MEMORY_WINDOW_MS
#define FEBRUARY_WORKING_MEMORY_WINDOW_MS CONFIG_FEBRUARY_WORKING_MEMORY_WINDOW_MS
#endif

#ifdef CONFIG_FEBRUARY_EPISODIC_MAX_HABITS
#define FEBRUARY_EPISODIC_MAX_HABITS CONFIG_FEBRUARY_EPISODIC_MAX_HABITS
#endif
#endif // CONFIG_FEBRUARY 已表态：用 Kconfig 的值

#endif // AURORA_FEBRUARY_KCONFIG_GLUE_HPP
