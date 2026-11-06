#!/usr/bin/env bash
# 验证"接收方能把 uife 与修改过的 LGPL 库重新链接"这套机制确实成立。
#
# 做两件事，缺一不可：
#   ① 正向：把一个**改过的** libewf.a 交给构建系统，必须能重链出可运行的 uife；
#   ② 反向：把同一个位置的库换成损坏文件，链接必须**失败**——否则说明那个库根本
#      没参与链接，"换库重链"只是文档上的说法。
#
# 任何一步不符合预期都以非零退出码结束，CI 的 relink-check 任务就是跑它。
#
# 用法：
#   bash scripts/relink-check.sh --build build --source . [--sample build/corpus/.../sample.img]

set -uo pipefail

build_dir="build"
source_dir="."
sample=""
static_lib=""

while [ $# -gt 0 ]; do
    case "$1" in
        --build) build_dir="$2"; shift 2 ;;
        --source) source_dir="$2"; shift 2 ;;
        --sample) sample="$2"; shift 2 ;;
        --static-lib) static_lib="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

exe="$build_dir/uife"
[ -f "$exe" ] || exe="$build_dir/uife.exe"
if [ ! -f "$exe" ]; then
    echo "找不到 $build_dir/uife（先完整构建一次）" >&2
    exit 2
fi

log() { printf '%s\n' "$*"; }
fail() { printf '::error::%s\n' "$*" >&2; exit 1; }
summary() {
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
}

# ---------------------------------------------------------------- 定位静态库
is_thin_archive() {
    [ "$(head -c 8 "$1" 2>/dev/null)" = "!<thin>" ]
}

find_static_libewf() {
    [ -n "$static_lib" ] && { printf '%s' "$static_lib"; return 0; }
    local dir
    dir=$(pkg-config --variable=libdir libewf 2>/dev/null || true)
    local candidates=()
    [ -n "$dir" ] && candidates+=("$dir/libewf.a")
    candidates+=(
        /usr/lib/x86_64-linux-gnu/libewf.a
        /usr/lib/aarch64-linux-gnu/libewf.a
        /usr/lib64/libewf.a
        /usr/lib/libewf.a
        /usr/local/lib/libewf.a
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        if [ -f "$candidate" ] && ! is_thin_archive "$candidate"; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

original_lib=$(find_static_libewf || true)
if [ -z "$original_lib" ]; then
    # 只有共享库时也能验证"重链"这件事，但验证不了"换一份改过的静态库"。
    fail "找不到静态 libewf.a（发行版的 -dev 包通常带；共享库版本无法验证换库重链）"
fi
log "[发现] 静态 libewf: $original_lib ($(wc -c < "$original_lib") 字节)"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/uife-relink-XXXXXX")
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT

# ------------------------------------------------------ 造一个"改过的"静态库
modified_lib="$work_dir/libewf-modified.a"
cp "$original_lib" "$modified_lib"

cat > "$work_dir/relink_marker.c" <<'EOF'
/* 只用来改变归档内容，证明链接用的是我们提供的那一份。 */
const char uife_relink_marker[] = "uife-relink-marker";
int uife_relink_marker_entry(void) { return 0; }
EOF

cc_bin="${CC:-cc}"
command -v "$cc_bin" >/dev/null 2>&1 || cc_bin=gcc
"$cc_bin" -c -o "$work_dir/relink_marker.o" "$work_dir/relink_marker.c" \
    || fail "编译标记目标文件失败"
ar r "$modified_lib" "$work_dir/relink_marker.o" >/dev/null \
    || fail "改写静态库失败（ar r）"
log "[准备] 改过的静态库: $modified_lib"

# ---------------------------------------------------------------- 链接与验证
relink_and_build() {
    local library="$1"
    # UIFE_USE_PKG_CONFIG=OFF：否则 libewf.pc 会把链接目标固定回系统那份，
    # -DLIBEWF_LIBRARY 形同虚设，"换库"根本没发生。
    cmake -S "$source_dir" -B "$build_dir" \
        -DUIFE_USE_PKG_CONFIG=OFF -DLIBEWF_LIBRARY="$library" >/dev/null \
        || return 1
    cmake --build "$build_dir" --target uife >/dev/null 2>&1
}

smoke_test() {
    if [ -z "$sample" ] || [ ! -f "$sample" ]; then
        log "[跳过] 没提供 --sample，只验证链接"
        return 0
    fi
    local output
    if ! output=$("$exe" --json probe "$sample" 2>&1); then
        fail "重链后的 uife 跑不通 probe: $output"
    fi
    case "$output" in
        *'"format"'*) log "[通过] probe $(basename "$sample") 正常返回格式信息" ;;
        *) fail "重链后的 uife 输出里没有 format 字段: $output" ;;
    esac
}

# ① 正向：改过的库必须能链上
if ! relink_and_build "$modified_lib"; then
    fail "用改过的 libewf.a 重链失败——"$'\n'"文档承诺的换库重链不成立"
fi
log "[通过] 用改过的 libewf.a 重链成功"
smoke_test

# ② 反向：换回原始库，再把该位置替换成损坏文件，链接必须失败
broken_lib="$work_dir/libewf-broken.a"
printf 'this is not an archive\n' > "$broken_lib"
if relink_and_build "$broken_lib"; then
    fail "把 libewf.a 换成损坏文件后链接仍然成功——说明该库没有真正参与链接"
fi
log "[通过] 损坏的 libewf.a 让链接失败，确认库确实参与链接"

# ③ 恢复可用状态，避免把构建目录留在坏掉的配置上
if ! relink_and_build "$original_lib"; then
    fail "恢复原始 libewf.a 后重链失败"
fi
smoke_test
log "[完成] 换库重链机制验证通过"

summary "### relink-check（LGPL §4(d)(0)）"
summary ""
summary "- 静态库: \`$original_lib\`"
summary "- 改过的库可重链: 是"
summary "- 损坏的库会链接失败: 是（证明库确实参与链接）"
summary "- smoke test: ${sample:-未提供样本}"
