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

    // 2. Receiver calls sys_ipc_receive on slot 2
    int ok_recv = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ok_recv, static_cast<int>(IpcStatus::Ok));
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);

    // 3. Sender calls sys_ipc_call on slot 1
    int ok_call = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ok_call, static_cast<int>(IpcStatus::Ok));

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
