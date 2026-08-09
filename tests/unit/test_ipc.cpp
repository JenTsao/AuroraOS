#include <gtest/gtest.h>
#include "../../kernel/task.hpp"
#include "../../kernel/ipc.hpp"
#include "../../kernel/cspace.hpp"

using namespace auroraos::kernel;

TEST(IpcTest, FastpathCallAndReceive) {
    Scheduler::instance().init();
    
    // Create sender
    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));
    ASSERT_NE(sender, nullptr);
    
    // Create receiver
    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));
    ASSERT_NE(receiver, nullptr);
    
    Endpoint ep;
    
    // Setup message buffers
    char send_msg[] = "Hello IPC";
    char reply_msg[] = "Reply IPC";
    
    char recv_buf[32] = {0};
    char recv_reply_buf[32] = {0};
    
    // 1. Receiver waits for message
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(receiver->ipc_state, IpcState::Receiving);
    
    // 2. Sender calls endpoint
    ep.call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    
    // Verify fastpath happened: receiver is Ready, sender is ReplyBlocked
    EXPECT_EQ(receiver->ipc_state, IpcState::Ready);
    EXPECT_EQ(sender->ipc_state, IpcState::ReplyBlocked);
    EXPECT_STREQ(recv_buf, "Hello IPC");
    EXPECT_EQ(receiver->ipc_sender_id, sender->id);
    
    // 3. Receiver replies
    Endpoint::reply(receiver, receiver->ipc_sender_id, reply_msg, sizeof(reply_msg));
    
    // Verify reply fastpath: sender is Ready
    EXPECT_EQ(sender->ipc_state, IpcState::Ready);
    EXPECT_STREQ(recv_reply_buf, "Reply IPC");
}

TEST(IpcTest, SenderBlocksUntilReceiverReady) {
    Scheduler::instance().init();
    
    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));
    
    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));
    
    Endpoint ep;
    
    char send_msg[] = "Blocked Msg";
    char recv_buf[32] = {0};
    char recv_reply_buf[32] = {0};
    
    // 1. Sender calls first (Receiver not ready)
    ep.call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    EXPECT_EQ(sender->ipc_state, IpcState::Sending);
    
    // 2. Receiver calls receive
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    
    // Verify fastpath: message copied, sender moves to ReplyBlocked
    EXPECT_EQ(sender->ipc_state, IpcState::ReplyBlocked);
    EXPECT_STREQ(recv_buf, "Blocked Msg");
}
