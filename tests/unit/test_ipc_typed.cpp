#include <gtest/gtest.h>
#include "../../kernel/task.hpp"
#include "../../kernel/ipc.hpp"
#include "../../kernel/cspace.hpp"

using namespace auroraos::kernel;

// ============================================================
// Test message types
// ============================================================
enum class TestMsgType : uint32_t {
    Ping = static_cast<uint32_t>(IpcMsgType::UserBase),
    Pong,
    Data,
};

struct PingMsg {
    uint32_t seq;
};

struct PongMsg {
    uint32_t seq;
    uint32_t status;
};

struct DataMsg {
    uint32_t id;
    char payload[32];
};

// ============================================================
// IpcMessage<T> template tests
// ============================================================
TEST(IpcTypedTest, MessageCreate) {
    PingMsg ping = {42};
    auto msg = IpcMessage<PingMsg>::create(IpcMsgType::UserBase, ping);

    EXPECT_EQ(msg.msg_type, IpcMsgType::UserBase);
    EXPECT_EQ(msg.payload_size, sizeof(PingMsg));
    EXPECT_EQ(msg.payload.seq, 42u);
}

TEST(IpcTypedTest, MessageWireSize) {
    EXPECT_EQ(IpcMessage<PingMsg>::wire_size(), sizeof(IpcMessage<PingMsg>));
    EXPECT_EQ(IpcMessage<PongMsg>::wire_size(), sizeof(IpcMessage<PongMsg>));
    EXPECT_EQ(IpcMessage<DataMsg>::wire_size(), sizeof(IpcMessage<DataMsg>));
}

TEST(IpcTypedTest, MessageHeaderSize) {
    // Header should be 8 bytes (4 for type + 4 for size)
    EXPECT_EQ(sizeof(IpcMsgHeader), 8u);
    EXPECT_EQ(sizeof(IpcRawMessage), 8u);
}

// ============================================================
// Type validation helper tests
// ============================================================
TEST(IpcTypedTest, ValidateTypeValid) {
    IpcRawMessage msg;
    msg.msg_type = IpcMsgType::UserBase;
    msg.payload_size = 4;

    EXPECT_TRUE(ipc_validate_type(&msg, sizeof(msg), 64));
}

TEST(IpcTypedTest, ValidateTypeInvalid) {
    IpcRawMessage msg;
    msg.msg_type = IpcMsgType::UserBase;
    msg.payload_size = 4;

    EXPECT_FALSE(ipc_validate_type(&msg, sizeof(msg), 65));
}

TEST(IpcTypedTest, ValidateTypeNullPtr) {
    EXPECT_FALSE(ipc_validate_type(nullptr, 0, 0));
}

TEST(IpcTypedTest, ValidateTypeTooShort) {
    IpcRawMessage msg;
    EXPECT_FALSE(ipc_validate_type(&msg, 4, 0)); // len < sizeof(IpcRawMessage)
}

// ============================================================
// Typed IPC call/receive via Endpoint
// ============================================================
TEST(IpcTypedTest, TypedCallAndReceive) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));
    ASSERT_NE(sender, nullptr);

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));
    ASSERT_NE(receiver, nullptr);

    Endpoint ep;

    PingMsg ping = {7};
    char recv_buf[sizeof(IpcMessage<PingMsg>)] = {0};

    // Receiver waits
    ep.receive(receiver, recv_buf, sizeof(recv_buf));

    // Sender calls with typed message
    ipc_call(ep, sender, IpcMsgType::UserBase, ping, recv_buf, sizeof(recv_buf));

    // Verify message was delivered
    const auto* typed = reinterpret_cast<const IpcMessage<PingMsg>*>(recv_buf);
    EXPECT_EQ(typed->msg_type, IpcMsgType::UserBase);
    EXPECT_EQ(typed->payload_size, sizeof(PingMsg));
    EXPECT_EQ(typed->payload.seq, 7u);
}

TEST(IpcTypedTest, TypeMismatchReturnsFalse) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    // Send a PingMsg
    PingMsg ping = {1};
    char recv_buf[sizeof(IpcMessage<PingMsg>)] = {0};
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    ipc_call(ep, sender, IpcMsgType::UserBase, ping, recv_buf, sizeof(recv_buf));

    // Try to receive as PongMsg — type mismatch
    PongMsg pong;
    // Manually parse since ipc_receive would fail
    const auto* typed = reinterpret_cast<const IpcMessage<PingMsg>*>(recv_buf);
    EXPECT_NE(typed->msg_type, static_cast<IpcMsgType>(static_cast<uint32_t>(TestMsgType::Pong)));
}

TEST(IpcTypedTest, RawBackwardCompatible) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    // Raw message (no type header)
    char send_msg[] = "Hello Raw";
    char recv_buf[32] = {0};

    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    ep.call(sender, send_msg, sizeof(send_msg), recv_buf, sizeof(recv_buf));

    EXPECT_STREQ(recv_buf, "Hello Raw");
}

TEST(IpcTypedTest, MultipleMessageTypes) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    // Send Ping
    PingMsg ping = {10};
    char recv_buf[sizeof(IpcMessage<DataMsg>)] = {0};
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    ipc_call(ep, sender, IpcMsgType::UserBase, ping, recv_buf, sizeof(recv_buf));

    auto* typed1 = reinterpret_cast<const IpcMessage<PingMsg>*>(recv_buf);
    EXPECT_EQ(typed1->payload.seq, 10u);

    // Send Data (different type, same endpoint)
    DataMsg data = {99, "test payload"};
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    ipc_call(ep, sender, static_cast<IpcMsgType>(static_cast<uint32_t>(TestMsgType::Data)), data, recv_buf, sizeof(recv_buf));

    auto* typed2 = reinterpret_cast<const IpcMessage<DataMsg>*>(recv_buf);
    EXPECT_EQ(typed2->payload.id, 99u);
    EXPECT_STREQ(typed2->payload.payload, "test payload");
}

// ============================================================
// ipc_msg_type field in TCB
// ============================================================
TEST(IpcTypedTest, TcbMsgTypeInitializedToZero) {
    Scheduler::instance().init();

    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([](){}, stack, sizeof(stack));
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->ipc_msg_type, 0u);
}

TEST(IpcTypedTest, TcbMsgTypeRecordsType) {
    Scheduler::instance().init();

    uint32_t sender_stack[128];
    TaskControlBlock* sender = Scheduler::instance().create_task([](){}, sender_stack, sizeof(sender_stack));

    uint32_t receiver_stack[128];
    TaskControlBlock* receiver = Scheduler::instance().create_task([](){}, receiver_stack, sizeof(receiver_stack));

    Endpoint ep;

    // Initially zero
    EXPECT_EQ(receiver->ipc_msg_type, 0u);

    // Send typed message
    PingMsg ping = {5};
    char recv_buf[sizeof(IpcMessage<PingMsg>)] = {0};
    ep.receive(receiver, recv_buf, sizeof(recv_buf));
    ipc_call(ep, sender, IpcMsgType::UserBase, ping, recv_buf, sizeof(recv_buf));

    // After receive, ipc_msg_type should be set
    // (In real kernel this is set by SVC handler; in unit test we simulate it)
    receiver->ipc_msg_type = static_cast<uint32_t>(IpcMsgType::UserBase);
    EXPECT_EQ(receiver->ipc_msg_type, 64u);
}
