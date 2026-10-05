// =============================================================================
// tests/unit/test_syscall_ipc.cpp
//
// IPC 内核系统调用处理 (KernelIpc) 与权能模型、超时/非阻塞测试
// =============================================================================
#include <gtest/gtest.h>
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/syscall_ipc.hpp"

using namespace auroraos::kernel;

class SyscallIpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();

        sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));
        receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

        ep = new Endpoint();
    }

    void TearDown() override {
        delete ep;
    }

    uint32_t sender_stack[128];
    uint32_t receiver_stack[128];
    TaskControlBlock* sender;
    TaskControlBlock* receiver;
    Endpoint* ep;
};

TEST_F(SyscallIpcTest, SuccessfulIpcWithCapabilities) {
    // 1. Setup Capabilities
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0}; // Write only
    sender->security.cspace[1].object = ep;

    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0}; // Read only
    receiver->security.cspace[2].object = ep;

    char send_msg[] = "Hello System";
    char recv_buf[32] = {0};

    // 2. Receiver calls sys_ipc_receive on slot 2 (blocks waiting for message)
    int ok_recv = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ok_recv, static_cast<int>(IpcStatus::Blocked));
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);

    // 3. Sender calls sys_ipc_call on slot 1 (delivers message to receiver, enters ReplyBlocked)
    int ok_call = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ok_call, static_cast<int>(IpcStatus::Blocked));

    // 4. Verification
    EXPECT_STREQ(recv_buf, "Hello System");
    EXPECT_EQ(receiver->ipc.sender_id, sender->scheduler.id);
}

TEST_F(SyscallIpcTest, CallFailsWithoutWriteRights) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {1, 0, 0, 0}; // Read only, missing Write
    sender->security.cspace[1].object = ep;

    char send_msg[] = "Hello";
    int ok_call = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), nullptr, 0);

    EXPECT_EQ(ok_call, static_cast<int>(IpcStatus::NoPermission));
    EXPECT_EQ(sender->ipc.state, IpcState::Ready); // State unchanged
}

TEST_F(SyscallIpcTest, ReceiveFailsWithoutReadRights) {
    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {0, 1, 0, 0}; // Write only, missing Read
    receiver->security.cspace[2].object = ep;

    char recv_buf[32] = {0};
    int ok_recv = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf));

    EXPECT_EQ(ok_recv, static_cast<int>(IpcStatus::NoPermission));
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready); // State unchanged
}

TEST_F(SyscallIpcTest, CallFailsWithInvalidSlotOrType) {
    // Empty slot
    int ok_call = KernelIpc::sys_ipc_call(sender, 1, nullptr, 0, nullptr, 0);
    EXPECT_EQ(ok_call, static_cast<int>(IpcStatus::NoPermission));

    // Wrong type
    sender->security.cspace[1].type = CapType::Memory;
    sender->security.cspace[1].rights = {1, 1, 1, 0};
    sender->security.cspace[1].object = nullptr;
    ok_call = KernelIpc::sys_ipc_call(sender, 1, nullptr, 0, nullptr, 0);
    EXPECT_EQ(ok_call, static_cast<int>(IpcStatus::NoPermission));
}

TEST_F(SyscallIpcTest, NonBlockingAndTimedSyscallIpc) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0}; // Write
    sender->security.cspace[1].object = ep;

    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0}; // Read
    receiver->security.cspace[2].object = ep;

    char send_msg[] = "Nonblock Syscall";
    char recv_buf[32] = {0};
    char reply_buf[32] = {0};

    // 1. 无 receiver 时以 IPC_NONBLOCK 调用，应返回 WouldBlock
    int call_res = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf), IPC_NONBLOCK);
    EXPECT_EQ(call_res, static_cast<int>(IpcStatus::WouldBlock));
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);

    // 2. 无 sender 时以 IPC_NONBLOCK 接收，应返回 WouldBlock
    int recv_res = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf), nullptr, IPC_NONBLOCK);
    EXPECT_EQ(recv_res, static_cast<int>(IpcStatus::WouldBlock));
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);

    // 3. 带 3 ticks 超时发起 call
    call_res = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf), 3);
    EXPECT_EQ(sender->ipc.state, IpcState::Sending);

    // 推进 3 ticks
    for (int i = 0; i < 3; i++) {
        Scheduler::instance().tick_update();
    }
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_EQ(sender->ipc.status, IpcStatus::Timeout);
}

// =============================================================================
// 阻塞式 IPC 的返回值投递
//
// 真实内核里，返回槽由 SVC 入口装填（Cortex-M 是硬件压栈帧里的 r0，RV32 是 trap
// 帧里的 a0），任务阻塞期间该内存一直留在它自己的栈上。下面用一块模拟栈的局部
// 变量充当那个槽，验证终态唤醒点是否把真实结果写了回去。
// =============================================================================

static constexpr uint32_t kSentinel = 0xDEAD0000u;

// 1. 接收方阻塞后被发送方唤醒：返回槽必须拿到 Ok，且 out_sender_id 是本次的发送方
TEST_F(SyscallIpcTest, BlockingReceiveDeliversFinalStatusAndSender) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0}; // Write
    sender->security.cspace[1].object = ep;

    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0}; // Read
    receiver->security.cspace[2].object = ep;

    uint32_t fake_return_reg = kSentinel;
    uint32_t sender_id_out = kSentinel;
    uint32_t badge_out = kSentinel;
    receiver->ipc.ipc_ret_slot = &fake_return_reg;

    char send_msg[] = "Delivered";
    char recv_buf[32] = {0};

    int recv_res = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf), &sender_id_out, &badge_out,
                                              IPC_TIMEOUT_INFINITE, 0);
    // 挂起时函数只能报告"还在等"，但绝不能把这个中间态写进返回寄存器
    EXPECT_EQ(recv_res, static_cast<int>(IpcStatus::Blocked));
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);
    EXPECT_EQ(fake_return_reg, kSentinel) << "阻塞尚未结束就改写了返回值";
    EXPECT_EQ(sender_id_out, kSentinel) << "把上一次的 sender_id 残值写给了用户";

    // 发送方投递，走 Endpoint::call 的 fast-path 唤醒接收方
    int call_res = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), nullptr, 0);
    EXPECT_EQ(call_res, static_cast<int>(IpcStatus::Blocked)); // 发送方转入 ReplyBlocked

    EXPECT_EQ(fake_return_reg, static_cast<uint32_t>(static_cast<int>(IpcStatus::Ok)));
    EXPECT_EQ(sender_id_out, receiver->ipc.sender_id);
    EXPECT_EQ(sender_id_out, sender->scheduler.id);
    EXPECT_STREQ(recv_buf, "Delivered");
    // 投递完必须作废，否则下一次无关的唤醒会写进失效的栈帧
    EXPECT_EQ(receiver->ipc.ipc_ret_slot, nullptr);
    EXPECT_EQ(receiver->ipc.pending_sender_id_out, nullptr);
    EXPECT_EQ(receiver->ipc.pending_badge_out, nullptr);
}

// 2. 阻塞接收超时：返回槽拿到 Timeout 而不是停在 Blocked
TEST_F(SyscallIpcTest, BlockingReceiveTimeoutIsDelivered) {
    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0};
    receiver->security.cspace[2].object = ep;

    uint32_t fake_return_reg = kSentinel;
    receiver->ipc.ipc_ret_slot = &fake_return_reg;

    char recv_buf[32] = {0};
    int res = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf), nullptr, nullptr, 4u, 0);
    EXPECT_EQ(res, static_cast<int>(IpcStatus::Blocked));

    for (uint32_t i = 0; i < 4u; i++) {
        Scheduler::instance().tick_update();
    }

    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);
    EXPECT_EQ(receiver->ipc.status, IpcStatus::Timeout);
    EXPECT_EQ(fake_return_reg, static_cast<uint32_t>(static_cast<int>(IpcStatus::Timeout)));
    EXPECT_EQ(receiver->ipc.ipc_ret_slot, nullptr);
}

// 3. 阻塞 call 等待应答：Endpoint::reply 必须把 Ok 投递回发送方的返回寄存器
TEST_F(SyscallIpcTest, BlockingCallReplyDeliversToSender) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0};
    sender->security.cspace[1].object = ep;

    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0};
    receiver->security.cspace[2].object = ep;

    uint32_t fake_return_reg = kSentinel;
    sender->ipc.ipc_ret_slot = &fake_return_reg;

    char send_msg[] = "Request";
    char reply_msg[] = "Response";
    char reply_buf[32] = {0};
    char recv_buf[64] = {0};

    // 发送方先阻塞
    EXPECT_EQ(KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf)),
              static_cast<int>(IpcStatus::Blocked));
    EXPECT_EQ(sender->ipc.state, IpcState::Sending);
    EXPECT_EQ(fake_return_reg, kSentinel);

    // 接收方取走消息，发送方转入 ReplyBlocked（仍然挂起，返回值不该出现）
    EXPECT_EQ(KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf)), static_cast<int>(IpcStatus::Ok));
    EXPECT_EQ(sender->ipc.state, IpcState::ReplyBlocked);
    EXPECT_EQ(fake_return_reg, kSentinel);

    // 应答后才把 Ok 送回发送方
    EXPECT_EQ(KernelIpc::sys_ipc_reply(receiver, receiver->ipc.sender_id, reply_msg, sizeof(reply_msg)),
              static_cast<int>(IpcStatus::Ok));
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_EQ(fake_return_reg, static_cast<uint32_t>(static_cast<int>(IpcStatus::Ok)));
    EXPECT_STREQ(reply_buf, "Response");
    EXPECT_EQ(sender->ipc.ipc_ret_slot, nullptr);
}

// 4. 非阻塞调用没有挂起任务，返回槽作废之后不得再被任何唤醒路径改写
TEST_F(SyscallIpcTest, NonBlockingCallAbandonsReturnSlot) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0};
    sender->security.cspace[1].object = ep;

    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0};
    receiver->security.cspace[2].object = ep;

    uint32_t fake_return_reg = kSentinel;
    sender->ipc.ipc_ret_slot = &fake_return_reg;

    char send_msg[] = "Nb";
    char reply_buf[32] = {0};

    // 有接收方在等 → 投递成功，发送方进入 AwaitReply，本次调用没有挂起
    char recv_buf[32] = {0};
    EXPECT_EQ(ep->receive(receiver, recv_buf, sizeof(recv_buf)), IpcStatus::Blocked);
    EXPECT_EQ(KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf),
                                      IPC_NONBLOCK),
              static_cast<int>(IpcStatus::Ok));
    EXPECT_EQ(sender->ipc.state, IpcState::AwaitReply);

    // SyscallDispatcher::dispatch 出口处的等价动作
    Endpoint::settle_ipc_return_slot(sender);
    EXPECT_EQ(sender->ipc.ipc_ret_slot, nullptr);

    // 之后的应答只能写 reply_buf，绝不能穿透到上一次系统调用的栈帧
    uint32_t sid = receiver->ipc.sender_id;
    EXPECT_EQ(Endpoint::reply(receiver, sid, reply_buf, sizeof("Nb")), IpcStatus::Ok);
    EXPECT_EQ(fake_return_reg, kSentinel) << "把结果写进了已经失效的异常帧";
}
