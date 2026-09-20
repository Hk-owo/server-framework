#!/usr/bin/env bash
#
# ============================================================================
#  端点 × 并发矩阵压测
#
#  4 个端点 × 5 个并发级别 × N 轮（默认 3），每轮 DURATION（默认 10s），Keep-Alive。
#  每个端点前重启服务器并预热，端点之间互不污染；某轮压测中服务器退出则记录
#  信号并自动重启，继续后续轮次。
#
#  同时采集：服务器进程 CPU 时间（含 starttime 校验，防 PID 复用读到脏值）、
#  wrk 客户端自身 CPU 时间（判断瓶颈在服务端还是压测端）。
#
#  用法: ./benchmark_matrix.sh [--duration 10s] [--rounds 3] [--output DIR]
#
#  输出: 每个轮次一个 .txt（wrk 原始输出）+ .meta（CPU/耗时/是否崩溃）
#  汇总: python3 benchmark_report.py <输出目录>
#
#  依赖: wrk, curl
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BIN_DIR="${ROOT}/build/Release/bin"
BIN="${BIN_DIR}/WebProject"
OUT="${ROOT}/build/Release/benchmark/matrix"
PORT=8080
BASE="http://localhost:${PORT}"
DURATION=10s
ROUNDS=3
WARMUP=2s

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --rounds)   ROUNDS="$2"; shift 2 ;;
        --output)   OUT="$2"; shift 2 ;;
        --port)     PORT="$2"; BASE="http://localhost:${PORT}"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

# 清空并重建输出目录。必须放在下面写 lua 脚本**之前**：否则那句 rm -rf 会把
# 刚写好的 lua 一起删掉，wrk 找不到脚本就退回默认 GET 去打 /echo，
# 那一格量到的其实是 404 的吞吐
rm -rf "${OUT}"
mkdir -p "${OUT}"

LUA_DIR="${OUT}/lua"
mkdir -p "${LUA_DIR}"
cat > "${LUA_DIR}/post_echo.lua" <<'LUA'
wrk.method = "POST"
wrk.body   = '{"ping":"pong"}'
wrk.headers["Content-Type"] = "application/json"
LUA

# name|path|lua(或 -)
ENDPOINTS=(
  "json|/json|-"
  "async_status|/async/status|-"
  "api_user|/api/user/42|-"
  "post_echo|/echo|${LUA_DIR}/post_echo.lua"
)

# name|threads|connections
LEVELS=(
  "low|2|10"
  "medium|4|50"
  "high|4|100"
  "veryhigh|8|200"
  "extreme|8|500"
)

CLK_TCK="$(getconf CLK_TCK)"
SRV_PID=""

# server 日志：固定单文件，每次启动清零 + 上限守护，不会无限增长
SERVER_LOG="${OUT}/server.log"
SERVER_LOG_LIMIT=$((16 * 1024 * 1024))   # 16MB
LOG_GUARD_PID=""

# 日志上限守护：超过上限就清空，防止异常（如 accept 死循环）刷日志把磁盘写满
log_guard_start() {
    ( while kill -0 "$1" 2>/dev/null; do
          sleep 1
          local s
          s="$(stat -c%s "${SERVER_LOG}" 2>/dev/null || echo 0)"
          [[ "${s}" -gt "${SERVER_LOG_LIMIT}" ]] && : > "${SERVER_LOG}"
      done ) &
    LOG_GUARD_PID=$!
}

# ── /proc/<pid>/stat 解析（去掉 comm 字段后再按位置取，避免进程名含空格错位）
srv_identity() {   # starttime：用于确认 PID 没被复用
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || { echo ""; return; }
    echo "${stat#*) }" | awk '{print $20}'
}
srv_cpu_ms() {     # utime + stime → ms
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || { echo -1; return; }
    echo "${stat#*) }" | awk -v t="${CLK_TCK}" '{printf "%d", ($12 + $13) * 1000 / t}'
}
srv_alive() {      # 显式排除僵尸态
    local st
    st="$(ps -o stat= -p "$1" 2>/dev/null)" || return 1
    [[ -n "${st}" && "${st}" != *Z* ]] || return 1
}

start_server() {
    local tag="$1"
    # 等端口真正空闲：上一个实例退出时 spdlog 排空队列会拖慢析构
    for _ in $(seq 1 100); do
        ss -ltn 2>/dev/null | grep -q ":${PORT}" || break
        sleep 0.2
    done
    # 每次启动清零：日志只保留当前端点的
    : > "${SERVER_LOG}"
    ( cd "${BIN_DIR}" && exec "${BIN}" >> "${SERVER_LOG}" 2>&1 ) &
    SRV_PID=$!
    log_guard_start "${SRV_PID}"
    for _ in $(seq 1 100); do
        if curl -s -m 1 "${BASE}/hello" >/dev/null 2>&1; then
            return 0
        fi
        if ! srv_alive "${SRV_PID}"; then
            echo "    [错误] 服务器(${tag})启动失败，日志尾部："
            tail -20 "${SERVER_LOG}" || true
            exit 1
        fi
        sleep 0.2
    done
    echo "    [错误] 服务器(${tag})就绪超时"
    exit 1
}

stop_server() {
    [[ -n "${SRV_PID}" ]] || return 0
    srv_alive "${SRV_PID}" && kill "${SRV_PID}" 2>/dev/null
    wait "${SRV_PID}" 2>/dev/null
    SRV_PID=""
}

trap 'stop_server' EXIT

echo "=================================================================="
echo "  端点 × 并发矩阵压测（Keep-Alive）"
echo "=================================================================="
echo "  机器      : $(nproc) 核 / $(free -h | awk '/Mem:/{print $2}') 内存 / 内核 $(uname -r)"
echo "  服务器    : ${BIN}"
echo "  端点      : ${#ENDPOINTS[@]} 个   级别: ${#LEVELS[@]} 个   轮次/级: ${ROUNDS}"
echo "  单轮时长  : ${DURATION}   预热: ${WARMUP}/端点   连接模式: Keep-Alive"
echo "  总计      : $(( ${#ENDPOINTS[@]} * ${#LEVELS[@]} * ROUNDS )) 轮压测"
echo "  输出目录  : ${OUT}"
echo "=================================================================="

for ep in "${ENDPOINTS[@]}"; do
    IFS='|' read -r ep_name ep_path ep_lua <<< "${ep}"
    mkdir -p "${OUT}/${ep_name}"

    echo ""
    echo "### 端点 ${ep_name}  (${ep_path})"
    start_server "${ep_name}"

    extra=()
    if [[ "${ep_lua}" != "-" ]]; then
        extra=(-s "${ep_lua}")
    fi

    # 预热（结果丢弃），让线程池/连接进入稳态
    wrk -t2 -c10 -d"${WARMUP}" "${extra[@]}" "${BASE}${ep_path}" >/dev/null 2>&1 || true

    for lv in "${LEVELS[@]}"; do
        IFS='|' read -r lv_name threads conns <<< "${lv}"
        for r in $(seq 1 "${ROUNDS}"); do
            if ! srv_alive "${SRV_PID}"; then
                wait "${SRV_PID}" 2>/dev/null; rc=$?
                echo "    [重启] 上一轮服务器已退出 (exit=${rc} sig=$(( rc > 128 ? rc - 128 : 0 )))，重启后继续"
                start_server "${ep_name}"
                wrk -t2 -c10 -d"${WARMUP}" "${extra[@]}" "${BASE}${ep_path}" >/dev/null 2>&1 || true
            fi

            raw="${OUT}/${ep_name}/${lv_name}_r${r}.txt"
            id0="$(srv_identity "${SRV_PID}")"
            cpu0="$(srv_cpu_ms "${SRV_PID}")"
            t0="$(date +%s%3N)"
            /usr/bin/time -f "%e %U %S" -o "${raw}.time" \
                timeout 180 wrk -t"${threads}" -c"${conns}" -d"${DURATION}" --latency \
                "${extra[@]}" "${BASE}${ep_path}" > "${raw}" 2>&1
            rc=$?
            t1="$(date +%s%3N)"
            id1="$(srv_identity "${SRV_PID}")"
            cpu1="$(srv_cpu_ms "${SRV_PID}")"

            crashed=0
            exitsig=0
            if ! srv_alive "${SRV_PID}" || [[ "${id0}" != "${id1}" ]]; then
                crashed=1
                wait "${SRV_PID}" 2>/dev/null; exitsig=$?
            fi

            cpu_delta=-1
            if [[ "${crashed}" == "0" && "${cpu0}" -ge 0 && "${cpu1}" -ge 0 ]]; then
                cpu_delta=$(( cpu1 - cpu0 ))
            fi

            {
                echo "exit_code=${rc}"
                echo "level=${lv_name}"
                echo "threads=${threads}"
                echo "connections=${conns}"
                echo "round=${r}"
                echo "crashed=${crashed}"
                echo "server_exit_sig=$(( exitsig > 128 ? exitsig - 128 : 0 ))"
                echo "server_cpu_ms=${cpu_delta}"
                echo "wall_ms=$(( t1 - t0 ))"
            } > "${raw}.meta"

            rps="$(grep 'Requests/sec:' "${raw}" 2>/dev/null | awk '{print $2}')"
            if [[ "${crashed}" == "1" ]]; then
                echo "    ${lv_name} (-t${threads} -c${conns}) r${r}: 服务器崩溃 (sig=$(( exitsig > 128 ? exitsig - 128 : 0 )))  rps=${rps:-N/A}"
            else
                echo "    ${lv_name} (-t${threads} -c${conns}) r${r}: ${rps:-FAILED} req/s"
            fi
        done
        sleep 1
    done

    stop_server
done

echo ""
echo "=================================================================="
echo "  压测完成，原始输出：${OUT}"
echo "  汇总表格：python3 ${SCRIPT_DIR}/benchmark_report.py ${OUT}"
echo "=================================================================="
