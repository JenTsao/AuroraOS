# AuroraOS hacker_app_task (task id=5) 栈损坏定位脚本
# 用法:
#   终端1: qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic \
#          -kernel build_qemu4/auroraOS.elf -s -S
#   终端2: arm-none-eabi-gdb build_qemu4/auroraOS.elf -x aurora_debug.gdb

target remote :1234
set pagination off
set print pretty on

# ---- 1. 在 sleep_ms 返回、即将执行降权+越权写之前打点 ----
break hacker_app_task
commands
  printf "\n=== [BP] hacker_app_task ENTRY, PSP=%p ===\n", $psp
  continue
end

# ---- 2. 每次 PendSV 触发时记录关键寄存器，重点看 task5 相关的两次 ----
break PendSV_Handler
commands
  printf "\n--- [PendSV] PSP(old)=%p  g_current_tcb=%p  g_next_tcb=%p ---\n", \
         $psp, g_current_tcb_ptr, g_next_tcb_ptr
  # 打印 next tcb 的 id / stack_ptr（TCB 第一个成员是 stack_ptr, 需按结构体偏移读）
  printf "    next_tcb->stack_ptr = 0x%x\n", *(unsigned int*)g_next_tcb_ptr
  continue
end

# ---- 3. UsageFault 处理函数入口，此时打印故障寄存器与 hacker_stack 内容 ----
break UsageFault_Handler
commands
  printf "\n!!! [FAULT] UsageFault_Handler hit !!!\n"
  printf "hacker_stack (first 64 bytes):\n"
  x/16xw hacker_stack
  printf "hacker_stack (last 64 bytes, near top):\n"
  x/16xw (char*)hacker_stack + sizeof(hacker_stack) - 64
  # 找到 task id=5 的 TCB，打印其 stack_ptr / stack_base / mpu_sandbox
  print Scheduler::instance().tasks[5]
  continue
end

# ---- 4. 关键：在 mpu_switch_sandbox 前后各打一次栈顶内容，对比是否被冲掉 ----
break mpu_switch_sandbox
commands
  printf "\n>>> [mpu_switch_sandbox] ENTER, next=%p\n", next
  if next != 0
    printf "    next->id (offset check manually) — inspect via TCB layout\n"
  end
  continue
end

echo \n=== 断点已设置，输入 c 或 continue 开始运行 ===\n
