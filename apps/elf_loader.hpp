#ifndef ELF_LOADER_HPP
#define ELF_LOADER_HPP

#include "../runtime/app_manifest.hpp"

class ElfLoader {
public:
    // 从 VFS 路径中加载并创建一个全新的动态后台进程
    static bool load_and_exec(const char* filepath);

    // 从 ELF 二进制中提取嵌入的 AppManifest (.aurora_manifest section)
    static bool read_manifest(const char* filepath, auroraos::runtime::AppManifest& out_manifest);

    // 获取上一次加载任务解析出的 Manifest
    static const auroraos::runtime::AppManifest& get_last_manifest();
};

#endif
