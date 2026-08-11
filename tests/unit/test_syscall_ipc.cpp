#include <gtest/gtest.h>
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/syscall_ipc.hpp"

using namespace auroraos::kernel;

class SyscallIpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        
        sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));
        receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));
        
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
    // Sender needs Write capability to Endpoint in slot 1
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {0, 1, 0, 0}; // Write only
    sender->security.cspace[1].object = ep;
    
    // Receiver needs Read capability to Endpoint in slot 2
    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {1, 0, 0, 0}; // Read only
    receiver->security.cspace[2].object = ep;
    
    char send_msg[] = "Hello System";
    char recv_buf[32] = {0};
    
    // 2. Receiver calls sys_ipc_receive on slot 2
    bool ok_recv = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf));
    EXPECT_TRUE(ok_recv);
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);
    
    // 3. Sender calls sys_ipc_call on slot 1
    bool ok_call = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), recv_buf, sizeof(recv_buf));
    EXPECT_TRUE(ok_call);
    
    // 4. Verification
    EXPECT_STREQ(recv_buf, "Hello System");
    EXPECT_EQ(receiver->ipc.sender_id, sender->scheduler.id);
}

TEST_F(SyscallIpcTest, CallFailsWithoutWriteRights) {
    sender->security.cspace[1].type = CapType::Endpoint;
    sender->security.cspace[1].rights = {1, 0, 0, 0}; // Read only, missing Write
    sender->security.cspace[1].object = ep;
    
    char send_msg[] = "Hello";
    bool ok_call = KernelIpc::sys_ipc_call(sender, 1, send_msg, sizeof(send_msg), nullptr, 0);
    
    EXPECT_FALSE(ok_call);
    EXPECT_EQ(sender->ipc.state, IpcState::Ready); // State unchanged
}

TEST_F(SyscallIpcTest, ReceiveFailsWithoutReadRights) {
    receiver->security.cspace[2].type = CapType::Endpoint;
    receiver->security.cspace[2].rights = {0, 1, 0, 0}; // Write only, missing Read
    receiver->security.cspace[2].object = ep;
    
    char recv_buf[32] = {0};
    bool ok_recv = KernelIpc::sys_ipc_receive(receiver, 2, recv_buf, sizeof(recv_buf));
    
    EXPECT_FALSE(ok_recv);
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready); // State unchanged
}

TEST_F(SyscallIpcTest, CallFailsWithInvalidSlotOrType) {
    // Empty slot
    bool ok_call = KernelIpc::sys_ipc_call(sender, 1, nullptr, 0, nullptr, 0);
    EXPECT_FALSE(ok_call);
    
    // Wrong type
    sender->security.cspace[1].type = CapType::Memory;
    sender->security.cspace[1].rights = {1, 1, 1, 0}; 
    sender->security.cspace[1].object = nullptr;
    ok_call = KernelIpc::sys_ipc_call(sender, 1, nullptr, 0, nullptr, 0);
    EXPECT_FALSE(ok_call);
}
