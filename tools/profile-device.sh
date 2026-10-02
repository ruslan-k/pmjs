#!/bin/sh
# PMJS on-device performance profiler.
#
# Runs a real game/launcher command and samples Linux /proc + /sys while it is
# alive. No Python, bc, jq, pidstat, pgrep, or procps dependency is required.
#
# Usage:
#   tools/profile-device.sh [-i SECONDS] [-o DIR] [--no-perf] -- COMMAND [ARG...]
#
# Examples:
#   tools/profile-device.sh -- ./example/run-game.sh /path/game /path/saves
#   tools/profile-device.sh -i 0.5 -o /tmp/omori-profile -- ./OMORI.sh
#
# Outputs:
#   samples.csv   time series for CPU/RSS/PSS/threads/I/O/temperature/frequency
#   summary.txt   compact aggregate report
#   game.log      stdout/stderr of the profiled launcher
#   device.txt    kernel/CPU/memory/devfreq metadata
#   perf-stat.txt optional perf(1) counters when perf is installed and usable

set -u

INTERVAL="${PMJS_PROFILE_INTERVAL:-1}"
OUT_DIR="${PMJS_PROFILE_OUT:-}"
USE_PERF=1

usage() {
  cat <<'EOF'
Usage: profile-device.sh [-i SECONDS] [-o DIR] [--no-perf] -- COMMAND [ARG...]

  -i SECONDS   Sampling period (default: 1; fractional values are allowed)
  -o DIR       Output directory (default: ./pmjs-profile-YYYYmmdd-HHMMSS)
  --no-perf    Do not attempt an optional perf stat attachment
  -h, --help   Show this help

The command may be a PMJS runner invocation or the normal PortMaster launcher.
The profiler follows descendants of the launcher, so wrapping the real Node
process in a shell script is fine.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -i)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      INTERVAL="$2"; shift 2 ;;
    -o)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      OUT_DIR="$2"; shift 2 ;;
    --no-perf)
      USE_PERF=0; shift ;;
    -h|--help)
      usage; exit 0 ;;
    --)
      shift; break ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2 ;;
  esac
done

[ "$#" -gt 0 ] || { echo "No command supplied." >&2; usage >&2; exit 2; }

case "$INTERVAL" in
  ''|*[!0-9.]*)
    echo "Invalid sampling interval: $INTERVAL" >&2
    exit 2 ;;
esac

if [ -z "$OUT_DIR" ]; then
  stamp="$(date '+%Y%m%d-%H%M%S' 2>/dev/null || echo run)"
  OUT_DIR="./pmjs-profile-$stamp"
fi
mkdir -p "$OUT_DIR" || exit 1

SAMPLES="$OUT_DIR/samples.csv"
SUMMARY="$OUT_DIR/summary.txt"
GAME_LOG="$OUT_DIR/game.log"
DEVICE="$OUT_DIR/device.txt"
PERF_LOG="$OUT_DIR/perf-stat.txt"
PID_FILE="$OUT_DIR/root.pid"

CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
case "$CLK_TCK" in ''|*[!0-9]*) CLK_TCK=100 ;; esac

now_ms() {
  value="$(date +%s%3N 2>/dev/null || true)"
  case "$value" in
    ''|*N*) echo "$(( $(date +%s) * 1000 ))" ;;
    *) echo "$value" ;;
  esac
}

read_kb() {
  key="$1"
  awk -v k="$key" '$1 == k ":" { print $2; exit }' /proc/meminfo 2>/dev/null
}

children_of() {
  pid="$1"
  file="/proc/$pid/task/$pid/children"
  if [ -r "$file" ]; then
    cat "$file" 2>/dev/null
    return
  fi
  # Fallback for kernels without task/*/children. This is slower but only used
  # on unusual old kernels.
  for stat in /proc/[0-9]*/stat; do
    [ -r "$stat" ] || continue
    ppid="$(awk '{print $4}' "$stat" 2>/dev/null)"
    [ "$ppid" = "$pid" ] && basename "$(dirname "$stat")"
  done
}

collect_pids_rec() {
  pid="$1"
  [ -d "/proc/$pid" ] || return
  echo "$pid"
  for child in $(children_of "$pid"); do
    collect_pids_rec "$child"
  done
}

collect_pids() {
  collect_pids_rec "$ROOT_PID" | awk '!seen[$1]++'
}

sum_status_field() {
  field="$1"; shift
  total=0
  for pid in "$@"; do
    value="$(awk -v k="$field" '$1 == k ":" { print $2; exit }' "/proc/$pid/status" 2>/dev/null)"
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    total=$((total + value))
  done
  echo "$total"
}

sum_stat_ticks() {
  total=0
  for pid in "$@"; do
    [ -r "/proc/$pid/stat" ] || continue
    value="$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null)"
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    total=$((total + value))
  done
  echo "$total"
}

sum_pss_kb() {
  total=0
  for pid in "$@"; do
    f="/proc/$pid/smaps_rollup"
    [ -r "$f" ] || continue
    value="$(awk '$1 == "Pss:" {print $2; exit}' "$f" 2>/dev/null)"
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    total=$((total + value))
  done
  echo "$total"
}

sum_io_field() {
  field="$1"; shift
  total=0
  for pid in "$@"; do
    value="$(awk -v k="$field" '$1 == k ":" {print $2; exit}' "/proc/$pid/io" 2>/dev/null)"
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    total=$((total + value))
  done
  echo "$total"
}

sum_fd_count() {
  total=0
  for pid in "$@"; do
    [ -d "/proc/$pid/fd" ] || continue
    value="$(ls -1 "/proc/$pid/fd" 2>/dev/null | wc -l | tr -d ' ')"
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    total=$((total + value))
  done
  echo "$total"
}

average_cpu_freq_khz() {
  count=0
  total=0
  for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
    [ -r "$f" ] || continue
    value="$(cat "$f" 2>/dev/null)"
    case "$value" in ''|*[!0-9]*) continue ;; esac
    total=$((total + value))
    count=$((count + 1))
  done
  [ "$count" -gt 0 ] && echo $((total / count)) || echo 0
}

find_gpu_freq_file() {
  for d in /sys/class/devfreq/*; do
    [ -d "$d" ] || continue
    name="$(basename "$d" | tr '[:upper:]' '[:lower:]')"
    target="$(readlink "$d/device" 2>/dev/null | tr '[:upper:]' '[:lower:]')"
    case "$name $target" in
      *mali*|*gpu*)
        [ -r "$d/cur_freq" ] && { echo "$d/cur_freq"; return; } ;;
    esac
  done
}

GPU_FREQ_FILE="$(find_gpu_freq_file)"

gpu_freq_khz() {
  [ -n "$GPU_FREQ_FILE" ] && [ -r "$GPU_FREQ_FILE" ] || { echo 0; return; }
  value="$(cat "$GPU_FREQ_FILE" 2>/dev/null)"
  case "$value" in ''|*[!0-9]*) echo 0 ;; *)
    # devfreq is conventionally Hz, cpufreq is conventionally kHz.
    [ "$value" -gt 10000000 ] && echo $((value / 1000)) || echo "$value"
  esac
}

max_temp_millic() {
  max=0
  for f in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$f" ] || continue
    value="$(cat "$f" 2>/dev/null)"
    case "$value" in ''|*[!0-9-]*) continue ;; esac
    [ "$value" -gt "$max" ] && max="$value"
  done
  echo "$max"
}

load1() {
  awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0
}

write_device_info() {
  {
    echo "timestamp=$(date -Iseconds 2>/dev/null || date)"
    echo "command=$*"
    echo "interval_seconds=$INTERVAL"
    echo "clk_tck=$CLK_TCK"
    echo "gpu_freq_file=${GPU_FREQ_FILE:-none}"
    echo
    echo "=== uname ==="
    uname -a 2>/dev/null || true
    echo
    echo "=== cpuinfo ==="
    cat /proc/cpuinfo 2>/dev/null || true
    echo
    echo "=== meminfo ==="
    cat /proc/meminfo 2>/dev/null || true
    echo
    echo "=== devfreq ==="
    for d in /sys/class/devfreq/*; do
      [ -d "$d" ] || continue
      echo "$d"
      for f in name cur_freq min_freq max_freq available_frequencies governor; do
        [ -r "$d/$f" ] && echo "  $f=$(cat "$d/$f" 2>/dev/null)"
      done
    done
    echo
    echo "=== thermal zones ==="
    for d in /sys/class/thermal/thermal_zone*; do
      [ -d "$d" ] || continue
      echo "$d type=$(cat "$d/type" 2>/dev/null) temp=$(cat "$d/temp" 2>/dev/null)"
    done
  } > "$DEVICE"
}

write_device_info "$@"

echo "elapsed_s,pids,cpu_pct,rss_kb,hwm_kb,pss_kb,threads,fd_count,read_bytes,write_bytes,voluntary_ctxt,nonvoluntary_ctxt,mem_available_kb,swap_free_kb,cpu_freq_khz,gpu_freq_khz,max_temp_millic,load1" > "$SAMPLES"

echo "[pmjs-profile] output: $OUT_DIR"
echo "[pmjs-profile] launching: $*"

START_MS="$(now_ms)"
"$@" >"$GAME_LOG" 2>&1 &
ROOT_PID=$!
echo "$ROOT_PID" > "$PID_FILE"

PERF_PID=""
if [ "$USE_PERF" -eq 1 ] && command -v perf >/dev/null 2>&1; then
  # Attachment is best-effort: many handheld kernels intentionally disable
  # perf_event_open for unprivileged users.
  (
    perf stat -p "$ROOT_PID"       -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions       2>"$PERF_LOG"
  ) &
  PERF_PID=$!
fi

cleanup() {
  if [ -n "$PERF_PID" ]; then
    kill "$PERF_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

PREV_MS="$START_MS"
PREV_TICKS=0

while kill -0 "$ROOT_PID" 2>/dev/null; do
  PIDS="$(collect_pids)"
  # shellcheck disable=SC2086
  set -- $PIDS
  PID_COUNT=$#

  NOW_MS="$(now_ms)"
  TICKS="$(sum_stat_ticks "$@")"
  DELTA_MS=$((NOW_MS - PREV_MS))
  DELTA_TICKS=$((TICKS - PREV_TICKS))
  if [ "$PREV_TICKS" -eq 0 ] || [ "$DELTA_MS" -le 0 ]; then
    CPU_PCT="0.0"
  else
    CPU_PCT="$(awk -v dt="$DELTA_TICKS" -v hz="$CLK_TCK" -v ms="$DELTA_MS"       'BEGIN { printf "%.2f", (dt * 100000.0) / (hz * ms) }')"
  fi

  RSS="$(sum_status_field VmRSS "$@")"
  HWM="$(sum_status_field VmHWM "$@")"
  PSS="$(sum_pss_kb "$@")"
  THREADS="$(sum_status_field Threads "$@")"
  FDS="$(sum_fd_count "$@")"
  READ_BYTES="$(sum_io_field read_bytes "$@")"
  WRITE_BYTES="$(sum_io_field write_bytes "$@")"
  VOL="$(sum_status_field voluntary_ctxt_switches "$@")"
  NONVOL="$(sum_status_field nonvoluntary_ctxt_switches "$@")"
  MEM_AVAIL="$(read_kb MemAvailable)"; MEM_AVAIL="${MEM_AVAIL:-0}"
  SWAP_FREE="$(read_kb SwapFree)"; SWAP_FREE="${SWAP_FREE:-0}"
  CPU_FREQ="$(average_cpu_freq_khz)"
  GPU_FREQ="$(gpu_freq_khz)"
  TEMP="$(max_temp_millic)"
  LOAD="$(load1)"
  ELAPSED="$(awk -v now="$NOW_MS" -v start="$START_MS"     'BEGIN { printf "%.3f", (now-start)/1000.0 }')"

  echo "$ELAPSED,$PID_COUNT,$CPU_PCT,$RSS,$HWM,$PSS,$THREADS,$FDS,$READ_BYTES,$WRITE_BYTES,$VOL,$NONVOL,$MEM_AVAIL,$SWAP_FREE,$CPU_FREQ,$GPU_FREQ,$TEMP,$LOAD" >> "$SAMPLES"

  PREV_MS="$NOW_MS"
  PREV_TICKS="$TICKS"
  sleep "$INTERVAL" 2>/dev/null || sleep 1
done

wait "$ROOT_PID"
EXIT_CODE=$?

cleanup
trap - EXIT INT TERM

END_MS="$(now_ms)"
DURATION="$(awk -v end="$END_MS" -v start="$START_MS"   'BEGIN { printf "%.3f", (end-start)/1000.0 }')"

awk -F, -v exit_code="$EXIT_CODE" -v duration="$DURATION" '
NR == 1 { next }
{
  n++
  cpu += $3
  if ($3 > cpu_max) cpu_max=$3
  if ($4 > rss_max) rss_max=$4
  if ($5 > hwm_max) hwm_max=$5
  if ($6 > pss_max) pss_max=$6
  if ($7 > threads_max) threads_max=$7
  if ($8 > fd_max) fd_max=$8
  if (n == 1 || $13 < mem_min) mem_min=$13
  if ($17 > temp_max) temp_max=$17
  if ($15 > 0) { cpu_freq += $15; cpu_freq_n++ }
  if ($16 > 0) { gpu_freq += $16; gpu_freq_n++ }
}
END {
  print "exit_code=" exit_code
  print "duration_seconds=" duration
  print "samples=" n
  if (n > 0) {
    printf "avg_cpu_pct=%.2f\n", cpu/n
    printf "max_cpu_pct=%.2f\n", cpu_max
    print "peak_rss_kb=" rss_max
    print "peak_hwm_kb=" hwm_max
    print "peak_pss_kb=" pss_max
    print "max_threads=" threads_max
    print "max_fd_count=" fd_max
    print "min_system_mem_available_kb=" mem_min
    print "max_temp_millic=" temp_max
  }
  if (cpu_freq_n) printf "avg_cpu_freq_khz=%.0f\n", cpu_freq/cpu_freq_n
  if (gpu_freq_n) printf "avg_gpu_freq_khz=%.0f\n", gpu_freq/gpu_freq_n
}' "$SAMPLES" > "$SUMMARY"

{
  echo "game_log=$GAME_LOG"
  echo "samples_csv=$SAMPLES"
  echo "device_info=$DEVICE"
  if [ -s "$PERF_LOG" ]; then echo "perf_stat=$PERF_LOG"; fi
} >> "$SUMMARY"

echo "[pmjs-profile] game exit code: $EXIT_CODE"
echo "[pmjs-profile] summary:"
cat "$SUMMARY"

exit "$EXIT_CODE"
