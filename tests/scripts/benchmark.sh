#!/bin/bash
#
# ============================================================================
#  HTTP Server Framework 性能测试脚本
#
#  功能: 自动启动服务器 → 分级 wrk 压测 → 汇总性能报告
#  用法: ./benchmark.sh [选项]
#
#  选项:
#    --port <port>      服务器端口 (默认 8080)
#    --duration <time>  每级压测时长 (默认 10s, wrk 格式如 5s/30s)
#    --build            测试前先构建 Release 版本
#    --keep-server      测试结束后不关闭服务器
#    --url <path>       压测路径 (默认 /json)
#    --output <dir>     原始结果输出目录
#    -h, --help         显示帮助
#
#  依赖: wrk, curl
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PORT=8080
DURATION=10s
URL_PATH="/json"
DO_BUILD=0
KEEP_SERVER=0
BASE_URL="http://localhost:${PORT}"
BIN_DIR="${PROJECT_ROOT}/build/Release/bin"
BIN="${BIN_DIR}/WebProject"
OUT_DIR="${PROJECT_ROOT}/build/Release/benchmark"
SERVER_PID=""
SERVER_STARTED_BY_US=0

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)       PORT="$2"; BASE_URL="http://localhost:${PORT}"; shift 2 ;;
        --duration)   DURATION="$2"; shift 2 ;;
        --url)        URL_PATH="$2"; shift 2 ;;
        --build)      DO_BUILD=1; shift ;;
        --keep-server) KEEP_SERVER=1; shift ;;
        --output)     OUT_DIR="$2"; shift 2 ;;
        -h|--help)    usage ;;
        *) echo "未知参数: $1"; usage ;;
    esac
done

FULL_URL="${BASE_URL}${URL_PATH}"

echo "============================================================"
echo "  HTTP Server Framework 性能测试"
echo "============================================================"
echo "  目标地址 : ${FULL_URL}"
echo "  压测时长 : ${DURATION}/级"
echo ""

# ---------- 1. 检查依赖 ----------
for cmd in wrk curl; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "[错误] 缺少依赖工具: ${cmd} (apt install wrk curl)"
        exit 1
    fi
done

# ---------- 2. 构建（可选） ----------
if [[ "${DO_BUILD}" == "1" ]]; then
    echo "[1/5] 构建 Release 版本..."
    cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/build/Release" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${PROJECT_ROOT}/build/Release" -j"$(nproc)" 2>&1 | tail -3
fi

# ---------- 3. 启动服务器 ----------
echo "[2/5] 检查服务器状态..."
if curl -s -m 1 "${BASE_URL}/hello" >/dev/null 2>&1; then
    echo "      服务器已在运行: ${BASE_URL}"
else
    if [[ ! -x "${BIN}" ]]; then
        echo "[错误] 未找到服务器二进制: ${BIN}"
        echo "       请先运行: cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release && cmake --build build/Release -j\$(nproc)"
        exit 1
    fi
    echo "      启动服务器: ${BIN}"
    mkdir -p "${OUT_DIR}"
    # 在 bin 目录启动，保证日志相对路径 ../logs/ 与 README 一致
    (
        cd "${BIN_DIR}"
        exec "${BIN}" > "${OUT_DIR}/server.log" 2>&1
    ) &
    SERVER_PID=$!
    echo "${SERVER_PID}" > "${OUT_DIR}/server.pid"
    SERVER_STARTED_BY_US=1
    ready=0
    for _ in $(seq 1 50); do
        if curl -s -m 1 "${BASE_URL}/hello" >/dev/null 2>&1; then ready=1; break; fi
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            echo "[错误] 服务器进程退出，日志: ${OUT_DIR}/server.log"
            tail -20 "${OUT_DIR}/server.log" || true
            exit 1
        fi
        sleep 0.2
    done
    if [[ "${ready}" != "1" ]]; then
        echo "[错误] 服务器 10s 内未就绪"
        exit 1
    fi
fi
echo "      服务器就绪 ✓"

# ---------- 4. 健康检查 ----------
echo "[3/5] 健康检查..."
hello_resp="$(curl -s -m 3 "${BASE_URL}/hello?name=Benchmark")"
echo "      GET /hello?name=Benchmark → ${hello_resp}"
echo "      POST /echo → $(curl -s -m 3 -X POST "${BASE_URL}/echo" -d ping)"
echo "      GET ${URL_PATH}  → OK"

# ---------- 5. 分级压测 ----------
echo "[4/5] 开始分级压力测试..."
mkdir -p "${OUT_DIR}"

# 线程数, 连接数, 级别描述
TEST_CASES=(
    "2,10,Low(2t/10c)"
    "4,50,Medium(4t/50c)"
    "4,100,High(4t/100c)"
    "8,200,VeryHigh(8t/200c)"
    "8,500,Extreme(8t/500c)"
)

declare -A RESULTS   # 级别 -> "rps|avg_ms|p99_ms|mbps|errors"

for tc in "${TEST_CASES[@]}"; do
    IFS=',' read -r threads conns desc <<< "${tc}"
    name="$(echo "${desc}" | tr '()/' '___')"
    raw="${OUT_DIR}/wrk_${name}.txt"
    echo ""
    echo "  ----------------------------------------------------------"
    echo "  级别 ${desc}: wrk -t${threads} -c${conns} -d${DURATION} ${FULL_URL}"
    echo "  ----------------------------------------------------------"
    if ! timeout 90 wrk -t"${threads}" -c"${conns}" -d"${DURATION}" --latency "${FULL_URL}" > "${raw}" 2>&1; then
        echo "  [警告] 该级别压测失败或超时"
        RESULTS["${desc}"]="FAILED|FAILED|FAILED|FAILED|FAILED"
        continue
    fi
    cat "${raw}"

    rps="$(grep 'Requests/sec:' "${raw}" | awk '{print $2}')"
    avg_ms="$(awk '/Thread Stats/,/Latency Distribution/' "${raw}" | grep 'Latency' | head -1 | awk '{print $2}')"
    p99_ms="$(grep '99%' "${raw}" | awk '{print $2}')"
    transfer="$(grep 'Transfer/sec:' "${raw}" | awk '{print $2}')"
    errors="$(grep 'Non-2xx or 3xx responses:' "${raw}" | awk '{print $1}' || true)"
    errors="${errors:-0}"
    RESULTS["${desc}"]="${rps}|${avg_ms}|${p99_ms}|${transfer}|${errors}"
done

# ---------- 6. 汇总报告 ----------
echo ""
echo "[5/5] 生成汇总报告..."
echo ""
echo "=================================================================="
echo "               HTTP Server Framework 性能测试报告"
echo "=================================================================="
echo "  机器信息  : $(nproc) 核 / $(free -h | awk '/Mem:/{print $2}') 内存 / 内核 $(uname -r)"
echo "  服务器    : WebProject (io_uring + C++23 协程) @ 0.0.0.0:${PORT}"
echo "  压测路径  : ${FULL_URL}  时长: ${DURATION}/级"
echo "  压测工具  : wrk $(wrk --version 2>&1 | head -1 | awk '{print $2}')"
echo "------------------------------------------------------------------"
printf "  %-14s %-6s %-6s %-12s %-10s %-10s %-6s\n" "级别" "线程" "连接" "Requests/sec" "Avg延迟" "P99延迟" "错误"
echo "------------------------------------------------------------------"
for tc in "${TEST_CASES[@]}"; do
    IFS=',' read -r threads conns desc <<< "${tc}"
    IFS='|' read -r rps avg_ms p99_ms transfer errors <<< "${RESULTS[$desc]}"
    printf "  %-14s %-6s %-6s %-12s %-10s %-10s %-6s\n" "${desc}" "${threads}" "${conns}" "${rps}" "${avg_ms}" "${p99_ms}" "${errors}"
done
echo "=================================================================="
echo ""
echo "  原始输出保存于: ${OUT_DIR}/wrk_*.txt"
echo "  服务器日志    : ${OUT_DIR}/server.log"

# ---------- 清理 ----------
if [[ "${SERVER_STARTED_BY_US}" == "1" && "${KEEP_SERVER}" == "0" ]]; then
    echo ""
    echo "  关闭测试服务器 (PID ${SERVER_PID})..."
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
fi

echo ""
echo "  测试完成"
