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
// 0) 引入 Kconfig 生成的真相来源
//
// 固件构建靠 board.cmake 的 `-include autoconf.h` 把它塞进每个 TU；
// 主机测试构建**没有**这个 flag（tests/CMakeLists.txt 只 -include
// tests/stubs/host_prelude.hpp）。若本文件依赖「宏已经被预包含」，那么
// 同一份胶水代码在固件里启用、在 host 单测里却整段被 #ifdef 编译掉 ——
// 测试就成了摆设。
//
// 因此这里显式引入。注意 config/autoconf.h 是 genconfig.py 生成的产物，
// **没有 include guard**，固件侧会被包含两次（命令行一次 + 这里一次）——
// 因为两次内容逐字节相同，重复的 #define 属「相同重定义」，合法且不告警。
// 不要给 autoconf.h 手工加 guard：下次 genconfig 会把它冲掉
// （AGENTS.md §25：生成文件不手工编辑）。
// ------------------------------------------------------------
#include "../../config/autoconf.h"

// 1) 总开关
//
// 固件胶水层（apps/watch/february_glue.cpp）需要知道「本构建到底有没有把
// February 编进来」。与其在各处散落 #ifdef CONFIG_FEBRUARY，不如在这里
// 收敛成一个语义明确的宏，调用点只认这一个。
#ifdef CONFIG_FEBRUARY
#define FEBRUARY_COMPILED 1
#else
#define FEBRUARY_COMPILED 0
#endif

// 2) 布尔开关：Kconfig 的 <X> → config.hpp 的 FEBRUARY_ENABLE_<X>
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

#endif // AURORA_FEBRUARY_KCONFIG_GLUE_HPP
