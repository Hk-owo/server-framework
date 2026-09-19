#!/usr/bin/env bash
#
# ============================================================================
#  低压梯度测试
#
#  - 低并发梯度 c1 / c2 / c5 / c10（同步端点 + 异步端点）
#  - 每档记录：吞吐、延迟、服务器 CPU（折算核数）
#  - 每档压测结束后再测"回到空载"3s 的 CPU，用于确认撤压后是否还在烧 CPU
#  - 用 exec 让 $! 就是服务器 PID（否则采到的是包装子 shell）
#
#  用法: ./benchmark_low_pressure.sh [--duration 10s] [--output DIR]
#
#  依赖: wrk, curl
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BIN_DIR="${ROOT}/build/Release/bin"
BIN="${BIN_DIR}/WebProject"
OUT="${ROOT}/build/Release/benchmark/low-pressure"
PORT=8080
BASE="http://localhost:${PORT}"
DURATION=10s
IDLE_WIN=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --output)   OUT="$2"; shift 2 ;;
        --port)     PORT="$2"; BASE="http://localhost:${PORT}"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

CLK_TCK="$(getconf CLK_TCK)"
SRV_PID=""

# server 日志：固定单文件，每次启动清零 + 上限守护，不会无限增长
SERVER_LOG="${OUT}/server.log"
SERVER_LOG_LIMIT=$((16 * 1024 * 1024))
LOG_GUARD_PID=""

log_guard_start() {
    ( while kill -0 "$1" 2>/dev/null; do
          sleep 1
          local s
          s="$(stat -c%s "${SERVER_LOG}" 2>/dev/null || echo 0)"
          [[ "${s}" -gt "${SERVER_LOG_LIMIT}" ]] && : > "${SERVER_LOG}"
      done ) &
    LOG_GUARD_PID=$!
}

cpu_ms() {
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || { echo -1; return; }
    echo "${stat#*) }" | awk -v t="${CLK_TCK}" '{printf "%d", ($12 + $13) * 1000 / t}'
}
thr() { awk '/^Threads/{print $2}' "/proc/$1/status" 2>/dev/null; }
alive() {
    local st
    st="$(ps -o stat= -p "$1" 2>/dev/null)" || return 1
    [[ -n "${st}" && "${st}" != *Z* ]]
}
start_server() {
    local tag="$1"
    for _ in $(seq 1 100); do
        ss -ltn 2>/dev/null | grep -q ":${PORT}" || break
        sleep 0.2
    done
    : > "${SERVER_LOG}"
    ( cd "${BIN_DIR}" && exec "${BIN}" >> "${SERVER_LOG}" 2>&1 ) &
    SRV_PID=$!
    log_guard_start "${SRV_PID}"
    for _ in $(seq 1 100); do
        curl -s -m 1 "${BASE}/hello" >/dev/null 2>&1 && return 0
        alive "${SRV_PID}" || return 1
        sleep 0.2
    done
    return 1
}
stop_server() {
    [[ -n "${SRV_PID}" ]] || return 0
    alive "${SRV_PID}" && kill "${SRV_PID}" 2>/dev/null
    wait "${SRV_PID}" 2>/dev/null
    SRV_PID=""
}
trap 'stop_server' EXIT

rm -rf "${OUT}"
mkdir -p "${OUT}"

echo "=================================================================="
echo "  低压状态测试   机器: $(nproc) 核 / 内核 $(uname -r)"
echo "=================================================================="

# ── 纯空载基线（不施加任何请求）
start_server "idle_baseline"
c0="$(cpu_ms "${SRV_PID}")"; sleep 5; c1="$(cpu_ms "${SRV_PID}")"
{
    echo "cpu_ms=$(( c1 - c0 ))"
    echo "window_ms=5000"
    echo "threads=$(thr "${SRV_PID}")"
} > "${OUT}/idle_baseline.meta"
printf "  [纯空载基线] 5s CPU 增量 %sms (~%s%%)  线程数=%s\n" \
       "$(( c1 - c0 ))" "$(( (c1 - c0) * 100 / 5000 ))" "$(thr "${SRV_PID}")"
stop_server

# name|path
ENDPOINTS=(
  "hello|/hello?name=Benchmark"
  "json|/json"
  "async_status|/async/status"
)
# label|threads|conns
LEVELS=(
  "c1|1|1"
  "c2|1|2"
  "c5|1|5"
  "c10|2|10"
)

for ep in "${ENDPOINTS[@]}"; do
    IFS='|' read -r ep_name ep_path <<< "${ep}"
    mkdir -p "${OUT}/${ep_name}"
    echo ""
    echo "### ${ep_name}  (${ep_path})"
    for lv in "${LEVELS[@]}"; do
        IFS='|' read -r lv_name threads conns <<< "${lv}"
        if ! start_server "${ep_name}_${lv_name}"; then
            echo "    ${lv_name}: 启动失败"
            continue
        fi
        raw="${OUT}/${ep_name}/${lv_name}.txt"
        c0="$(cpu_ms "${SRV_PID}")"
        t0="$(date +%s%3N)"
        timeout 120 wrk -t"${threads}" -c"${conns}" -d"${DURATION}" --latency \
              "${BASE}${ep_path}" > "${raw}" 2>&1
        t1="$(date +%s%3N)"
        c1="$(cpu_ms "${SRV_PID}")"

        crashed=0
        sig=0
        if ! alive "${SRV_PID}"; then
            crashed=1
            wait "${SRV_PID}" 2>/dev/null; sig=$?
        fi

        # 回到空载：静置 1s 让残留请求结束，再测 IDLE_WIN 秒
        idle_cpu=-1
        idle_thr=0
        if [[ "${crashed}" == "0" ]]; then
            sleep 1
            c2="$(cpu_ms "${SRV_PID}")"
            sleep "${IDLE_WIN}"
            c3="$(cpu_ms "${SRV_PID}")"
            idle_cpu=$(( c3 - c2 ))
            idle_thr="$(thr "${SRV_PID}")"
        fi

        wall=$(( t1 - t0 ))
        srv_cpu=$(( c1 - c0 ))
        {
            echo "threads=${threads}"
            echo "connections=${conns}"
            echo "crashed=${crashed}"
            echo "wall_ms=${wall}"
            echo "server_cpu_ms=${srv_cpu}"
            echo "idle_cpu_ms=${idle_cpu}"
            echo "idle_window_ms=$(( IDLE_WIN * 1000 ))"
            echo "threads_after=${idle_thr}"
        } > "${raw}.meta"

        rps="$(grep 'Requests/sec:' "${raw}" 2>/dev/null | awk '{print $2}')"
        avg="$(awk '/Thread Stats/,/Latency Distribution/' "${raw}" | grep 'Latency' | head -1 | awk '{print $2}')"
        p99="$(grep '99%' "${raw}" | awk '{print $2}')"
        if [[ "${crashed}" == "1" ]]; then
            printf "    %-4s (-t%s -c%s): 服务器崩溃 (sig=%s)\n" "${lv_name}" "${threads}" "${conns}" "$(( sig > 128 ? sig - 128 : 0 ))"
        else
            printf "    %-4s (-t%s -c%s): %-11s req/s  avg %-8s p99 %-8s | 压测中 CPU %s.%02d 核 | 撤压后空载 %s%%, 线程数 %s\n" \
                "${lv_name}" "${threads}" "${conns}" "${rps:-N/A}" "${avg:-N/A}" "${p99:-N/A}" \
                "$(( srv_cpu / wall ))" "$(( (srv_cpu * 100 / wall) % 100 ))" \
                "$(( idle_cpu * 100 / (IDLE_WIN * 1000) ))" "${idle_thr}"
        fi
        stop_server
    done
done

echo ""
echo "=================================================================="
echo "  完成，原始输出: ${OUT}"
echo "=================================================================="
