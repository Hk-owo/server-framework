#!/usr/bin/env bash
#
# ============================================================================
#  多事件循环压测 + 瓶颈诊断
#
#  与 benchmark_matrix.sh 的分工：那个脚本量"单实例在各级并发下的表现"，
#  这个脚本回答"扩展到多实例之后，到底谁先到瓶颈"。
#
#  它除了吞吐，还采集两个诊断指标：
#    - 服务端饱和率 = 服务器 CPU / 绑定的核数
#        ≈1 说明服务端满了；明显 <1 说明瓶颈在压测端或内核网络栈
#    - 各事件循环线程的 CPU 分布（极差百分比）
#        验证 SO_REUSEPORT 的哈希分发是否均匀
#
#  核分配：服务端绑前 N 核（N = 实例数），压测端绑其余核；
#  压测端默认用单进程多线程（实测这比多进程绑核更有效——
#  单线程 wrk 约 70k，而服务器单核约 169k，客户端本就需要更多核）。
#
#  用法: ./benchmark_multiloop.sh [--loops 8] [--duration 10s]
#                                 [--conns 500] [--threads 8] [--path /json]
#
#  依赖: wrk, curl, taskset
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BIN="${ROOT}/build/Release/bin/WebProject"
OUT="${ROOT}/build/Release/benchmark/multiloop"
PORT=8080
BASE="http://localhost:${PORT}"
PATH_URI="/json"
LOOPS=8
DURATION=10s
CONNS=500
THREADS=8
WARMUP=2s

while [[ $# -gt 0 ]]; do
    case "$1" in
        --loops)    LOOPS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --conns)    CONNS="$2"; shift 2 ;;
        --threads)  THREADS="$2"; shift 2 ;;
        --path)     PATH_URI="$2"; shift 2 ;;
        --output)   OUT="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

CORES="$(nproc)"
if (( LOOPS >= CORES )); then
    echo "[错误] 实例数(${LOOPS}) 必须小于核数(${CORES})，否则压测端没有核可用"
    exit 1
fi
SERVER_CORES="0-$((LOOPS - 1))"
# 压测端默认**不绑核**：实测绑定会明显限制 wrk（其线程数超过绑定核数时互相争抢），
# 交给内核自由调度反而更快（449k vs 244k）。需要时 export CLIENT_CORES=8-11 启用。
CLIENT_CORES="${CLIENT_CORES:-}"
CLIENT_DESC="${CLIENT_CORES:-不绑核（内核自由调度）}"

run_client() {   # 按需把压测命令绑到 CLIENT_CORES
    if [[ -n "${CLIENT_CORES}" ]]; then
        taskset -c "${CLIENT_CORES}" "$@"
    else
        "$@"
    fi
}

rm -rf "${OUT}"; mkdir -p "${OUT}"
SERVER_LOG="${OUT}/server.log"
SRV_PID=""

cpu_ms() {
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || { echo -1; return; }
    echo "${stat#*) }" | awk -v t="$(getconf CLK_TCK)" '{printf "%d", ($12 + $13) * 1000 / t}'
}
thread_ticks() {   # 每个线程的 (utime+stime)，一行一个，用于看分布
    local t st
    for t in /proc/$1/task/*; do
        st="$(cat "$t/stat" 2>/dev/null)" || continue
        echo "${st#*) }" | awk '{print $12 + $13}'
    done | sort -rn
}
alive() {
    local st
    st="$(ps -o stat= -p "$1" 2>/dev/null)" || return 1
    [[ -n "${st}" && "${st}" != *Z* ]]
}

trap 'SRV_PID="" ; pkill -x WebProject 2>/dev/null || true' EXIT
pkill -x WebProject 2>/dev/null || true
sleep 1

echo "=================================================================="
echo "  多事件循环压测 + 瓶颈诊断"
echo "=================================================================="
echo "  机器      : ${CORES} 核 / 内核 $(uname -r)"
echo "  环境负载  : load1=$(awk '{print $1}' /proc/loadavg)  —— 本机其他进程会显著影响结果；同一配置在不同负载下可能差数倍，务必在空闲机器上跑，或只比较同一时刻的相邻测量"
echo "  实例数    : SF_LOOPS=${LOOPS}   （服务端绑核 ${SERVER_CORES}，压测端 ${CLIENT_DESC}）"
echo "  压测参数  : -t${THREADS} -c${CONNS} -d${DURATION}  ${BASE}${PATH_URI}"
echo "=================================================================="

resources_ok=1
for cmd in wrk curl taskset; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "[错误] 缺少 $cmd"; resources_ok=0; }
done
(( resources_ok )) || exit 1

# ── 启动服务端（绑前 N 核，N 个事件循环实例各自 SO_REUSEPORT 绑同一端口）
# 必须先 cd 到可执行文件所在目录：日志是相对工作目录的 "../logs/test.log"，
# 从别处启动会把它写到仓库外面去（撞上只读目录时进程直接起不来）
: > "${SERVER_LOG}"
( cd "${BIN%/*}" && exec taskset -c "${SERVER_CORES}" env SF_LOOPS="${LOOPS}" "${BIN}" >> "${SERVER_LOG}" 2>&1 ) &
SRV_PID=$!
for _ in $(seq 1 150); do
    curl -s -m 1 "${BASE}/hello" >/dev/null 2>&1 && break
    alive "${SRV_PID}" || { echo "[错误] 服务器启动失败"; tail -20 "${SERVER_LOG}"; exit 1; }
    sleep 0.2
done
echo "  服务器 PID=${SRV_PID}  线程数=$(awk '/^Threads/{print $2}' /proc/${SRV_PID}/status)"

# 预热
run_client wrk -t2 -c10 -d"${WARMUP}" "${BASE}${PATH_URI}" >/dev/null 2>&1 || true

# ── 压测（单进程多线程，绑压测端可用核）
raw="${OUT}/wrk.txt"
c0="$(cpu_ms "${SRV_PID}")"
t0="$(date +%s%3N)"
run_client wrk -t"${THREADS}" -c"${CONNS}" -d"${DURATION}" --latency \
        "${BASE}${PATH_URI}" > "${raw}" 2>&1
t1="$(date +%s%3N)"
c1="$(cpu_ms "${SRV_PID}")"

# 压测结束后立刻采一次线程分布（此时各实例的累计 tick 已包含本次压测）
thread_ticks "${SRV_PID}" > "${OUT}/thread_ticks.txt"
alive "${SRV_PID}" && crash=0 || crash=1

# ── 汇总
wall=$(( t1 - t0 ))
srv_cpu=$(( c1 - c0 ))
rps="$(grep 'Requests/sec:' "${raw}" 2>/dev/null | awk '{print $2}')"

{
    echo "loops=${LOOPS}"
    echo "server_cores=${SERVER_CORES}"
    echo "client_cores=${CLIENT_CORES}"
    echo "threads=${THREADS}"
    echo "connections=${CONNS}"
    echo "crashed=${crash}"
    echo "wall_ms=${wall}"
    echo "server_cpu_ms=${srv_cpu}"
} > "${OUT}/meta.txt"

echo ""
echo "--- 吞吐与延迟 ---"
grep -E 'Latency Distribution|^\s+(50|75|90|99)%|Requests/sec|Socket errors' "${raw}" | head -10 | sed 's/^/  /'

echo ""
echo "--- 服务端饱和率 ---"
awk -v rps="${rps:-0}" -v c="${srv_cpu}" -v w="${wall}" -v n="${LOOPS}" 'BEGIN{
    cores = c / w;
    printf "  服务器 CPU        : %.2f 核（绑定 %d 核）\n", cores, n;
    printf "  饱和率            : %.0f%%   （接近 100%% = 服务端是瓶颈；明显偏低 = 瓶颈在压测端或内核网络栈）\n", cores / n * 100;
    if (rps > 0) printf "  每请求 CPU        : %.1f us\n", c * 1000 / rps;
    if (rps > 0 && cores > 0) printf "  单核效率          : %.0f req/s/核\n", rps / cores;
}'

echo ""
echo "--- 各线程 CPU tick 分布（验证 SO_REUSEPORT 是否均匀）---"
awk -v n="${LOOPS}" '{
        v[NR] = $1;
     }
     END{
        if (NR == 0) { print "  (无数据)"; exit }
        max = v[1]; min = v[1];
        for (i = 1; i <= NR && i <= n; i++) { if (v[i] > max) max = v[i]; if (v[i] < min) min = v[i] }
        printf "  最高=%d 最低=%d 极差=%.0f%%\n", max, min, (max - min) / max * 100;
        printf "  各线程: ";
        for (i = 1; i <= NR && i <= n; i++) printf "%d ", v[i];
        printf "\n  （前 %d 个应为各事件循环线程，分布越平均说明内核哈希越均匀）\n", n;
     }' "${OUT}/thread_ticks.txt"

[[ "${crash}" == "1" ]] && echo "" && echo "  [警告] 服务器在压测中退出"

echo ""
echo "=================================================================="
echo "  原始输出: ${OUT}/wrk.txt   服务端日志: ${SERVER_LOG}"
echo "=================================================================="

kill "${SRV_PID}" 2>/dev/null; wait "${SRV_PID}" 2>/dev/null
SRV_PID=""
