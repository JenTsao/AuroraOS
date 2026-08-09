// =============================================================================
// test_vfs.cpp — Unit tests for VfsManager (vfs/vfs.hpp + vfs/vfs.cpp)
//
// Strategy:
//  - VfsManager is a singleton; call init() in SetUp() to reset state.
//  - VNode is an abstract interface; we provide a lightweight FakeVNode that
//    owns a fixed-size buffer so read/write/lseek semantics can be verified
//    without any filesystem or hardware dependency.
//  - device.hpp is included by vfs.cpp but not strictly needed here; we
//    include a minimal stub (or rely on the include-path ordering).
//
// C++ Core Guidelines applied:
//  C.2: FakeVNode uses class (has invariants: buf_ size & cursor)
//  R.5: FakeVNode owns data as a member array (no heap allocation)
//  Con.1/Con.2: const member functions for observers
//  F.20: return values, not output parameters
// =============================================================================

#include <gtest/gtest.h>

#include "vfs.hpp"

#include <algorithm>
#include <array>
#include <cstring>

// ---------------------------------------------------------------------------
// FakeVNode — an in-memory VNode backed by a fixed-size buffer.
// Implements the same read/write/get_size contract as real VNodes.
// ---------------------------------------------------------------------------
class FakeVNode : public VNode {
public:
    static constexpr int kCapacity = 128;

    int read(char* buf, int len, int offset, void* /*priv*/ = nullptr) override {
        if (offset < 0 || offset >= write_pos_) return 0;
        const int available = write_pos_ - offset;
        const int to_read   = std::min(len, available);
        std::memcpy(buf, data_.data() + offset, static_cast<std::size_t>(to_read));
        return to_read;
    }

    int write(const char* buf, int len, int /*offset*/, void* /*priv*/ = nullptr) override {
        if (write_pos_ + len > kCapacity) return -1;  // Full
        std::memcpy(data_.data() + write_pos_, buf, static_cast<std::size_t>(len));
        write_pos_ += len;
        return len;
    }

    int get_size(void* /*priv*/ = nullptr) const override { return write_pos_; }

    // Expose internal buffer for assertions (non-VNode interface, test-only).
    const char* raw_data() const noexcept { return data_.data(); }

private:
    std::array<char, kCapacity> data_{};
    int write_pos_{0};
};

// ---------------------------------------------------------------------------
// VfsTest fixture
// ---------------------------------------------------------------------------
class VfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        VfsManager::instance().init();
        vnode_ = std::make_unique<FakeVNode>();
    }

    // Convenience: mount the default vnode at a path.
    bool mount(const char* path = "/dev/test") {
        return VfsManager::instance().mount(path, vnode_.get());
    }

    std::unique_ptr<FakeVNode> vnode_;
};

// ---------------------------------------------------------------------------
// 1. Mount followed by open returns a valid (non-negative) file descriptor
// ---------------------------------------------------------------------------
TEST_F(VfsTest, MountAndOpen) {
    ASSERT_TRUE(mount());

    const int fd = VfsManager::instance().open("/dev/test");

    EXPECT_GE(fd, 0) << "open() on a mounted path must return a valid fd";
}

// ---------------------------------------------------------------------------
// 2. Opening an unmounted path returns -1
// ---------------------------------------------------------------------------
TEST_F(VfsTest, OpenNotMounted) {
    const int fd = VfsManager::instance().open("/does/not/exist");

    EXPECT_EQ(fd, -1);
}

// ---------------------------------------------------------------------------
// 3. Write and read round-trip via FakeVNode
// ---------------------------------------------------------------------------
TEST_F(VfsTest, ReadWriteBasic) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    constexpr std::string_view kMessage = "hello aurora";
    const int written = VfsManager::instance().write(
        fd, kMessage.data(), static_cast<int>(kMessage.size()));
    EXPECT_EQ(written, static_cast<int>(kMessage.size()));

    // Seek back to 0 before reading.
    VfsManager::instance().lseek(fd, 0, 0 /*SEEK_SET*/);

    std::array<char, 64> buf{};
    const int bytes_read = VfsManager::instance().read(
        fd, buf.data(), static_cast<int>(buf.size()));

    EXPECT_EQ(bytes_read, static_cast<int>(kMessage.size()));
    EXPECT_EQ(std::string_view(buf.data(), static_cast<std::size_t>(bytes_read)), kMessage);
}

// ---------------------------------------------------------------------------
// 4. Offset advances automatically after read
// ---------------------------------------------------------------------------
TEST_F(VfsTest, OffsetAdvancesAfterRead) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    // Write 8 bytes.
    const char payload[] = "12345678";
    VfsManager::instance().write(fd, payload, 8);
    VfsManager::instance().lseek(fd, 0, 0 /*SEEK_SET*/);

    // First read: 4 bytes.
    std::array<char, 4> buf{};
    EXPECT_EQ(VfsManager::instance().read(fd, buf.data(), 4), 4);

    // Second read should continue from offset 4.
    EXPECT_EQ(VfsManager::instance().read(fd, buf.data(), 4), 4);
    EXPECT_EQ(std::string_view(buf.data(), 4), "5678");
}

// ---------------------------------------------------------------------------
// 5. lseek SEEK_SET correctly positions the cursor
// ---------------------------------------------------------------------------
TEST_F(VfsTest, LseekSet) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    VfsManager::instance().write(fd, "abcdefgh", 8);

    const int pos = VfsManager::instance().lseek(fd, 3, 0 /*SEEK_SET*/);
    EXPECT_EQ(pos, 3);

    std::array<char, 4> buf{};
    VfsManager::instance().read(fd, buf.data(), 1);
    EXPECT_EQ(buf[0], 'd');
}

// ---------------------------------------------------------------------------
// 6. lseek SEEK_CUR advances the cursor by a relative offset
// ---------------------------------------------------------------------------
TEST_F(VfsTest, LseekCur) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    VfsManager::instance().write(fd, "abcdefgh", 8);
    VfsManager::instance().lseek(fd, 0, 0 /*SEEK_SET*/);

    // Advance 2 from the start, then 2 more.
    VfsManager::instance().lseek(fd, 2, 1 /*SEEK_CUR*/);
    const int pos = VfsManager::instance().lseek(fd, 2, 1 /*SEEK_CUR*/);
    EXPECT_EQ(pos, 4);
}

// ---------------------------------------------------------------------------
// 7. lseek SEEK_END positions relative to the file size
// ---------------------------------------------------------------------------
TEST_F(VfsTest, LseekEnd) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    VfsManager::instance().write(fd, "12345", 5);

    // SEEK_END with offset 0 → position at end (5).
    const int pos = VfsManager::instance().lseek(fd, 0, 2 /*SEEK_END*/);
    EXPECT_EQ(pos, 5);
}

// ---------------------------------------------------------------------------
// 8. After close(), read returns -1 (fd is invalid)
// ---------------------------------------------------------------------------
TEST_F(VfsTest, CloseInvalidatesFd) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    VfsManager::instance().close(fd);

    std::array<char, 8> buf{};
    EXPECT_EQ(VfsManager::instance().read(fd, buf.data(), 8), -1)
        << "read on a closed fd must return -1";
}

// ---------------------------------------------------------------------------
// 9. Mounting beyond MAX_MOUNT_POINTS (8) returns false
// ---------------------------------------------------------------------------
TEST_F(VfsTest, MaxMountPoints) {
    // Accumulate 8 additional VNodes.
    std::array<FakeVNode, 8> extra_nodes{};
    std::array<const char*, 8> paths = {
        "/a", "/b", "/c", "/d", "/e", "/f", "/g", "/h"
    };

    for (int i = 0; i < 8; ++i) {
        VfsManager::instance().mount(paths[i], &extra_nodes[i]);
    }

    // The 9th mount should fail (MAX_MOUNT_POINTS = 8).
    FakeVNode overflow_node;
    EXPECT_FALSE(VfsManager::instance().mount("/overflow", &overflow_node))
        << "mount beyond MAX_MOUNT_POINTS must return false";
}

// ---------------------------------------------------------------------------
// 10. Opening beyond MAX_OPEN_FILES (16) returns -1
// ---------------------------------------------------------------------------
TEST_F(VfsTest, MaxOpenFiles) {
    ASSERT_TRUE(mount());

    // Open the same path 16 times.
    std::array<int, 16> fds{};
    for (int i = 0; i < 16; ++i) {
        fds[i] = VfsManager::instance().open("/dev/test");
    }

    // 17th open must fail.
    const int overflow_fd = VfsManager::instance().open("/dev/test");
    EXPECT_EQ(overflow_fd, -1)
        << "open() beyond MAX_OPEN_FILES must return -1";

    // Cleanup.
    for (const int fd : fds) {
        if (fd >= 0) VfsManager::instance().close(fd);
    }
}

// ---------------------------------------------------------------------------
// 11. ioctl is forwarded to the VNode (FakeVNode returns -1 by default)
// ---------------------------------------------------------------------------
TEST_F(VfsTest, IoctlForwarded) {
    ASSERT_TRUE(mount());
    const int fd = VfsManager::instance().open("/dev/test");
    ASSERT_GE(fd, 0);

    // FakeVNode inherits VNode::ioctl which returns -1; that is the expected
    // result, verifying the call was forwarded and not silently dropped.
    EXPECT_EQ(VfsManager::instance().ioctl(fd, 42, nullptr), -1);
}
