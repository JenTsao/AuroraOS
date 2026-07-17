#ifndef KERNEL_OTA_HPP
#define KERNEL_OTA_HPP

#include <cstdint>
#include <cstddef>

namespace aurora {

class OtaManager {
public:
    OtaManager();

    // Unpack firmware from a file in VFS (LittleFS) and write it to the Staging partition
    // After unpacking, it verifies the Ed25519 signature.
    // If successful, it marks the partition as UPDATE_PENDING.
    bool unpack_from_vfs(const char* filepath);

    // Trigger a system reboot to apply the update
    void reboot();

private:
    // image_size: 固件 payload 大小（不含 128 字节 FirmwareHeader）
    bool verify_signature(uint32_t part_b_offset, uint32_t image_size, const uint8_t* expected_signature);
    bool erase_partition(uint32_t start_addr, uint32_t size);
    bool write_flash_word(uint32_t address, uint32_t data);
};

} // namespace aurora

#endif // KERNEL_OTA_HPP
