"""AuroraOS SDK 打包脚本。

sdk/include 与 sdk/src 是生成产物，源码树（runtime/、syscall/、services/）才是唯一
事实来源。每次打包都会先清空 sdk/include/auroraos 与 sdk/src/auroraos 再重新生成，
避免出现"陈旧快照与源码树漂移"（曾导致 SDK 丢失 sys_cap_mint 的 a3 传参修复等）。

打包内容采用显式清单而非通配扫描，让 SDK 的 API 面对任何变更都可见、可评审。

不打包的文件（依赖仓内 apps/、kernel/、3rdparty/ 内部头，无法独立编译）：
  - runtime/lua_app.hpp、runtime/js_app.hpp   （依赖 apps/ 下的小程序引擎）
  - runtime/js_heap.hpp、runtime/lua_heap.hpp （依赖 kernel/core/arch_api.hpp、3rdparty）
  - services/firewall/firewall_client.cpp     （依赖 kernel/core/syscall_ipc.hpp）
服务端实现（*_service.*、power_manager.*、service_manager.*）属于系统镜像，不面向应用。
"""

import os
import shutil

# 头文件（安装到 sdk/include/auroraos/...）
HEADER_MANIFEST = {
    # 运行时 API
    "runtime/app_base.hpp": "runtime/app_base.hpp",
    "runtime/app_manifest.hpp": "runtime/app_manifest.hpp",
    "runtime/app_sandbox.hpp": "runtime/app_sandbox.hpp",
    "runtime/app_state.hpp": "runtime/app_state.hpp",
    "runtime/aurora_runtime.hpp": "runtime/aurora_runtime.hpp",
    "runtime/ipc_runtime.hpp": "runtime/ipc_runtime.hpp",
    # 系统调用 ABI
    "syscall/syscall.hpp": "syscall/syscall.hpp",
    # 微服务注册中心与各服务 IPC 契约/客户端 API
    "services/service_registry.hpp": "services/service_registry.hpp",
    "services/vfs/vfs_ipc.hpp": "services/vfs/vfs_ipc.hpp",
    "services/vfs/vfs_client.hpp": "services/vfs/vfs_client.hpp",
    "services/power/power_state.hpp": "services/power/power_state.hpp",
    "services/power/power_ipc.hpp": "services/power/power_ipc.hpp",
    "services/power/power_client.hpp": "services/power/power_client.hpp",
    "services/display/display_ipc.hpp": "services/display/display_ipc.hpp",
    "services/display/display_client.hpp": "services/display/display_client.hpp",
    "services/sensor/sensor_ipc.hpp": "services/sensor/sensor_ipc.hpp",
    "services/sensor/sensor_client.hpp": "services/sensor/sensor_client.hpp",
    # 仅有 IPC 契约、暂无客户端封装的服务（应用可经 IpcChannel 直接调用）
    "services/net/net_ipc.hpp": "services/net/net_ipc.hpp",
    "services/firewall/firewall_ipc.hpp": "services/firewall/firewall_ipc.hpp",
}

# 源文件（安装到 sdk/src/auroraos/...）
SOURCE_MANIFEST = {
    "runtime/app_base.cpp": "runtime/app_base.cpp",
    "runtime/app_sandbox.cpp": "runtime/app_sandbox.cpp",
    "runtime/aurora_runtime.cpp": "runtime/aurora_runtime.cpp",
    "services/vfs/vfs_client.cpp": "services/vfs/vfs_client.cpp",
    "services/power/power_client.cpp": "services/power/power_client.cpp",
    "services/display/display_client.cpp": "services/display/display_client.cpp",
    "services/sensor/sensor_client.cpp": "services/sensor/sensor_client.cpp",
}


def install(manifest, root_dir, dest_dir):
    for src_rel, dest_rel in manifest.items():
        src = os.path.join(root_dir, src_rel)
        if not os.path.isfile(src):
            raise FileNotFoundError(f"打包清单中的文件不存在: {src}")
        dest = os.path.join(dest_dir, dest_rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(src, dest)


def main():
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    sdk_dir = os.path.join(root_dir, "sdk")
    include_dir = os.path.join(sdk_dir, "include", "auroraos")
    src_dir = os.path.join(sdk_dir, "src", "auroraos")

    print("Packaging Aurora SDK...")

    # 先清空生成产物目录，杜绝陈旧文件残留
    for generated in (include_dir, src_dir):
        if os.path.isdir(generated):
            shutil.rmtree(generated)
        os.makedirs(generated)

    print(f"Installing {len(HEADER_MANIFEST)} headers...")
    install(HEADER_MANIFEST, root_dir, include_dir)

    print(f"Installing {len(SOURCE_MANIFEST)} sources...")
    install(SOURCE_MANIFEST, root_dir, src_dir)

    print("Aurora SDK packaged successfully at:", sdk_dir)


if __name__ == "__main__":
    main()
