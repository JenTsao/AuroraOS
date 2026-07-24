#include <gtest/gtest.h>
#include "../../kernel/task.hpp"
#include "../../kernel/ipc.hpp"
#include "../../kernel/cspace.hpp"

using namespace auroraos::kernel;

class CSpaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
    }

    TaskControlBlock* create_task() {
        uint32_t stack[256];
        return Scheduler::instance().create_task([](){}, stack, sizeof(stack));
    }
};

// --- cap_lookup ---

TEST_F(CSpaceTest, LookupValidSlot) {
    TaskControlBlock* t = create_task();
    ASSERT_NE(t, nullptr);

    // Manually insert an endpoint capability
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, true, false, 0};
    t->cspace[0].badge = 42;
    t->cspace[0].object = &ep;

    Capability* found = CSpace::cap_lookup(t, 0);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->type, CapType::Endpoint);
    EXPECT_EQ(found->badge, 42u);
    EXPECT_TRUE(found->rights.read);
    EXPECT_TRUE(found->rights.write);
    EXPECT_FALSE(found->rights.grant);
}

TEST_F(CSpaceTest, LookupInvalidSlot) {
    TaskControlBlock* t = create_task();
    EXPECT_EQ(CSpace::cap_lookup(t, MAX_CSPACE_SLOTS), nullptr);
    EXPECT_EQ(CSpace::cap_lookup(t, 99), nullptr);
}

TEST_F(CSpaceTest, LookupNullSlot) {
    TaskControlBlock* t = create_task();
    EXPECT_EQ(CSpace::cap_lookup(t, 0), nullptr);
}

TEST_F(CSpaceTest, LookupNullTask) {
    EXPECT_EQ(CSpace::cap_lookup(nullptr, 0), nullptr);
}

// --- cap_delete ---

TEST_F(CSpaceTest, DeleteSlot) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[3].type = CapType::Endpoint;
    t->cspace[3].object = &ep;

    EXPECT_TRUE(CSpace::cap_delete(t, 3));
    EXPECT_EQ(t->cspace[3].type, CapType::Null);
    EXPECT_EQ(t->cspace[3].object, nullptr);
}

TEST_F(CSpaceTest, DeleteInvalidSlot) {
    TaskControlBlock* t = create_task();
    EXPECT_FALSE(CSpace::cap_delete(t, MAX_CSPACE_SLOTS));
}

// --- cap_derive ---

TEST_F(CSpaceTest, DeriveBasic) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, true, true, 0};
    t->cspace[0].badge = 7;
    t->cspace[0].object = &ep;

    // Derive with same rights
    EXPECT_TRUE(CSpace::cap_derive(t, 0, 1, CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_GRANT));

    Capability* derived = CSpace::cap_lookup(t, 1);
    ASSERT_NE(derived, nullptr);
    EXPECT_EQ(derived->type, CapType::Endpoint);
    EXPECT_EQ(derived->badge, 7u); // inherited
    EXPECT_TRUE(derived->rights.read);
    EXPECT_TRUE(derived->rights.write);
    EXPECT_TRUE(derived->rights.grant);
    EXPECT_EQ(derived->object, &ep);
}

TEST_F(CSpaceTest, DeriveRightsDowngrade) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, true, true, 0};
    t->cspace[0].object = &ep;

    // Derive with only read right (downgrade)
    EXPECT_TRUE(CSpace::cap_derive(t, 0, 1, CAP_RIGHT_READ));

    Capability* derived = CSpace::cap_lookup(t, 1);
    ASSERT_NE(derived, nullptr);
    EXPECT_TRUE(derived->rights.read);
    EXPECT_FALSE(derived->rights.write);
    EXPECT_FALSE(derived->rights.grant);
}

TEST_F(CSpaceTest, DeriveRightsEscalation) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, false, false, 0};
    t->cspace[0].object = &ep;

    // Try to escalate: request write when source doesn't have it
    EXPECT_FALSE(CSpace::cap_derive(t, 0, 1, CAP_RIGHT_READ | CAP_RIGHT_WRITE));

    // Destination should remain null
    EXPECT_EQ(t->cspace[1].type, CapType::Null);
}

TEST_F(CSpaceTest, DeriveFromNullSlot) {
    TaskControlBlock* t = create_task();
    EXPECT_FALSE(CSpace::cap_derive(t, 0, 1, CAP_RIGHT_READ));
}

TEST_F(CSpaceTest, DeriveInheritsBadge) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, true, false, 0};
    t->cspace[0].badge = 99;
    t->cspace[0].object = &ep;

    EXPECT_TRUE(CSpace::cap_derive(t, 0, 2, CAP_RIGHT_READ));
    Capability* derived = CSpace::cap_lookup(t, 2);
    ASSERT_NE(derived, nullptr);
    EXPECT_EQ(derived->badge, 99u);
}

// --- cap_mint ---

TEST_F(CSpaceTest, MintBasic) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, true, false, 0};
    t->cspace[0].badge = 7;
    t->cspace[0].object = &ep;

    // Mint with new badge
    EXPECT_TRUE(CSpace::cap_mint(t, 0, 1, CAP_RIGHT_READ | CAP_RIGHT_WRITE, 123));

    Capability* minted = CSpace::cap_lookup(t, 1);
    ASSERT_NE(minted, nullptr);
    EXPECT_EQ(minted->badge, 123u); // new badge, not inherited
    EXPECT_TRUE(minted->rights.read);
    EXPECT_TRUE(minted->rights.write);
}

TEST_F(CSpaceTest, MintRightsEscalation) {
    TaskControlBlock* t = create_task();
    Endpoint ep;
    t->cspace[0].type = CapType::Endpoint;
    t->cspace[0].rights = {true, false, false, 0};
    t->cspace[0].object = &ep;

    // Try to escalate: request grant when source doesn't have it
    EXPECT_FALSE(CSpace::cap_mint(t, 0, 1, CAP_RIGHT_READ | CAP_RIGHT_GRANT, 50));
    EXPECT_EQ(t->cspace[1].type, CapType::Null);
}

// --- cap_revoke ---

TEST_F(CSpaceTest, RevokeSingleTask) {
    TaskControlBlock* t1 = create_task();
    TaskControlBlock* t2 = create_task();
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);

    Endpoint ep;
    // Both tasks have a cap to the same endpoint
    t1->cspace[0].type = CapType::Endpoint;
    t1->cspace[0].object = &ep;
    t1->cspace[0].rights = {true, true, false, 0};

    t2->cspace[0].type = CapType::Endpoint;
    t2->cspace[0].object = &ep;
    t2->cspace[0].rights = {true, true, false, 0};

    // Revoke from t1 — should nullify t2's cap but keep t1's
    EXPECT_TRUE(CSpace::cap_revoke(t1, 0));

    EXPECT_NE(t1->cspace[0].type, CapType::Null); // source preserved
    EXPECT_EQ(t2->cspace[0].type, CapType::Null); // other task revoked
}

TEST_F(CSpaceTest, RevokeNullCap) {
    TaskControlBlock* t = create_task();
    // Revoking a null slot should be a no-op (return false)
    EXPECT_FALSE(CSpace::cap_revoke(t, 0));
}

TEST_F(CSpaceTest, RevokeInvalidSlot) {
    TaskControlBlock* t = create_task();
    EXPECT_FALSE(CSpace::cap_revoke(t, MAX_CSPACE_SLOTS));
}

// --- cap_grant ---

TEST_F(CSpaceTest, GrantBasic) {
    TaskControlBlock* t1 = create_task();
    TaskControlBlock* t2 = create_task();
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);

    Endpoint ep;
    t1->cspace[0].type = CapType::Endpoint;
    t1->cspace[0].rights = {true, true, true, 0};
    t1->cspace[0].badge = 10;
    t1->cspace[0].object = &ep;

    // Grant to t2 with reduced rights
    EXPECT_TRUE(CSpace::cap_grant(t1, t2, 0, 3, CAP_RIGHT_READ, 55));

    Capability* granted = CSpace::cap_lookup(t2, 3);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->type, CapType::Endpoint);
    EXPECT_EQ(granted->badge, 55u);
    EXPECT_TRUE(granted->rights.read);
    EXPECT_FALSE(granted->rights.write);
    EXPECT_EQ(granted->object, &ep);
}

TEST_F(CSpaceTest, GrantEscalation) {
    TaskControlBlock* t1 = create_task();
    TaskControlBlock* t2 = create_task();

    Endpoint ep;
    t1->cspace[0].type = CapType::Endpoint;
    t1->cspace[0].rights = {true, false, false, 0};
    t1->cspace[0].object = &ep;

    // Try to grant with write right that source doesn't have
    EXPECT_FALSE(CSpace::cap_grant(t1, t2, 0, 0, CAP_RIGHT_READ | CAP_RIGHT_WRITE, 1));
    EXPECT_EQ(t2->cspace[0].type, CapType::Null);
}

TEST_F(CSpaceTest, GrantFromNullSlot) {
    TaskControlBlock* t1 = create_task();
    TaskControlBlock* t2 = create_task();
    EXPECT_FALSE(CSpace::cap_grant(t1, t2, 0, 0, CAP_RIGHT_READ, 1));
}

// --- is_valid_slot ---

TEST_F(CSpaceTest, IsValidSlot) {
    EXPECT_TRUE(CSpace::is_valid_slot(0));
    EXPECT_TRUE(CSpace::is_valid_slot(15));
    EXPECT_FALSE(CSpace::is_valid_slot(16));
    EXPECT_FALSE(CSpace::is_valid_slot(99));
}
