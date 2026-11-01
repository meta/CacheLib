#!/bin/bash
# Build CacheLib with DTO, configure DSA work queues, and run cachebench
# pinned to socket 0 with DSA checksum offload and (optionally) DTO's
# transparent memcpy/memset interception.
#
# Usage: ./run_dsa_cachebench.sh [options]
#   --workload cdn|bigcache
#                       cdn      = synthetic CDN hit-ratio config (default)
#                       bigcache = production BigCache trace replay
#                                  (~/bigcache_trace_sea1c01_20250414_20250421.json,
#                                  scaled exactly as the offload study: 400GB Navy,
#                                  16GB parcel, 500k ops/thread, fiber scheduler)
#   --config PATH       base cachebench config (overrides the workload default)
#   --offload on|off|none
#                       on   = Navy data checksums on, computed by DSA
#                       off  = Navy data checksums on, computed in software
#                       none = base config's own checksum setting
#                              (cdn: no Navy section added; bigcache: checksums
#                              off = the study's no-checksum baseline A)
#   --intercept on|off  transparent DTO interception of large memcpy/memset.
#                       off (default) sets DTO_MIN_BYTES=1GB so only the explicit
#                       checksum-offload API uses DSA; on leaves DTO's own
#                       size threshold (32KB) in effect.
#   --fibers on|off     Navy fiber scheduler (NavyRequestScheduler, libaio,
#                       study settings: maxNumReads 1024 / maxNumWrites 1200 /
#                       qDepth 32). Default off; pass --fibers on to enable.
#   --nvm-size-mb N     Navy cache size (default: cdn 20480, bigcache 409600)
#   --num-ops N         override test_config.numOps (per stressor thread;
#                       bigcache default 500000 = the study's 12M-op replay)
#   --page-size 4k|2mb|1gb
#                       page size backing the DRAM cache (slabs + hash table,
#                       SysV hugetlb shm). The hugetlb pools must be reserved
#                       out-of-band (setup_dsa_bench.sh); 4k = default pages.
#   --devices "0 2 4 6" DSA device ids to enable (default "0 2 4 6")
#   --out DIR           output/work directory (default ./dsa_bench_out)
#   --skip-build        do not (re)build cachebench
#   --skip-dsa          do not reconfigure DSA work queues
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build-cachelib}"
PREFIX="$REPO/opt/cachelib"
DTO_DIR="${DTO_DIR:-$HOME/DTO}"

WORKLOAD=cdn
CONFIG=""
OFFLOAD=on
INTERCEPT=off
FIBERS=off
NVM_SIZE_MB=""
NUM_OPS=""
PAGE_SIZE=4k
DEVICES="0 2 4 6"
OUT="$PWD/dsa_bench_out"
SKIP_BUILD=0
SKIP_DSA=0

while [ $# -gt 0 ]; do
  case "$1" in
    --workload)    WORKLOAD="$2"; shift 2 ;;
    --config)      CONFIG="$2"; shift 2 ;;
    --offload)     OFFLOAD="$2"; shift 2 ;;
    --intercept)   INTERCEPT="$2"; shift 2 ;;
    --fibers)      FIBERS="$2"; shift 2 ;;
    --nvm-size-mb) NVM_SIZE_MB="$2"; shift 2 ;;
    --num-ops)     NUM_OPS="$2"; shift 2 ;;
    --page-size)   PAGE_SIZE="$2"; shift 2 ;;
    --devices)     DEVICES="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    --skip-build)  SKIP_BUILD=1; shift ;;
    --skip-dsa)    SKIP_DSA=1; shift ;;
    -h|--help)     sed -n '2,33p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

case "$WORKLOAD" in
  cdn)
    [ -n "$CONFIG" ] || CONFIG="$REPO/cachelib/cachebench/test_configs/hit_ratio/cdn/config.json"
    [ -n "$NVM_SIZE_MB" ] || NVM_SIZE_MB=20480
    ;;
  bigcache)
    [ -n "$CONFIG" ] || CONFIG="$HOME/bigcache_trace_sea1c01_20250414_20250421.json"
    [ -n "$NVM_SIZE_MB" ] || NVM_SIZE_MB=409600
    [ -n "$NUM_OPS" ] || NUM_OPS=500000
    ;;
  *) echo "--workload must be cdn|bigcache" >&2; exit 1 ;;
esac
case "$OFFLOAD" in on|off|none) ;; *) echo "--offload must be on|off|none" >&2; exit 1 ;; esac
case "$INTERCEPT" in on|off) ;; *) echo "--intercept must be on|off" >&2; exit 1 ;; esac
case "$FIBERS" in on|off) ;; *) echo "--fibers must be on|off" >&2; exit 1 ;; esac
mkdir -p "$OUT"

export LD_LIBRARY_PATH="${DTO_LIB_DIR:+$DTO_LIB_DIR:}$PREFIX/lib:$PREFIX/lib64:/usr/local/lib:${LD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------- build ----
if [ "$SKIP_BUILD" = 0 ]; then
  echo "== Building cachebench (BUILD_WITH_DTO=ON) =="
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    mkdir -p "$BUILD_DIR"
    export CMAKE_PREFIX_PATH="$PREFIX/lib/cmake:$PREFIX:/usr/local${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    # CMAKE_DISABLE_FIND_PACKAGE_uring: cachelib enables its io_uring path
    # whenever liburing exists on the system, but that requires a folly
    # built with liburing; disable the probe so the two cannot disagree.
    cmake -S "$REPO/cachelib" -B "$BUILD_DIR" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DBUILD_TESTS=ON \
      -DBUILD_WITH_DTO=ON \
      -DCMAKE_DISABLE_FIND_PACKAGE_uring=ON
  fi
  cmake --build "$BUILD_DIR" --target cachebench -j "$(nproc)"
fi
CACHEBENCH="$BUILD_DIR/cachebench/cachebench"
[ -x "$CACHEBENCH" ] || { echo "cachebench not found at $CACHEBENCH" >&2; exit 1; }

# ------------------------------------------------------------ DSA setup ----
if [ "$SKIP_DSA" = 0 ]; then
  echo "== Configuring DSA shared work queues (devices: $DEVICES) =="
  for d in $DEVICES; do
    sudo "$DTO_DIR/accelConfig.sh" "$d" yes 0 4 >/dev/null
  done
  sudo chmod 0666 /dev/dsa/wq*.0
fi
# only the offload arm needs a device; software arms run anywhere
if [ "$OFFLOAD" = on ]; then
  ls /dev/dsa/wq*.0 >/dev/null 2>&1 || { echo "no enabled DSA WQs under /dev/dsa" >&2; exit 1; }
  echo "DSA WQs: $(ls /dev/dsa/)"
fi

# ------------------------------------------------------- derive config -----
case "$PAGE_SIZE" in
  4k)  HUGE_BYTES=0 ;;
  2mb) HUGE_BYTES=2097152
       free=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
       [ "$free" -ge 4600 ] || { echo "need >=4600 free 2MB pages (have $free); reserve with setup_dsa_bench.sh" >&2; exit 1; } ;;
  1gb) HUGE_BYTES=1073741824
       free=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages)
       [ "$free" -ge 26 ] || { echo "need >=26 free 1GB pages (have $free); reserve with setup_dsa_bench.sh" >&2; exit 1; } ;;
  *) echo "bad --page-size '$PAGE_SIZE' (4k|2mb|1gb)" >&2; exit 1 ;;
esac
export HUGE_BYTES

# SysV shm metadata dir; recreated fresh for every run
SHM_CACHE_DIR="$OUT/shm_meta_${WORKLOAD}_page_${PAGE_SIZE}"
export SHM_CACHE_DIR

RUN_CONFIG="$OUT/config_${WORKLOAD}_offload_${OFFLOAD}_page_${PAGE_SIZE}.json"
CONFIG="$CONFIG" RUN_CONFIG="$RUN_CONFIG" OFFLOAD="$OFFLOAD" \
NVM_SIZE_MB="$NVM_SIZE_MB" NUM_OPS="$NUM_OPS" OUT="$OUT" FIBERS="$FIBERS" \
HUGE_BYTES="$HUGE_BYTES" \
python3 - <<'EOF'
import json, os, shutil, sys
cfgPath = os.environ["CONFIG"]
cfg = json.load(open(cfgPath))
base = os.path.dirname(os.path.abspath(cfgPath))
tc = cfg["test_config"]
cc = cfg["cache_config"]
offload = os.environ["OFFLOAD"]
isTraceReplay = "replay" in tc.get("generator", "")

# cachebench resolves distribution files by prepending the config file's
# directory (even to absolute paths), so copy them next to the derived
# config and reference them by basename.
for k in ("popDistFile", "valSizeDistFile"):
    if k in tc:
        src = tc[k] if os.path.isabs(tc[k]) else os.path.join(base, tc[k])
        dst = os.path.join(os.environ["OUT"], os.path.basename(src))
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copyfile(src, dst)
        tc[k] = os.path.basename(src)
# the (multi-GB) trace file is opened directly: absolutize, don't copy
if "traceFileName" in tc and not os.path.isabs(tc["traceFileName"]):
    tc["traceFileName"] = os.path.join(base, tc["traceFileName"])
if "traceFileName" in tc and not os.path.exists(tc["traceFileName"]):
    sys.exit(f"trace file not found: {tc['traceFileName']}")

if os.environ["NUM_OPS"]:
    tc["numOps"] = int(os.environ["NUM_OPS"])

if isTraceReplay:
    # Trace replay (BigCache): the base config already has a Navy section
    # sized for its original production host; rescale it to this machine
    # exactly as the offload study did.
    cc["nvmCacheSizeMB"] = int(os.environ["NVM_SIZE_MB"])
    cc["nvmCachePaths"] = [os.path.join(os.environ["OUT"], "navy_cache_file")]
    cc["navyParcelMemoryMB"] = 16384
    if offload != "none":
        cc["navyDataChecksum"] = True
        cc["navyChecksumOffload"] = offload == "on"
        # only a default: a base config that sets the gate (a size sweep) wins
        cc.setdefault("navyChecksumOffloadMinSize", 16384)
    else:
        # "no checksum" arm: override whatever the base config says, so a
        # config saved from an offload run does not silently keep checksums
        # (and a DSA self-check) on
        cc["navyDataChecksum"] = False
        cc["navyChecksumOffload"] = False
        cc["navyBlockCacheFlushCopyOffload"] = False
elif offload != "none":
    # Synthetic workloads (CDN): the base config is DRAM-only; add the
    # hybrid Navy tier the offload applies to.
    cc["nvmCacheSizeMB"] = int(os.environ["NVM_SIZE_MB"])
    cc["nvmCachePaths"] = [os.path.join(os.environ["OUT"], "navy_cache_file")]
    # defaults only: a base config may set these (the fiber scheduler needs
    # maxNumReads 1024 / maxNumWrites 1200 to divide evenly, so 32 writers
    # is invalid with --fibers on; use 30 or 40)
    cc.setdefault("navyReaderThreads", 32)
    cc.setdefault("navyWriterThreads", 32)
    cc["navyDataChecksum"] = True
    cc["navyChecksumOffload"] = offload == "on"
    cc.setdefault("navyChecksumOffloadMinSize", 16384)

huge = int(os.environ.get("HUGE_BYTES", "0"))
# DRAM cache (slabs + hash table) on SysV shm so the page-size knob actually
# applies to the cache; hugePageSize 0 = normal 4K pages. Without shmType +
# cacheDir the allocator silently uses heap memory and ignores hugePageSize.
cc["shmType"] = "sysv"
cc["cacheDir"] = os.environ["SHM_CACHE_DIR"]
if huge:
    cc["hugePageSize"] = huge

if os.environ["FIBERS"] == "on":
    # NavyRequestScheduler (fibers, libaio) with the study's settings;
    # required for the offload's yield-during-DSA-wait to overlap work.
    cc["navyMaxNumReads"] = 1024
    cc["navyMaxNumWrites"] = 1200
    cc["navyQDepth"] = 32
    cc["navyEnableIoUring"] = False

# Print the navy counter map at the end of the run: the offload/fallback/wait
# counters are registered in RegionManager::getCounters but only reach the log
# with this on. A run that cannot be audited from its log is not a measurement.
cc["printNvmCounters"] = True

json.dump(cfg, open(os.environ["RUN_CONFIG"], "w"), indent=2)
print(f"wrote {os.environ['RUN_CONFIG']}")
EOF

# ---------------------------------------------------------------- run ------
# navy with 128 reader + 300 writer threads needs far more fds than the
# usual 1024 soft limit
ulimit -n 65536 2>/dev/null || true

export DTO_USESTDC_CALLS=0
export DTO_CRC_MIN_BYTES=4096
# DTO's per-call statistics (the "Number of Memory Operations" table) cost two
# clock_gettime calls and bookkeeping on EVERY interposed memcpy/memmove in the
# process - cachebench's startup parallel key sort alone went from 1 s to
# 13-138 s of CPU with them on. They are off unless asked for; the harness
# reports the offload counters from Navy instead.
export DTO_COLLECT_STATS="${DTO_COLLECT_STATS:-0}"
# Keep freed memory mapped. Navy populates its region and flush buffers
# itself (RegionManager), which is what keeps a DSA using shared virtual
# addressing from stalling on IOMMU page requests; these tunables remove the
# remaining allocator churn (a few hundred thousand first-touch page requests
# per run from glibc unmapping and re-obtaining large chunks) and also trim
# the software arm by ~7%, so they are set for every arm. jemalloc users: the
# equivalent is dirty_decay_ms:-1,muzzy_decay_ms:-1.
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.malloc.mmap_threshold=33554432:glibc.malloc.trim_threshold=4294967295:glibc.malloc.top_pad=67108864}"
# DTO_WAIT_METHOD only matters where DTO itself waits on a descriptor: the
# preloaded libdto.so of the --intercept arm, and dto_async_wait(), which the
# checksum offload calls only when dto_wait_blocks() says the method parks the
# thread (never with DTO 1.2). The offload's own poll loops yield to other
# fibers or pause regardless of this setting.
export DTO_WAIT_METHOD="${DTO_WAIT_METHOD:-busypoll}"
# cachelib links libdto_explicit, the DTO API without the libc interposers, so
# the checksum offload never intercepts the process's own memcpy/memset. The
# --intercept arm adds that on top by preloading libdto.so (the interposing
# variant of the same library) into the cachebench process.
if [ "$INTERCEPT" = off ]; then
  export DTO_MIN_BYTES=1073741824   # 1GB: belt and braces, nothing intercepts
else
  unset DTO_MIN_BYTES               # DTO's own threshold decides
  LIBDTO=""
  for d in ${DTO_LIB_DIR:+"$DTO_LIB_DIR"} "$BUILD_DIR/_deps/dto-build" \
           "$PREFIX/lib" "$PREFIX/lib64" /usr/local/lib /usr/local/lib64; do
    if [ -f "$d/libdto.so" ]; then LIBDTO="$d/libdto.so"; break; fi
  done
  [ -n "$LIBDTO" ] || { echo "--intercept on needs libdto.so; set DTO_LIB_DIR" >&2; exit 1; }
  echo "   transparent interception: LD_PRELOAD=$LIBDTO"
  export LD_PRELOAD="$LIBDTO${LD_PRELOAD:+:$LD_PRELOAD}"
fi

LOG="$OUT/cachebench_${WORKLOAD}_offload_${OFFLOAD}_intercept_${INTERCEPT}_page_${PAGE_SIZE}.log"
echo "== Running cachebench on socket 0 (workload=$WORKLOAD offload=$OFFLOAD intercept=$INTERCEPT fibers=$FIBERS) =="
echo "   log: $LOG"

# The fiber scheduler has a known ~1-in-4 startup hang (frozen at
# NavyRequestDispatcher startup). With --progress 60 a healthy run writes to
# its log at least once a minute, so prolonged log silence while the process
# lives means hung: kill and retry (up to 3 attempts). A cold trace preload
# is silent too, so the threshold is configurable (WATCHDOG_SILENCE seconds).
WATCHDOG_SILENCE="${WATCHDOG_SILENCE:-330}"
attempt_run() {
  rm -f "$OUT/navy_cache_file"
  # PREFILL_NAVY=1: populate the Navy file before the run (tmpfs stores: a
  # fresh file is 100 GB of first-touch page allocation inside the measured
  # window, which stalls flushes and inflates insert latency for every arm)
  if [ -n "${PREFILL_NAVY:-}" ]; then
    dd if=/dev/zero of="$OUT/navy_cache_file" bs=1M count="$NVM_SIZE_MB" status=none
  fi
  rm -rf "$SHM_CACHE_DIR"
  # reap orphaned SysV segments (>100MB, ours) from earlier killed runs;
  # a clean cachelib exit persists them for warm restart, which would pin
  # the hugetlb pool across runs
  ipcs -m 2>/dev/null | awk -v u="$USER" '$3==u && $5+0>100000000 {print $2}' | while read -r id; do ipcrm -m "$id" 2>/dev/null || true; done
  /usr/bin/time -v numactl -N 0 -m 0 \
    "$CACHEBENCH" --json_test_config "$RUN_CONFIG" --progress 60 --report_api_latency=true \
    > "$LOG" 2>&1 &
  local tpid=$!
  local last=-1 silent=0
  while kill -0 "$tpid" 2>/dev/null; do
    sleep 30
    local sz
    sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
    if [ "$sz" = "$last" ]; then
      silent=$((silent + 1))
      if [ "$silent" -ge $((WATCHDOG_SILENCE / 30)) ]; then
        pkill -9 -P "$tpid" 2>/dev/null
        kill -9 "$tpid" 2>/dev/null
        wait "$tpid" 2>/dev/null
        return 99
      fi
    else
      silent=0
    fi
    last=$sz
  done
  wait "$tpid"
}

set +e
rc=99
WATCHDOG_RETRIES="${WATCHDOG_RETRIES:-6}"
for attempt in $(seq 1 "$WATCHDOG_RETRIES"); do
  attempt_run
  rc=$?
  [ "$rc" -ne 99 ] && break
  echo "WATCHDOG: run frozen for ${WATCHDOG_SILENCE}s+ (startup hang?), retrying (attempt $attempt of $WATCHDOG_RETRIES)"
done
set -e
rm -f "$OUT/navy_cache_file"
rm -rf "$SHM_CACHE_DIR"
ipcs -m 2>/dev/null | awk -v u="$USER" '$3==u && $5+0>100000000 {print $2}' | while read -r id; do ipcrm -m "$id" 2>/dev/null || true; done

# ------------------------------------------------------------- report ------
echo "exit=$rc"
grep -E "Total Ops|get   |set   |NVM Gets|NVM Puts" "$LOG" | head -6 || true
# exclude the printed counter map: its key names (navy_*_checksum_errors : 0)
# would otherwise count as errors
echo "checksum error lines: $(grep -viE '^navy_' "$LOG" | grep -ciE 'checksum.*(error|mismatch)' || true)"
echo "checksum error counters: $(grep -iE '^navy_.*checksum.*error' "$LOG" | sed 's/  :  /=/' | tr '\n' ' ')"
echo "-- DSA offload counters (device fallbacks must be 0 for an offload run) --"
grep -E "^navy_bc_(csum|copyout|flush_copy)_|^navy_hugetlb_arena_misses" "$LOG" | sed 's/  :  / = /' || true
grep -E "User time|System time|Elapsed \(wall|Maximum resident" "$LOG" || true
if grep -q "Number of Memory Operations" "$LOG"; then
  echo "-- DTO op counts (see full table in the log) --"
  grep -A20 "Number of Memory Operations" "$LOG" | head -24
fi
exit $rc
