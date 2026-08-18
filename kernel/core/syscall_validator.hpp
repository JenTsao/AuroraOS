// =============================================================================
// kernel/core/syscall_validator.hpp
//
// 用户态指针与内存区间合法性校验器 (SyscallValidator)
// 核心职责：
//   1. 防止用户态向内核传入非法野指针、NULL 或跨特权级越界指针 (Pointer Validation)
//   2. 全面支持用户态编程模型区间：
//      - 任务私有栈空间 (Task Stack)
//      - 任务 MPU / PMP 沙盒保护区域 (Task Sandbox Region)
//      - 用户堆 / 动态分配缓冲区 (Heap / Dynamic Memory)
//      - 全局静态数据与 BSS 段 (Data / BSS Sections)
//      - MMU 虚拟地址空间已映射的用户页 (MMU User Pages)
//      - 只读代码段与字符串常量 (Flash / .rodata, 仅限只读校验，禁止作为写目标)
// =============================================================================
#ifndef SYSCALL_VALIDATOR_HPP
#define SYSCALL_VALIDATOR_HPP

#include <stdint.h>
#include <stddef.h>

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

class SyscallValidator {
public:
    // 基于调用任务完整上下文的通用内存指针校验接口
    // 参数:
    //   ptr: 用户传入的内存起始指针
    //   len: 请求访问的字节长度
    //   task: 当前调用任务的 TCB 指针
    //   need_write: 是否需要写权限 (true: 必须为可写内存; false: 只读或读写皆可)
    [[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t len,
                                                const TaskControlBlock* task,
                                                bool need_write = false) noexcept;

    // 兼容接口：基于栈基址与大小的校验接口 (同时支持堆/数据/BSS与Flash)
    [[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t len,
                                                uintptr_t task_stack_base,
                                                size_t task_stack_size,
                                                bool need_write = false) noexcept;
};

} // namespace kernel
} // namespace auroraos

#endif // SYSCALL_VALIDATOR_HPP
