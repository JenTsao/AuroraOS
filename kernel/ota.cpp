#include "ota.hpp"
#include "firmware_header.hpp"
#include "../config/partition_table.hpp"
#include "../3rdparty/ed25519/ed25519.h"
#include "posix.hpp"
#include <cstring>
#ifdef AURORA_HOST_TEST
#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#endif

namespace aurora {

extern "C" void sys_print(const char* str);

namespace flash_internal {
    constexpr uint32_t FLASH_CTRL_BASE = 0x400FD000;
    volatile uint32_t* const FMA = reinterpret_cast<uint32_t*>(FLASH_CTRL_BASE + 0x000);
    volatile uint32_t* const FMD = reinterpret_cast<uint32_t*>(FLASH_CTRL_BASE + 0x004);
    volatile uint32_t* const FMC = reinterpret_cast<uint32_t*>(FLASH_CTRL_BASE + 0x008);
    constexpr uint32_t FLASH_KEY = 0x71D50000;

    bool erase_page(uint32_t address) {
        *FMA = address & ~(1024 - 1);
        *FMC = FLASH_KEY | 0x2; 
        uint32_t timeout = 1000000;
        while ((*FMC & 0x2) && --timeout) {} 
        return timeout > 0;
    }

    bool write_word(uint32_t address, uint32_t data) {
        *FMA = address;
        *FMD = data;
        *FMC = FLASH_KEY | 0x1; 
        uint32_t timeout = 1000000;
        while ((*FMC & 0x1) && --timeout) {} 
        return timeout > 0;
    }
}

OtaManager::OtaManager() {}

bool OtaManager::erase_partition(uint32_t start_addr, uint32_t size) {
    for (uint32_t addr = start_addr; addr < start_addr + size; addr += 1024) {
        if (!flash_internal::erase_page(addr)) return false;
    }
    return true;
}

bool OtaManager::write_flash_word(uint32_t address, uint32_t data) {
    return flash_internal::write_word(address, data);
}

// image_size: 固件 payload 大小（不含 128 字节 FirmwareHeader）
bool OtaManager::verify_signature(uint32_t offset, uint32_t image_size, const uint8_t* expected_signature) {
    // CONFIG_OTA_DEV_MODE is a Kconfig option (enabled via "OTA Development Mode").
    // When enabled, a well-known mock public key is used so that any firmware
    // with the matching mock signature (0xED-filled) passes verification.
    // This MUST NOT be enabled for production / user-facing builds.
    //
    // AURORA_HOST_TEST bypasses the production guard so unit tests can compile
    // and exercise the OTA unpack logic without real cryptographic hardware.
#if defined(CONFIG_OTA_DEV_MODE) || defined(AURORA_HOST_TEST)
    #pragma message("WARNING: Using mock OTA ROOT_PUBLIC_KEY. OTA signature verification is DISABLED.")
    const uint8_t ROOT_PUBLIC_KEY[32] = {
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA
    };
#else
    #error "CONFIG_OTA_DEV_MODE is NOT enabled. "
           "You MUST provide a real OTA ROOT_PUBLIC_KEY for production builds. "
           "Either (a) define CONFIG_OTA_DEV_MODE=y in Kconfig for dev/QEMU builds, "
           "or (b) replace this #error with a real key read from eFuse / OTP."
#endif
    
    // Message payload starts after the header
    const uint8_t* message = reinterpret_cast<const uint8_t*>(offset + sizeof(FirmwareHeader));
    return ed25519_verify(expected_signature, message, image_size, ROOT_PUBLIC_KEY) != 0;
}

bool OtaManager::unpack_from_vfs(const char* filepath) {
    int fd = open(filepath, 0);
    if (fd < 0) {
        sys_print("[OTA] Failed to open update package\r\n");
        return false;
    }

    PartitionTable* pt = reinterpret_cast<PartitionTable*>(0x00007000);
    uint32_t part_b_offset = pt->magic == PT_MAGIC ? pt->part_b.offset : PT_PART_B.offset;
    uint32_t part_size     = pt->magic == PT_MAGIC ? pt->part_b.size   : PT_PART_B.size;

    // Read the Header First
    FirmwareHeader header;
    int bytes_read = read(fd, &header, sizeof(FirmwareHeader));
    if (bytes_read != sizeof(FirmwareHeader) || header.magic != FIRMWARE_MAGIC) {
        sys_print("[OTA] Invalid firmware header in package\r\n");
        close(fd);
        return false;
    }

    // 防整数溢出的边界检查 (image_size = payload size, 不含 header)
    if (header.image_size == 0 || header.image_size > part_size - sizeof(FirmwareHeader)) {
        sys_print("[OTA] Image too large for partition\r\n");
        close(fd);
        return false;
    }

    // Erase Staging Partition AFTER validating header magic and size
    if (!erase_partition(part_b_offset, part_size)) {
        sys_print("[OTA] Flash erase failed\r\n");
        close(fd);
        return false;
    }

    // Write body in chunks (skipping header for now)
    uint8_t buffer[256];
    uint32_t current_offset = part_b_offset + sizeof(FirmwareHeader);
    uint32_t remaining = header.image_size;

    while (remaining > 0) {
        int to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        memset(buffer, 0, sizeof(buffer)); // 清零防止最后不满 4 字节时写入脏数据
        bytes_read = read(fd, buffer, to_read);
        if (bytes_read <= 0) break;

        uint32_t* word_buf = reinterpret_cast<uint32_t*>(buffer);
        uint32_t words = (bytes_read + 3) / 4;
        for (size_t i = 0; i < words; ++i) {
            if (!write_flash_word(current_offset + (i * 4), word_buf[i])) {
                sys_print("[OTA] Flash write failed\r\n");
                close(fd);
                erase_partition(part_b_offset, part_size);
                return false;
            }
        }
        
        current_offset += bytes_read;
        remaining -= bytes_read;
    }

    close(fd);

    if (remaining > 0) {
        sys_print("[OTA] EOF reached before full image read\r\n");
        erase_partition(part_b_offset, part_size); // Clear incomplete image
        return false;
    }

    // Verify written payload
    if (!verify_signature(part_b_offset, header.image_size, header.signature)) {
        sys_print("[OTA] Signature verification failed\r\n");
        erase_partition(part_b_offset, part_size); // Clear invalid image
        return false;
    }

    // Finally, write header (status = UPDATE_PENDING) now that image is valid
    header.status = FirmwareStatus::UPDATE_PENDING;
    uint32_t* header_words = reinterpret_cast<uint32_t*>(&header);
    for (size_t i = 0; i < sizeof(FirmwareHeader) / 4; ++i) {
        write_flash_word(part_b_offset + (i * 4), header_words[i]);
    }

    sys_print("[OTA] Unpack successful, rebooting...\r\n");
    reboot();
    return true;
}

void OtaManager::reboot() {
    volatile uint32_t* AIRCR = reinterpret_cast<uint32_t*>(0xE000ED0C);
    *AIRCR = (0x05FA0000 | 0x04); // SYSRESETREQ
    while (true) {} 
}

} // namespace aurora
