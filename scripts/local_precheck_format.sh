#!/usr/bin/env bash
# =============================================================================
# scripts/local_precheck_format.sh
#
# 本地复刻 CI 的 clang-format 增量门禁。
# 对应 job: .github/workflows/build.yml -> "clang-format" (BLOCKING)
#
# 为什么需要这个脚本
# ------------------
# CI 跑在 ubuntu-24.04 上，`apt install clang-format` 装的是 **LLVM 18.x**。
# 开发机 PATH 上很可能是别的版本（本机实测是 22.1.8）。不同大版本对同一段
# 代码的续行 / 折行判定不同，用 22 预检会**误判**：
#   - 假阳性：报一堆 CI 不会报的差异 -> 白改
#   - 假阴性：本地"通过"，推上去 CI 挂
# 实测：本机 v22 对仓库抽样 120 个生产文件报 73 个不合规；仓库里大量历史代码
# 本来就不整体符合 .clang-format（CI 只查**改动行**，所以一直是绿的）。
#
# 本脚本强制使用 18.x；若不是 18.x 直接报错退出，防止后人用错的版本自欺。
#
# 用法
# ----
#   scripts/local_precheck_format.sh                  # 检查「工作区(含未跟踪新文件) vs HEAD」
#   scripts/local_precheck_format.sh --commit <rev>   # 检查某个提交（复刻 CI 单提交门禁）
#   scripts/local_precheck_format.sh --fix [--commit <rev>]
#                                                     # 同上，但把改动行就地格式化
#   CLANG_FORMAT=/path/to/18/clang-format scripts/local_precheck_format.sh   # 手动指定
#
# 退出码：0 = 通过（或无事可做）；1 = 有改动行不合规；2 = 环境问题（版本不对等）
# =============================================================================
set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "ERROR: 当前目录不在 git 仓库内" >&2
    exit 2
}
cd "$REPO_ROOT" || exit 2

# ----------------------------------------------------------------------------
# 1. 定位 clang-format 18.x，并把它所在目录前置到 PATH
#    ⚠️ 必须让 git-clang-format 与 clang-format **同版本成对**：
#       新版 git-clang-format 会调用 `clang-format -list-ignored`，
#       而 18.x 的 clang-format 不认这个参数 -> 直接报错。
#       pip 的 clang-format wheel 会在同目录同时装 clang-format 和
#       git-clang-format，所以前置目录即可保证成对。
# ----------------------------------------------------------------------------
cf_is_v18() {   # $1 = 可执行路径
    [ -x "$1" ] || [ -f "$1" ] || return 1
    "$1" --version 2>/dev/null | grep -Eq 'version 18\.'
}

resolve_cf18() {
    local cand dir

    if [ -n "${CLANG_FORMAT:-}" ]; then
        if cf_is_v18 "$CLANG_FORMAT"; then echo "$CLANG_FORMAT"; return 0; fi
        echo "ERROR: \$CLANG_FORMAT=$CLANG_FORMAT 不是 18.x" >&2
        return 1
    fi

    # pip wheel 自带二进制的常见落点（不需要 LLVM 本体）
    for cand in \
        "$HOME/.workbuddy/binaries/python/versions/"*/Scripts/clang-format.exe \
        "$HOME/.workbuddy/binaries/python/versions/"*/Scripts/clang-format \
        "$HOME/AppData/Roaming/Python/Python"*/Scripts/clang-format.exe \
        "$HOME/.local/bin/clang-format" \
        /usr/lib/llvm-18/bin/clang-format \
        /usr/bin/clang-format-18 \
        /opt/homebrew/opt/llvm@18/bin/clang-format; do
        if cf_is_v18 "$cand"; then echo "$cand"; return 0; fi
    done

    # 最后才看 PATH
    dir="$(command -v clang-format 2>/dev/null || true)"
    if [ -n "$dir" ] && cf_is_v18 "$dir"; then echo "$dir"; return 0; fi

    return 1
}

CF="$(resolve_cf18)" || {
    echo "" >&2
    echo "══════════════════════════════════════════════════════════════════" >&2
    echo " ERROR: 没找到 clang-format 18.x —— CI 用的是 18.x，不能用别的版本预检" >&2
    echo "══════════════════════════════════════════════════════════════════" >&2
    echo " 本机 PATH 上的版本：$(command -v clang-format >/dev/null 2>&1 && clang-format --version || echo '未安装')" >&2
    echo "" >&2
    echo " 安装方式（推荐，wheel 自带二进制，不需要装 LLVM 本体）：" >&2
    echo "   python -m pip install \"clang-format==18.1.8\"" >&2
    echo "   然后确认 PATH 拿到的是 18：" >&2
    echo "   python -c \"import shutil;print(shutil.which('clang-format'))\"" >&2
    echo "" >&2
    echo " 备选：npm i -g clang-format@18" >&2
    echo " 备选：下载 LLVM 18 Windows release zip，用其中的 clang-format.exe" >&2
    echo "       并 export CLANG_FORMAT=/path/to/clang-format.exe" >&2
    echo "══════════════════════════════════════════════════════════════════" >&2
    exit 2
}

CF_DIR="$(dirname "$CF")"
export PATH="$CF_DIR:$PATH"

if ! command -v git-clang-format >/dev/null 2>&1; then
    echo "ERROR: 找不到 git-clang-format（与 clang-format 同目录应有）" >&2
    echo "       已尝试前置 PATH: $CF_DIR" >&2
    exit 2
fi

echo "clang-format      : $CF  ($("$CF" --version 2>/dev/null))"
echo "git-clang-format  : $(command -v git-clang-format)"

# ----------------------------------------------------------------------------
# 2. 解析参数
# ----------------------------------------------------------------------------
MODE="worktree"        # worktree | commit
BASE=""
FIX=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fix)     FIX=1 ;;
        --commit)  MODE="commit"; BASE="${2:-}"; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *)         echo "ERROR: 未知参数 '$1'（见 --help）" >&2; exit 2 ;;
    esac
    shift
done

# ----------------------------------------------------------------------------
# 3. 复刻 CI 的文件筛选（与 build.yml clang-format job 完全同构）
#    ext:  \.(c|cc|cpp|cxx|h|hpp)$
#    excl: ^(3rdparty|tests)/
# ----------------------------------------------------------------------------
FILTER_EXT='\.(c|cc|cpp|cxx|h|hpp)$'
FILTER_EXCL='^(3rdparty|tests)/'

mapfile -t FILES < <(
    if [ "$MODE" = "commit" ]; then
        git diff --name-only --diff-filter=ACMR "${BASE}^" "${BASE}" 2>/dev/null
    else
        { git diff --name-only --diff-filter=ACMR HEAD 2>/dev/null
          git ls-files --others --exclude-standard 2>/dev/null; } | sort -u
    fi | grep -E "$FILTER_EXT" | grep -vE "$FILTER_EXCL" || true
)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "→ 没有生产 C/C++ 文件改动；clang-format 门禁会跳过。PASS"
    exit 0
fi

echo "→ 待检查文件（${#FILES[@]} 个）："
printf '   %s\n' "${FILES[@]}"

EXCLUDED=()
for f in "${FILES[@]}"; do
    [ -f "$f" ] || EXCLUDED+=("$f")
done
if [ "${#EXCLUDED[@]}" -gt 0 ]; then
    echo "   (跳过已删除的文件：${EXCLUDED[*]})"
    TMP=()
    for f in "${FILES[@]}"; do
        [ -f "$f" ] && TMP+=("$f")
    done
    FILES=("${TMP[@]}")
    [ "${#FILES[@]}" -eq 0 ] && { echo "→ 没有可检查的文件。PASS"; exit 0; }
fi

# ----------------------------------------------------------------------------
# 4. 执行
#    CI 原命令：git clang-format --diff <base> <files...>
#    （base = HEAD^1；只检查「改动行」，未改动的历史不合规不会误报）
# ----------------------------------------------------------------------------
TARGET="${BASE}^"        # commit 模式：与 CI 单提交门禁等价
[ "$MODE" = "worktree" ] && TARGET="HEAD"

if [ "$FIX" -eq 1 ]; then
    echo "→ --fix：就地格式化改动行"
    git clang-format --force "$TARGET" -- "${FILES[@]}"
    echo "→ 完成。请重新执行一次不带 --fix 的本脚本确认通过。"
    exit 0
fi

OUTPUT="$(git clang-format --diff "$TARGET" -- "${FILES[@]}" 2>&1)" || true

if [ -z "$OUTPUT" ] \
   || [ "$OUTPUT" = "no modified files to format" ] \
   || [ "$OUTPUT" = "clang-format did not modify any files" ]; then
    echo "✅ PASS —— 改动行符合 .clang-format（clang-format 18.x）"
    exit 0
else
    echo "❌ FAIL —— 以下改动行违反 .clang-format（CI 会挂）"
    echo "   自动修复： scripts/local_precheck_format.sh --fix"
    echo "──────────────────────────────────────────────────────────"
    echo "$OUTPUT"
    exit 1
fi
