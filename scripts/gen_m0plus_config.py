#!/usr/bin/env python3
"""M0+ (Nucleo-L031K6) 配置生成入口 —— 保留 CI/本地既有调用契约的兼容包装。

历史版本在本脚本内硬编码一份 autoconf.h / autoconf.cmake，与 Kconfig 形成
两份配置真相，存在漂移风险（M0+ 的裁剪策略应只在 Kconfig 中表达）。现改为
委托 scripts/genconfig.py --board nucleo_l031k6，从 Kconfig 单一事实源生成，
再把产物按历史契约写入仓库 config/ 目录：

    python scripts/gen_m0plus_config.py     # 无参数；产物 config/autoconf.h / config/autoconf.cmake

构建本身不消费 config/ 下的产物（CMake 每次 configure 都把 autoconf 三件套
生成进构建目录并优先命中），此处输出仅供既有 CI 步骤与本地查阅兼容。
"""

import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    config_dir = os.path.join(repo_root, "config")
    genconfig = os.path.join(repo_root, "scripts", "genconfig.py")

    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            [sys.executable, genconfig, "--board", "nucleo_l031k6", "--out", tmp],
            cwd=repo_root,
        )
        if result.returncode != 0:
            return result.returncode
        os.makedirs(config_dir, exist_ok=True)
        for name in ("autoconf.h", "autoconf.cmake"):
            shutil.copy2(os.path.join(tmp, name), os.path.join(config_dir, name))

    print(f"Generated {os.path.join(config_dir, 'autoconf.h')}")
    print(f"Generated {os.path.join(config_dir, 'autoconf.cmake')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
