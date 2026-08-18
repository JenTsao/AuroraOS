// =============================================================================
// tests/unit/test_ipc.cpp
//
// 统一 IPC 端点消息传递、非阻塞 (nb_call / nb_receive) 与超时截止时间全覆盖测试
// =============================================================================
#include <gtest/gtest.h>
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/ipc.hpp"
#include "../../kernel/core/cspace.hpp"

using namespace auroraos::kernel;

// 1. 同步快速通道测试 (Fastpath)
TEST(IpcTest, FastpathCallAndReceive) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));
    ASSERT_NE(sender, nullptr);

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));
    ASSERT_NE(receiver, nullptr);

    Endpoint ep;

    char send_msg[] = "Hello IPC";
    char reply_msg[] = "Reply IPC";

    char recv_buf[32] = {0};
    char recv_reply_buf[32] = {0};

    // 1. Receiver 阻塞等待接收消息
    IpcStatus recv_st = ep.receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(recv_st, IpcStatus::Ok);
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);

    // 2. Sender 发送调用
    IpcStatus call_st = ep.call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    EXPECT_EQ(call_st, IpcStatus::Ok);

    // 验证 Fastpath：Receiver 被唤醒为 Ready，Sender 进入 ReplyBlocked
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);
    EXPECT_EQ(sender->ipc.state, IpcState::ReplyBlocked);
    EXPECT_STREQ(recv_buf, "Hello IPC");
    EXPECT_EQ(receiver->ipc.sender_id, sender->scheduler.id);

    // 3. Receiver 回复
    IpcStatus reply_st = Endpoint::reply(receiver, receiver->ipc.sender_id, reply_msg, sizeof(reply_msg));
    EXPECT_EQ(reply_st, IpcStatus::Ok);

    // 验证回复 Fastpath：Sender 恢复 Ready
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_STREQ(recv_reply_buf, "Reply IPC");
}

// 2. 发送方先到达并阻塞等待接收方
TEST(IpcTest, SenderBlocksUntilReceiverReady) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    char send_msg[] = "Blocked Msg";
    char recv_buf[32] = {0};
    char recv_reply_buf[32] = {0};

    // 1. Sender 先调用 (Receiver 未就绪)
    IpcStatus call_st = ep.call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    EXPECT_EQ(call_st, IpcStatus::Ok);
    EXPECT_EQ(sender->ipc.state, IpcState::Sending);

    // 2. Receiver 随后调用 receive
    IpcStatus recv_st = ep.receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(recv_st, IpcStatus::Ok);

    // 验证消息成功传递，Sender 转移至 ReplyBlocked
    EXPECT_EQ(sender->ipc.state, IpcState::ReplyBlocked);
    EXPECT_STREQ(recv_buf, "Blocked Msg");
}

// 3. 非阻塞 IPC (nb_call / nb_receive) 测试
TEST(IpcTest, NonBlockingCallAndReceive) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    char send_msg[] = "Non-blocking Test";
    char recv_buf[32] = {0};
    char recv_reply_buf[32] = {0};

    // 1. 在没有 receiver 等待时，nb_call 立即返回 WouldBlock，sender 保持 Ready 不阻塞
    IpcStatus nb_call_st = ep.nb_call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    EXPECT_EQ(nb_call_st, IpcStatus::WouldBlock);
    EXPECT_EQ(sender->ipc.status, IpcStatus::WouldBlock);
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);

    // 2. 在没有 sender 等待时，nb_receive 立即返回 WouldBlock，receiver 保持 Ready 不阻塞
    IpcStatus nb_recv_st = ep.nb_receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(nb_recv_st, IpcStatus::WouldBlock);
    EXPECT_EQ(receiver->ipc.status, IpcStatus::WouldBlock);
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);

    // 3. 当 receiver 已经阻塞在 ep.receive 时，nb_call 应成功传递消息并立即返回 Ok
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);

    nb_call_st = ep.nb_call(sender, send_msg, sizeof(send_msg), recv_reply_buf, sizeof(recv_reply_buf));
    EXPECT_EQ(nb_call_st, IpcStatus::Ok);
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);
    EXPECT_STREQ(recv_buf, "Non-blocking Test");
}

// 4. 发送方超时截止时间 (Sender Timeout) 测试
TEST(IpcTest, SenderTimeoutExpires) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));

    Endpoint ep;
    char send_msg[] = "Timeout sender";
    char recv_reply[32] = {0};

    // 发送方设置 5 ticks 超时时间入队等待接收方
    IpcStatus st = ep.call(sender, send_msg, sizeof(send_msg), recv_reply, sizeof(recv_reply), 5);
    EXPECT_EQ(st, IpcStatus::Ok);
    EXPECT_EQ(sender->ipc.state, IpcState::Sending);
    EXPECT_EQ(sender->scheduler.sleep_ticks, 5u);

    // 调度器推进 4 ticks，尚未超时
    for (int i = 0; i < 4; ++i) {
        Scheduler::instance().tick_update();
    }
    EXPECT_EQ(sender->ipc.state, IpcState::Sending);
    EXPECT_EQ(sender->scheduler.sleep_ticks, 1u);

    // 推进第 5 tick，超时触发！
    Scheduler::instance().tick_update();
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_EQ(sender->ipc.status, IpcStatus::Timeout);
    EXPECT_EQ(sender->scheduler.sleep_ticks, 0u);

    // 验证超时后 sender 已从端点队列自动摘除
    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));
    char recv_buf[32] = {0};

    // receiver 此时尝试非阻塞接收，因 sender 已超时摘除，应返回 WouldBlock
    IpcStatus nb_st = ep.nb_receive(receiver, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(nb_st, IpcStatus::WouldBlock);
}

// 5. 接收方超时截止时间 (Receiver Timeout) 测试
TEST(IpcTest, ReceiverTimeoutExpires) {
    Scheduler::instance().init();

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;
    char recv_buf[32] = {0};

    // 接收方设置 10 ticks 超时时间等待发送方
    IpcStatus st = ep.receive(receiver, recv_buf, sizeof(recv_buf), 10);
    EXPECT_EQ(st, IpcStatus::Ok);
    EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);
    EXPECT_EQ(receiver->scheduler.sleep_ticks, 10u);

    // 调度器补偿跳过 10 ticks
    Scheduler::instance().compensate_ticks(10);
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);
    EXPECT_EQ(receiver->ipc.status, IpcStatus::Timeout);
    EXPECT_EQ(receiver->scheduler.sleep_ticks, 0u);
}

// 6. 应答等待超时 (ReplyBlocked Timeout) 测试
TEST(IpcTest, ReplyBlockedTimeoutExpires) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;
    char send_msg[] = "Call with reply timeout";
    char recv_buf[32] = {0};
    char reply_buf[32] = {0};

    // Receiver 先就绪
    ep.receive(receiver, recv_buf, sizeof(recv_buf));

    // Sender 发起调用，设置等待应答超时为 8 ticks
    ep.call(sender, send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf), 8);
    EXPECT_EQ(sender->ipc.state, IpcState::ReplyBlocked);
    EXPECT_EQ(sender->scheduler.sleep_ticks, 8u);

    // Receiver 处理消息超时未回复，8 ticks 过去
    for (int i = 0; i < 8; ++i) {
        Scheduler::instance().tick_update();
    }

    // Sender 自动解除 ReplyBlocked，状态恢复 Ready，状态码为 Timeout
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_EQ(sender->ipc.status, IpcStatus::Timeout);
    EXPECT_EQ(sender->scheduler.sleep_ticks, 0u);

    // 此时 Receiver 若尝试回复，应返回 Invalid / 失败
    char reply_msg[] = "Late reply";
    IpcStatus late_reply_st = Endpoint::reply(receiver, sender->scheduler.id, reply_msg, sizeof(reply_msg));
    EXPECT_EQ(late_reply_st, IpcStatus::Invalid);
}
