#ifndef AURORA_TOUCH_ABI_HPP
#define AURORA_TOUCH_ABI_HPP

// ========================================================
// 触摸数据跨层 ABI 布局守卫 (compile-time layout guard)
// ========================================================
//
// 为什么需要这个文件：
//   触摸数据以“裸结构体”形式跨越任务边界与共享内存，从内核驱动层 ->
//   手势层 -> UI 派发层 -> 用户态。链路上传递的不是指针、不是消息队列，
//   而是结构体本身，因此**二进制布局就是 ABI**。
//   一旦布局漂移，内核写、用户态读，字段会错位。这类 bug 在源码层面
//   完全看不出来，只会在运行时表现为“触摸坐标乱跳”，极难定位。
//   本文件把所有跨层结构的布局约束固化为 static_assert，让布局漂移
//   在**编译期**就变成编译错误，而不是运行时的坐标错乱。
//
// 为什么这里不直接用 <type_traits>：
//   boards/qemu/rv32_virt/board.cmake:52 与 boards/st/nucleo-l031k6/board.cmake:39
//   都带 -nostdlib++，固件侧拿不到 libstdc++ 头文件；而宿主机测试能拿到。
//   若直接 include <type_traits>，-nostdlib++ 板型直接编译失败。
//
//   本文件的方案是：**完全不依赖任何 C++ 标准库头文件**，改用编译器内建：
//     - offsetof            -> <stddef.h> 的 offsetof（freestanding C 头，
//                              C++ 标准强制要求 freestanding 实现提供，
//                              与 -nostdlib++ 无关，始终可用）
//     - is_standard_layout  -> __is_standard_layout(T)   (GCC/Clang 内建)
//     - is_trivially_copyable -> __is_trivially_copyable(T) (GCC/Clang 内建)
//
//   这些内建在 GCC 与 Clang 上均可用，且不依赖 libstdc++。
//   已实测在 arm-none-eabi-g++ 13.2.1 的 `-ffreestanding -nostdlib++`
//   下编译通过，因此固件侧与宿主机侧走的是**同一份实现**，
//   不需要宏切换、不需要两套代码路径，也就不存在
//   “宿主侧过了但固件侧没过”的分叉风险。
//
//   注意一个易踩的坑：在 -ffreestanding 下 <cstddef> 不可用，
//   此时裸用 offsetof 会报 “‘offsetof’ was not declared in this scope”。
//   必须 include <stddef.h>（C 头，freestanding 下保证存在）而不是 <cstddef>。
//
// 校验目标（均为跨任务/跨层传递的裸结构体）：
//   TouchPoint     —— 驱动 -> UI 派发层
//   InputEvent     —— 在 VFS read/write 中流转的数据体
//   GestureEvent   —— 手势层 -> UI 层
//   RawTouchEvent  —— 汇顶 GT316 等驱动 -> 手势层
//
// 维护约定：
//   任何人修改上述结构的字段顺序、类型或增删字段，都必须同步更新本文件。
//   这不是可选的礼节 —— 漏改意味着 ABI 静默漂移，编译期守卫失效。
//   若确实需要变更布局，应视为**破坏性变更**，必须同时更新所有读写方。
// ========================================================

#include <stddef.h>  // offsetof（freestanding C 头，-nostdlib++ 下可用）
#include <stdint.h>

#include "input_event.hpp"     // TouchPoint / InputEvent
#include "gesture_recognizer.hpp"  // GestureEvent / RawTouchEvent

// ========================================================
// 0. 布局常量的单一事实来源 (single source of truth)
// ========================================================
// 这些常量由编译器从真实类型计算得出，不是手写数字。
// 若布局发生漂移，static_assert 会失败——而常量本身会自动跟着变，
// 因此下面的断言会准确指出“哪一条约束被打破”，而不是整体静默通过。
namespace aurora_touch_abi {

// 汇总对齐与总大小（由编译器计算，用于下方断言）
#define AURORA_ABI_SIZEOF(T)      sizeof(T)
#define AURORA_ABI_ALIGNOF(T)     alignof(T)
#define AURORA_ABI_OFFSETOF(T, m) offsetof(T, m)

}  // namespace aurora_touch_abi

// ========================================================
// 1. TouchPoint —— 驱动层 -> UI 派发层
// ========================================================
// 布局：x@0(2) y@2(2) state@4(1) is_valid@5(1)，sizeof=6 align=2
//
// 偏移来源：
//   x      @0  uint16_t 天然 2 字节对齐，结构体首字段偏移必为 0
//   y      @2  紧跟 x 之后，2+2=4 无空隙
//   state  @4  TouchState 底层类型为 uint8_t（见 input_event.hpp:14），
//                  故占 1 字节；4+2=6 之前无空隙
//   is_valid@5 bool 在 ARM EABI / x86-64 SysV 下均为 1 字节，5+1=6 无空隙
//   sizeof = 6：成员之和 2+2+1+1 = 6，**无 padding**，布局紧凑
//   align   = 2：取各成员最大对齐 2
//
// 该结构体无空隙且为全标量，是最理想的共享内存载荷。

static_assert(AURORA_ABI_SIZEOF(TouchPoint) == 6,
              "TouchPoint ABI: sizeof 发生变化。跨层共享内存布局已漂移，"
              "必须同步更新 touch_abi.hpp 与所有读写方（属破坏性变更）");
static_assert(AURORA_ABI_ALIGNOF(TouchPoint) == 2,
              "TouchPoint ABI: alignof 应为 2（最大成员对齐 = uint16_t）");
static_assert(AURORA_ABI_OFFSETOF(TouchPoint, x) == 0,
              "TouchPoint ABI: x 是首成员，偏移必须为 0");
static_assert(AURORA_ABI_OFFSETOF(TouchPoint, y) == 2,
              "TouchPoint ABI: y 应紧跟 x（2 字节）之后，偏移为 2");
static_assert(AURORA_ABI_OFFSETOF(TouchPoint, state) == 4,
              "TouchPoint ABI: state 应在 y（uint16_t）之后，偏移为 4");
static_assert(AURORA_ABI_OFFSETOF(TouchPoint, is_valid) == 5,
              "TouchPoint ABI: is_valid 应紧跟 state（uint8_t 枚举），偏移为 5");
static_assert(__is_standard_layout(TouchPoint),
              "TouchPoint ABI: 必须是 standard-layout，"
              "否则跨层字段偏移无保证，禁止用于共享内存传递");
static_assert(__is_trivially_copyable(TouchPoint),
              "TouchPoint ABI: 必须可平凡复制，"
              "共享内存按字节拷贝时不得触发用户定义的拷贝语义");

// ========================================================
// 2. InputEvent —— VFS read/write 中流转的数据体
// ========================================================
// 布局：timestamp@0(4) type@4(1) [pad@5 空洞] code@6(2) value@8(4)
//       sizeof=12 align=4
//
// 偏移来源：
//   timestamp@0  uint32_t 天然 4 字节对齐，首字段偏移 0
//   type      @4  InputEventType 底层类型为 **uint8_t**
//                 （见 input_event.hpp:7，`: uint8_t`），
//                 故只占 1 字节，位于偏移 4
//   ── 偏移 5 是 1 字节 padding 空洞 ──
//   code      @6  uint16_t 要求 2 字节对齐，不能放在偏移 5，
//                 编译器将其推到下一个满足 2 字节对齐的偏移 6
//   value     @8  int32_t 要求 4 字节对齐，6+2=8 恰好满足
//   sizeof = 12：成员之和 4+1+2+4 = 11，实际 12，**存在 1 字节空洞**（偏移 5）
//   align   = 4：取各成员最大对齐 4
//
// 【已知 ABI 风险 — 有意保留，未修复】
//   偏移 5 的 1 字节空洞带来两个问题：
//     1) sizeof(12) > 成员和(11)，跨层拷贝会多带 1 字节未初始化数据。
//        若该结构体被 memset 或整体赋值，padding 字节内容不确定；
//        在共享内存场景下这可能造成**信息泄漏**（残留旧数据）。
//     2) 空洞位置隐式依赖枚举的底层类型。若将来有人把
//        `InputEventType : uint8_t` 改成 `: uint16_t`（或去掉底层类型
//        让编译器选默认底层类型），偏移会整体改变，sizeof 变化。
//   消除空洞需要调整字段顺序（例如 type 与 code 调换），这**会改变
//   二进制布局 = 破坏性 ABI 变更**，所有读写方都要同步修改。
//   因此本守卫只做断言与标注，不擅自修改结构体定义。

static_assert(AURORA_ABI_SIZEOF(InputEvent) == 12,
              "InputEvent ABI: sizeof 发生变化。注意 type 与 code 之间存在 "
              "1 字节 padding 空洞（见本文件说明），若 sizeof 改变说明"
              "枚举底层类型或字段顺序被改动，属破坏性 ABI 变更");
static_assert(AURORA_ABI_ALIGNOF(InputEvent) == 4,
              "InputEvent ABI: alignof 应为 4（最大成员对齐 = int32_t）");
static_assert(AURORA_ABI_OFFSETOF(InputEvent, timestamp) == 0,
              "InputEvent ABI: timestamp 是首成员，偏移必须为 0");
static_assert(AURORA_ABI_OFFSETOF(InputEvent, type) == 4,
              "InputEvent ABI: type 应紧随 uint32_t timestamp，偏移为 4。"
              "注意 InputEventType 底层为 uint8_t，只占 1 字节");
static_assert(AURORA_ABI_OFFSETOF(InputEvent, code) == 6,
              "InputEvent ABI: code 应位于偏移 6。偏移 5 是 padding 空洞——"
              "uint16_t 的 2 字节对齐无法满足偏移 5，故被推到 6。"
              "若此断言失败，通常是 type 的底层类型从 uint8_t 被改大了");
static_assert(AURORA_ABI_OFFSETOF(InputEvent, value) == 8,
              "InputEvent ABI: value(int32_t, 4 字节对齐) 应位于偏移 8");
// 固化 padding 空洞的存在：11 字节成员 + 1 字节空洞 = 12。
// 这条断言的意义是把“空洞存在”变成显式的、可搜索的事实。
static_assert(AURORA_ABI_SIZEOF(InputEvent) ==
                  sizeof(uint32_t) + sizeof(InputEventType) + sizeof(uint16_t) + sizeof(int32_t) + 1,
              "InputEvent ABI: 预期的 1 字节 padding 空洞（偏移 5）不再存在。"
              "空洞消失说明结构体被改动过——请同步更新本文件并评估是否"
              "改变了已部署的 ABI");
static_assert(__is_standard_layout(InputEvent),
              "InputEvent ABI: 必须是 standard-layout，"
              "否则跨层字段偏移无保证，禁止用于共享内存传递");
static_assert(__is_trivially_copyable(InputEvent),
              "InputEvent ABI: 必须可平凡复制，"
              "共享内存按字节拷贝时不得触发用户定义的拷贝语义");

// 底层类型固化：type 的宽度直接决定偏移 5 的空洞宽度
static_assert(sizeof(InputEventType) == 1,
              "InputEvent ABI: InputEventType 底层类型必须为 uint8_t。"
              "若放宽为更宽的底层类型，InputEvent 的字段偏移将整体改变");
static_assert(sizeof(TouchState) == 1,
              "TouchPoint ABI: TouchState 底层类型必须为 uint8_t，"
              "否则 TouchPoint 的 sizeof/偏移将改变");

// ========================================================
// 3. GestureEvent —— 手势层 -> UI 层
// ========================================================
// 布局：type@0(1) x@2(2) y@4(2) delta_x@6(2) delta_y@8(2)
//       duration_ms@12(4) is_edge@16(1) [尾随 3 字节] sizeof=20 align=4
//
// 偏移来源：
//   type       @0  GestureType 底层为 uint8_t，占 1 字节
//   ── 偏移 1 是 1 字节 padding（为让 x 满足 2 字节对齐）──
//   x          @2  uint16_t 对齐 2；1 字节不够，故推到 2
//   y          @4  2+2=4 无空隙
//   delta_x    @6  4+2=6 无空隙
//   delta_y    @8  6+2=8 无空隙
//   ── 偏移 10-11 是 2 字节 padding（为让 duration_ms 满足 4 字节对齐）──
//   duration_ms@12 int32 前的 uint32_t 要求 4 字节对齐，
//                  10 不满足 4 的倍数（10 % 4 = 2），故推到 12
//   is_edge    @16 bool 占 1 字节
//   ── 偏移 17-19 是 3 字节尾部 padding（结构体需为 align 的倍数，
//                  20 % 4 == 0，故补到 20）──
//   sizeof = 20：成员之和 1+2+2+2+2+4+1 = 14，实际 20，**6 字节空洞**
//   align   = 4
//
// 【已知 ABI 风险 — 有意保留，未修复】
//   6 字节空洞（偏移 1、10-11、17-19）占 sizeof 的 30%。
//   这是“按对齐逐个插入标量字段”写法的典型代价。
//   若该结构体整体跨共享内存传递，padding 字节是未初始化的，
//   存在信息泄漏与比较/哈希不稳定（memcmp 结果不可靠）的风险。
//   消除需要字段重排（把 uint16_t/int16_t 聚在一起、uint32_t 前置），
//   属破坏性 ABI 变更，故此处仅标注不改。
//
//   注：GestureEvent 带有用户提供的构造函数（gesture_recognizer.hpp:61），
//   但该构造函数只初始化成员，不影响平凡复制性与 standard-layout——
//   下面两条断言即为证。

static_assert(AURORA_ABI_SIZEOF(GestureEvent) == 20,
              "GestureEvent ABI: sizeof 发生变化（当前 20，含 6 字节 padding）。"
              "跨层布局已漂移，必须同步更新所有读写方");
static_assert(AURORA_ABI_ALIGNOF(GestureEvent) == 4,
              "GestureEvent ABI: alignof 应为 4（最大成员对齐 = uint32_t）");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, type) == 0,
              "GestureEvent ABI: type 是首成员，偏移必须为 0");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, x) == 2,
              "GestureEvent ABI: x 应位于偏移 2。偏移 1 是 padding——"
              "uint16_t 的 2 字节对齐无法满足偏移 1，故被推到 2");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, y) == 4,
              "GestureEvent ABI: y 应紧随 x，偏移为 4");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, delta_x) == 6,
              "GestureEvent ABI: delta_x 应紧随 y，偏移为 6");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, delta_y) == 8,
              "GestureEvent ABI: delta_y 应紧随 delta_x，偏移为 8");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, duration_ms) == 12,
              "GestureEvent ABI: duration_ms(uint32_t) 应位于偏移 12。"
              "偏移 10-11 是 padding——10 不是 4 的倍数，故被推到 12");
static_assert(AURORA_ABI_OFFSETOF(GestureEvent, is_edge) == 16,
              "GestureEvent ABI: is_edge 应紧随 duration_ms，偏移为 16");
// 固化 6 字节 padding：14 字节成员 + 6 字节空洞 = 20
static_assert(AURORA_ABI_SIZEOF(GestureEvent) ==
                  sizeof(GestureType) + sizeof(uint16_t) * 2 + sizeof(int16_t) * 2 +
                      sizeof(uint32_t) + sizeof(bool) + 6,
              "GestureEvent ABI: 预期的 6 字节 padding（偏移 1、10-11、17-19）"
              "不再存在。结构体被改动过——请同步更新本文件");
static_assert(__is_standard_layout(GestureEvent),
              "GestureEvent ABI: 必须是 standard-layout，"
              "否则跨层字段偏移无保证，禁止用于共享内存传递");
static_assert(__is_trivially_copyable(GestureEvent),
              "GestureEvent ABI: 必须可平凡复制。"
              "注意该结构体有用户提供的构造函数，若此断言失败说明"
              "有人加入了会影响复制语义的成员或虚函数");
static_assert(sizeof(GestureType) == 1,
              "GestureEvent ABI: GestureType 底层类型必须为 uint8_t，"
              "否则 GestureEvent 的字段偏移将改变");

// ========================================================
// 4. RawTouchEvent —— 汇顶 GT316 等驱动 -> 手势层
// ========================================================
// 布局：x@0(2) y@2(2) state@4(1) [pad@5-7 空洞] timestamp@8(4)
//       sizeof=12 align=4
//
// 偏移来源：
//   x        @0  uint16_t 对齐 2，首字段偏移 0
//   y        @2  2+2=4 无空隙
//   state    @4  TouchState 底层 uint8_t，占 1 字节
//   ── 偏移 5-7 是 3 字节 padding（为让 timestamp 满足 4 字节对齐）──
//   timestamp@8 uint32_t 要求 4 字节对齐；4+1=5，5 % 4 = 1，
//                  故推到 8，中间补 3 字节
//   sizeof = 12：成员之和 2+2+1+4 = 9，实际 12，**3 字节空洞**
//   align   = 4
//
// 【已知 ABI 风险 — 有意保留，未修复】
//   3 字节空洞同样带来未初始化字节泄漏与 memcmp 不可靠的风险。
//   若把 timestamp 挪到 x/y 之前（或给 state 补显式对齐），
//   可将其压到 10 字节甚至更小——但这会改变二进制布局 = 破坏性变更。
//   故此处仅标注，不擅自修改。

static_assert(AURORA_ABI_SIZEOF(RawTouchEvent) == 12,
              "RawTouchEvent ABI: sizeof 发生变化（当前 12，含 3 字节 padding）");
static_assert(AURORA_ABI_ALIGNOF(RawTouchEvent) == 4,
              "RawTouchEvent ABI: alignof 应为 4（最大成员对齐 = uint32_t）");
static_assert(AURORA_ABI_OFFSETOF(RawTouchEvent, x) == 0,
              "RawTouchEvent ABI: x 是首成员，偏移必须为 0");
static_assert(AURORA_ABI_OFFSETOF(RawTouchEvent, y) == 2,
              "RawTouchEvent ABI: y 应紧随 x，偏移为 2");
static_assert(AURORA_ABI_OFFSETOF(RawTouchEvent, state) == 4,
              "RawTouchEvent ABI: state(TouchState, uint8_t) 应位于偏移 4");
static_assert(AURORA_ABI_OFFSETOF(RawTouchEvent, timestamp) == 8,
              "RawTouchEvent ABI: timestamp(uint32_t) 应位于偏移 8。"
              "偏移 5-7 是 padding——5 不是 4 的倍数，故被推到 8");
static_assert(AURORA_ABI_SIZEOF(RawTouchEvent) ==
                  sizeof(uint16_t) + sizeof(uint16_t) + sizeof(TouchState) + sizeof(uint32_t) + 3,
              "RawTouchEvent ABI: 预期的 3 字节 padding（偏移 5-7）不再存在。"
              "结构体被改动过——请同步更新本文件");
static_assert(__is_standard_layout(RawTouchEvent),
              "RawTouchEvent ABI: 必须是 standard-layout，"
              "否则跨层字段偏移无保证，禁止用于共享内存传递");
static_assert(__is_trivially_copyable(RawTouchEvent),
              "RawTouchEvent ABI: 必须可平凡复制，"
              "驱动 -> 手势层按字节传递时不得触发用户定义的拷贝语义");

#endif  // AURORA_TOUCH_ABI_HPP
