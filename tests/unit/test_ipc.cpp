// =============================================================================
// tests/unit/test_ipc.cpp
//
// 统一 IPC 端点消息传递、seL4 Badge 认证、Label 过滤与生命周期安全清理测试
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

// 7. seL4 风格 Capability Badge 身份认证传递测试
TEST(IpcTest, BadgeAuthentication) {
    Scheduler::instance().init();

    uint32_t sender1_stack[128], sender2_stack[128], receiver_stack[128];
    TaskControlBlock* sender1 = Scheduler::instance().create_task([]() {}, sender1_stack, sizeof(sender1_stack));
    TaskControlBlock* sender2 = Scheduler::instance().create_task([]() {}, sender2_stack, sizeof(sender2_stack));
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;
    char msg1[] = "Client 1 Msg";
    char msg2[] = "Client 2 Msg";
    char recv_buf[32] = {0};
    char reply_buf[32] = {0};

    // 1. Receiver 就绪接收
    ep.receive(receiver, recv_buf, sizeof(recv_buf));

    // 2. Sender1 使用 Badge 0x11223344 发起 IPC
    ep.call(sender1, msg1, sizeof(msg1), reply_buf, sizeof(reply_buf), IPC_TIMEOUT_INFINITE, 0x11223344);

    // 验证 Receiver 成功收到真实的 Badge 认证凭证，且不可被客户端篡改
    EXPECT_EQ(receiver->ipc.badge, 0x11223344u);
    EXPECT_EQ(receiver->ipc.sender_id, sender1->scheduler.id);
    EXPECT_STREQ(recv_buf, "Client 1 Msg");

    // 回复 Sender1
    Endpoint::reply(receiver, sender1->scheduler.id, nullptr, 0);

    // 3. Receiver 再次接收
    ep.receive(receiver, recv_buf, sizeof(recv_buf));

    // 4. Sender2 使用 Badge 0x99887766 发起 IPC
    ep.call(sender2, msg2, sizeof(msg2), reply_buf, sizeof(reply_buf), IPC_TIMEOUT_INFINITE, 0x99887766);
    EXPECT_EQ(receiver->ipc.badge, 0x99887766u);
    EXPECT_EQ(receiver->ipc.sender_id, sender2->scheduler.id);
    EXPECT_STREQ(recv_buf, "Client 2 Msg");
}

// 8. 消息 Label / Protocol Type 多接收者选择性匹配测试
TEST(IpcTest, SelectiveReceiveAndMultiReceiver) {
    Scheduler::instance().init();

    uint32_t s1_stk[128], s2_stk[128], w1_stk[128], w2_stk[128];
    TaskControlBlock* sender_sensor = Scheduler::instance().create_task([]() {}, s1_stk, sizeof(s1_stk));
    TaskControlBlock* sender_net    = Scheduler::instance().create_task([]() {}, s2_stk, sizeof(s2_stk));
    TaskControlBlock* worker_sensor = Scheduler::instance().create_task([]() {}, w1_stk, sizeof(w1_stk));
    TaskControlBlock* worker_net    = Scheduler::instance().create_task([]() {}, w2_stk, sizeof(w2_stk));

    Endpoint ep;

    // 构造两种不同 Label (msg_type) 的消息
    constexpr uint32_t LABEL_SENSOR = 100;
    constexpr uint32_t LABEL_NET    = 200;

    struct TestSensorMsg {
        IpcMsgType type{static_cast<IpcMsgType>(LABEL_SENSOR)};
        uint32_t size{sizeof(int)};
        int val{42};
    } sensor_msg;

    struct TestNetMsg {
        IpcMsgType type{static_cast<IpcMsgType>(LABEL_NET)};
        uint32_t size{sizeof(int)};
        int packet_id{1024};
    } net_msg;

    char reply_buf[16] = {0};

    // 两个发送方分别向同一个 Endpoint 排队发送不同类型的消息
    ep.call(sender_sensor, &sensor_msg, sizeof(sensor_msg), reply_buf, sizeof(reply_buf));
    ep.call(sender_net, &net_msg, sizeof(net_msg), reply_buf, sizeof(reply_buf));

    // Worker 1 指定只接收 LABEL_NET 类型的消息
    TestNetMsg rcv_net{};
    IpcStatus st_net = ep.receive(worker_net, &rcv_net, sizeof(rcv_net), IPC_TIMEOUT_INFINITE, LABEL_NET);
    EXPECT_EQ(st_net, IpcStatus::Ok);
    EXPECT_EQ(worker_net->ipc.msg_type, LABEL_NET);
    EXPECT_EQ(rcv_net.packet_id, 1024);
    EXPECT_EQ(worker_net->ipc.sender_id, sender_net->scheduler.id);

    // Worker 2 指定只接收 LABEL_SENSOR 类型的消息
    TestSensorMsg rcv_sensor{};
    IpcStatus st_sensor = ep.receive(worker_sensor, &rcv_sensor, sizeof(rcv_sensor), IPC_TIMEOUT_INFINITE, LABEL_SENSOR);
    EXPECT_EQ(st_sensor, IpcStatus::Ok);
    EXPECT_EQ(worker_sensor->ipc.msg_type, LABEL_SENSOR);
    EXPECT_EQ(rcv_sensor.val, 42);
    EXPECT_EQ(worker_sensor->ipc.sender_id, sender_sensor->scheduler.id);
}

// 9. 端点生命周期与析构安全清理 (Anti-dangling Pointer) 测试
TEST(IpcTest, EndpointDestructionSafeCleanup) {
    Scheduler::instance().init();

    uint32_t sender_stack[128], receiver_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, sender_stack, sizeof(sender_stack));
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, receiver_stack, sizeof(receiver_stack));

    struct Msg1 {
        IpcMsgType type{static_cast<IpcMsgType>(10)};
        uint32_t size{4};
        char data[4]{'a', 'b', 'c', '\0'};
    } send_msg;
    char reply_buf[16] = {0};
    char recv_buf[16] = {0};

    {
        // 局部作用域内的动态 Endpoint
        Endpoint local_ep;

        // sender 和 receiver 分别排队挂起在 local_ep 上 (不同 label，互不匹配，均处于等待队列)
        local_ep.call(sender, &send_msg, sizeof(send_msg), reply_buf, sizeof(reply_buf));
        local_ep.receive(receiver, recv_buf, sizeof(recv_buf), IPC_TIMEOUT_INFINITE, 20);

        EXPECT_EQ(sender->ipc.waiting_endpoint, &local_ep);
        EXPECT_EQ(sender->ipc.state, IpcState::Sending);

        EXPECT_EQ(receiver->ipc.waiting_endpoint, &local_ep);
        EXPECT_EQ(receiver->ipc.state, IpcState::Receiving);

        // local_ep 在离开作用域时自动析构，触发 cancel_all(ReceiverDead)
    }

    // 验证：两个任务已被安全唤醒，waiting_endpoint 指针已清空为 nullptr，状态码置为 ReceiverDead
    EXPECT_EQ(sender->ipc.waiting_endpoint, nullptr);
    EXPECT_EQ(sender->ipc.state, IpcState::Ready);
    EXPECT_EQ(sender->ipc.status, IpcStatus::ReceiverDead);

    EXPECT_EQ(receiver->ipc.waiting_endpoint, nullptr);
    EXPECT_EQ(receiver->ipc.state, IpcState::Ready);
    EXPECT_EQ(receiver->ipc.status, IpcStatus::ReceiverDead);
}

// 10. CSpace 能力撤销 (cap_revoke) 与等待队列安全解链测试
TEST(IpcTest, CSpaceRevocationCancelsWaiters) {
    Scheduler::instance().init();

    uint32_t root_stk[128], task_stk[128];
    TaskControlBlock* root_task = Scheduler::instance().create_task([]() {}, root_stk, sizeof(root_stk));
    TaskControlBlock* client_task = Scheduler::instance().create_task([]() {}, task_stk, sizeof(task_stk));

    Endpoint ep;

    // root_task 在 slot 1 持有 Endpoint
    root_task->security.cspace[1].type = CapType::Endpoint;
    root_task->security.cspace[1].rights = {1, 1, 1, 0};
    root_task->security.cspace[1].object = &ep;

    // client_task 从 root_task 派生能力到 slot 2
    CSpace::cap_derive(root_task, 1, 2, CAP_RIGHT_READ | CAP_RIGHT_WRITE);
    // 派生给 client_task
    CSpace::cap_grant(root_task, client_task, 1, 2, CAP_RIGHT_READ | CAP_RIGHT_WRITE, 0x1234);

    // client_task 挂起在 ep 上等待接收
    char recv_buf[16] = {0};
    ep.receive(client_task, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(client_task->ipc.waiting_endpoint, &ep);
    EXPECT_EQ(client_task->ipc.state, IpcState::Receiving);

    // root_task 撤销 (revoke) 该 Endpoint 能力
    bool revoked = CSpace::cap_revoke(root_task, 1);
    EXPECT_TRUE(revoked);

    // 验证 client_task 的能力槽位已被清空，且挂起的 IPC 自动解除并返回 NoPermission
    EXPECT_EQ(client_task->security.cspace[2].type, CapType::Null);
    EXPECT_EQ(client_task->ipc.waiting_endpoint, nullptr);
    EXPECT_EQ(client_task->ipc.state, IpcState::Ready);
    EXPECT_EQ(client_task->ipc.status, IpcStatus::NoPermission);
}

// 11. WaitQueue 优先级降序插入与出队测试
TEST(IpcTest, WaitQueuePriorityOrdering) {
    Scheduler::instance().init();

    uint32_t stk1[128], stk2[128], stk3[128];
    TaskControlBlock* task_low = Scheduler::instance().create_task([]() {}, stk1, sizeof(stk1), TaskPriority::Low);
    TaskControlBlock* task_norm = Scheduler::instance().create_task([]() {}, stk2, sizeof(stk2), TaskPriority::Normal);
    TaskControlBlock* task_high = Scheduler::instance().create_task([]() {}, stk3, sizeof(stk3), TaskPriority::High);

    ASSERT_NE(task_low, nullptr);
    ASSERT_NE(task_norm, nullptr);
    ASSERT_NE(task_high, nullptr);

    WaitQueue q;
    // 逆序入队：Low -> Normal -> High
    q.enqueue(task_low);
    q.enqueue(task_norm);
    q.enqueue(task_high);

    // 验证出队顺序必须严格按优先级降序：High -> Normal -> Low
    EXPECT_EQ(q.dequeue(), task_high);
    EXPECT_EQ(q.dequeue(), task_norm);
    EXPECT_EQ(q.dequeue(), task_low);
    EXPECT_TRUE(q.empty());
}

// 12. Endpoint 多发送者按优先级服务测试
TEST(IpcTest, EndpointPrioritySortedSenders) {
    Scheduler::instance().init();

    uint32_t s_stk_low[128], s_stk_norm[128], s_stk_high[128], r_stk[128];
    TaskControlBlock* s_low = Scheduler::instance().create_task([]() {}, s_stk_low, sizeof(s_stk_low), TaskPriority::Low);
    TaskControlBlock* s_norm = Scheduler::instance().create_task([]() {}, s_stk_norm, sizeof(s_stk_norm), TaskPriority::Normal);
    TaskControlBlock* s_high = Scheduler::instance().create_task([]() {}, s_stk_high, sizeof(s_stk_high), TaskPriority::High);
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, r_stk, sizeof(r_stk), TaskPriority::Low);

    Endpoint ep;
    char msg_low[] = "LOW";
    char msg_norm[] = "NORM";
    char msg_high[] = "HIGH";
    char reply_buf[16];

    // 三个发送者依次入队等待接收方
    ep.call(s_low, msg_low, sizeof(msg_low), reply_buf, sizeof(reply_buf));
    ep.call(s_norm, msg_norm, sizeof(msg_norm), reply_buf, sizeof(reply_buf));
    ep.call(s_high, msg_high, sizeof(msg_high), reply_buf, sizeof(reply_buf));

    char recv_buf[16];

    // 第一次接收：必须优先拿到 High 发送者的消息
    EXPECT_EQ(ep.receive(receiver, recv_buf, sizeof(recv_buf)), IpcStatus::Ok);
    EXPECT_STREQ(recv_buf, "HIGH");
    EXPECT_EQ(receiver->ipc.sender_id, s_high->scheduler.id);
    Endpoint::reply(receiver, receiver->ipc.sender_id, (void*)"ACK", 4);

    // 第二次接收：必须拿到 Normal 发送者的消息
    EXPECT_EQ(ep.receive(receiver, recv_buf, sizeof(recv_buf)), IpcStatus::Ok);
    EXPECT_STREQ(recv_buf, "NORM");
    EXPECT_EQ(receiver->ipc.sender_id, s_norm->scheduler.id);
    Endpoint::reply(receiver, receiver->ipc.sender_id, (void*)"ACK", 4);

    // 第三次接收：拿到 Low 发送者的消息
    EXPECT_EQ(ep.receive(receiver, recv_buf, sizeof(recv_buf)), IpcStatus::Ok);
    EXPECT_STREQ(recv_buf, "LOW");
    EXPECT_EQ(receiver->ipc.sender_id, s_low->scheduler.id);
    Endpoint::reply(receiver, receiver->ipc.sender_id, (void*)"ACK", 4);
}

// 13. Endpoint 优先级继承协议 (PIP) 与优先级恢复测试
TEST(IpcTest, EndpointPriorityInheritanceProtocol) {
    Scheduler::instance().init();

    uint32_t r_stk[128], s_stk[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([]() {}, r_stk, sizeof(r_stk), TaskPriority::Low);
    TaskControlBlock* sender = Scheduler::instance().create_task([]() {}, s_stk, sizeof(s_stk), TaskPriority::High);

    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(sender, nullptr);

    Endpoint ep;
    char msg[] = "PIP Request";
    char reply_msg[] = "PIP Reply";
    char recv_buf[32];
    char reply_buf[32];

    // 初始状态：receiver 是 Low 优先级
    EXPECT_EQ(receiver->scheduler.current_priority, TaskPriority::Low);
    EXPECT_EQ(sender->scheduler.current_priority, TaskPriority::High);

    // 1. Receiver 进入等待
    EXPECT_EQ(ep.receive(receiver, recv_buf, sizeof(recv_buf)), IpcStatus::Ok);

    // 2. High 优先级 Sender 发送请求
    EXPECT_EQ(ep.call(sender, msg, sizeof(msg), reply_buf, sizeof(reply_buf)), IpcStatus::Ok);

    // 验证 PIP：Receiver 继承了 Sender 的 High 优先级以防止被中间优先级任务打断！
    EXPECT_EQ(receiver->scheduler.current_priority, TaskPriority::High);

    // 3. Receiver 处理完成并应答
    EXPECT_EQ(Endpoint::reply(receiver, receiver->ipc.sender_id, reply_msg, sizeof(reply_msg)), IpcStatus::Ok);

    // 验证 PIP 恢复：Receiver 优先级恢复至原始 Low 优先级
    EXPECT_EQ(receiver->scheduler.current_priority, TaskPriority::Low);
}

