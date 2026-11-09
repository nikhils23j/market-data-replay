#!/usr/bin/env bash
# Full evaluation: clean Release build, tests, fuzzing, demo replays, benchmark,
# ASan+UBSan and TSan runs. Logs go to build/eval/, summary to results.json.
# The recorded run took 113 s on an M2 laptop (see eval_wall_seconds in results.json).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
LOG=build/eval
JOBS=${JOBS:-8}
FUZZ_SEEDS=${FUZZ_SEEDS:-8}
FUZZ_EVENTS=${FUZZ_EVENTS:-400000}
MESSAGES=${MESSAGES:-3000000}
REPS=${REPS:-5}
PACE_REPS=${PACE_REPS:-3}

rm -rf build
mkdir -p "$LOG"
load() { sysctl -n vm.loadavg | tr -d '{}' | awk '{print $1}'; }
LOAD_START=$(load)
T0=$(date +%s)
step() { echo; echo "== $*"; }

step "configure + build (Release)"
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release > "$LOG/configure.log"
cmake --build build/release -j "$JOBS" 2>&1 | tee "$LOG/build.log" | grep -E "warning|error|Built target" || true
FLAGS=$(grep -E '^CMAKE_CXX_FLAGS_RELEASE:' build/release/CMakeCache.txt | cut -d= -f2)

step "unit + smoke tests (ctest)"
ctest --test-dir build/release --output-on-failure 2>&1 | tee "$LOG/ctest.log" | tail -4
UNIT=$(./build/release/mdr_tests | tee "$LOG/unit.log")
echo "$UNIT"

step "differential fuzz (Release)"
FUZZ_CMD="mdr_fuzz --seeds $FUZZ_SEEDS --events $FUZZ_EVENTS --sessions 40 --session-messages 20000"
./build/release/$FUZZ_CMD | tee "$LOG/fuzz.log"

step "generate session"
GEN_CMD="mdr_gen --out build/data --messages $MESSAGES --seed 42"
./build/release/$GEN_CMD | tee "$LOG/gen.log"

step "replay demos"
./build/release/mdr_replay --dir build/data | tee "$LOG/replay_ab.log"
./build/release/mdr_replay --dir build/data --threads 2 | tee "$LOG/replay_ab_2t.log"
./build/release/mdr_replay --dir build/data --lines a | tee "$LOG/replay_a_only.log"
./build/release/mdr_replay --dir build/data --no-snapshots | tee "$LOG/replay_no_snapshots.log" || echo "(digest mismatch expected without snapshots)"

step "benchmark"
BENCH_CMD="mdr_bench --dir build/data --reps $REPS --pace-reps $PACE_REPS --pace 1000000 --json build/eval/bench.json"
LOAD_BENCH=$(load)
./build/release/$BENCH_CMD | tee "$LOG/bench.log"

step "ASan + UBSan"
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMDR_SANITIZE=address,undefined > "$LOG/asan_configure.log"
cmake --build build/asan -j "$JOBS" > "$LOG/asan_build.log" 2>&1
ASAN_UNIT=$(./build/asan/mdr_tests | tee "$LOG/asan_unit.log")
echo "$ASAN_UNIT"
ASAN_FUZZ_CMD="mdr_fuzz --seeds 4 --events 50000 --sessions 6 --session-messages 10000"
./build/asan/$ASAN_FUZZ_CMD | tee "$LOG/asan_fuzz.log"

step "TSan"
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DMDR_SANITIZE=thread > "$LOG/tsan_configure.log"
cmake --build build/tsan -j "$JOBS" --target mdr_tests mdr_gen mdr_replay mdr_fuzz > "$LOG/tsan_build.log" 2>&1
TSAN_UNIT=$(./build/tsan/mdr_tests | tee "$LOG/tsan_unit.log")
echo "$TSAN_UNIT"
./build/tsan/mdr_gen --out build/tsan/data --messages 200000 --seed 3 > "$LOG/tsan_gen.log"
./build/tsan/mdr_replay --dir build/tsan/data --threads 2 | tee "$LOG/tsan_replay.log"
./build/tsan/mdr_fuzz --seeds 1 --events 1000 --sessions 6 --session-messages 5000 | tee "$LOG/tsan_fuzz.log"

T1=$(date +%s)
LOAD_END=$(load)
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | awk 'BEGIN{ORS="\\n"} {print}' | sed 's/\\n$//'; }
{
  echo "{"
  echo "  \"date\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo "  \"environment\": {"
  echo "    \"cpu\": \"$(sysctl -n machdep.cpu.brand_string)\","
  echo "    \"model\": \"$(sysctl -n hw.model)\","
  echo "    \"logical_cpus\": $(sysctl -n hw.ncpu),"
  echo "    \"perf_cores\": $(sysctl -n hw.perflevel0.physicalcpu), \"efficiency_cores\": $(sysctl -n hw.perflevel1.physicalcpu),"
  echo "    \"memory_bytes\": $(sysctl -n hw.memsize),"
  echo "    \"cache_line_bytes\": $(sysctl -n hw.cachelinesize),"
  echo "    \"os\": \"$(sw_vers -productName) $(sw_vers -productVersion) ($(uname -r))\","
  echo "    \"compiler\": \"$(esc "$(c++ --version | head -1)")\","
  echo "    \"cmake\": \"$(cmake --version | head -1)\","
  echo "    \"release_flags\": \"-std=c++20 $FLAGS\","
  echo "    \"sanitizer_flags\": \"CMake Debug (-g, no optimisation) + -fsanitize=<address,undefined | thread> -fno-omit-frame-pointer -fno-sanitize-recover=all\","
  echo "    \"load_avg_1m_at_start\": $LOAD_START, \"load_avg_1m_before_bench\": $LOAD_BENCH, \"load_avg_1m_at_end\": $LOAD_END,"
  echo "    \"note\": \"macOS cannot pin threads; both pipeline threads request QOS_CLASS_USER_INTERACTIVE\""
  echo "  },"
  echo "  \"commands\": {"
  echo "    \"build\": \"cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j $JOBS\","
  echo "    \"tests\": \"ctest --test-dir build/release --output-on-failure\","
  echo "    \"fuzz\": \"build/release/$FUZZ_CMD\","
  echo "    \"generate\": \"build/release/$GEN_CMD\","
  echo "    \"bench\": \"build/release/$BENCH_CMD\","
  echo "    \"asan_ubsan\": \"build/asan/mdr_tests && build/asan/$ASAN_FUZZ_CMD\","
  echo "    \"tsan\": \"build/tsan/mdr_tests && build/tsan/mdr_replay --dir build/tsan/data --threads 2 && build/tsan/mdr_fuzz --seeds 1 --events 1000 --sessions 6 --session-messages 5000\""
  echo "  },"
  echo "  \"unit_tests\": \"$UNIT\","
  echo "  \"fuzz\": \"$(esc "$(grep -v PASS "$LOG/fuzz.log")")\","
  echo "  \"asan_ubsan\": \"$ASAN_UNIT; $(esc "$(grep -v PASS "$LOG/asan_fuzz.log")")\","
  echo "  \"tsan\": \"$TSAN_UNIT; $(esc "$(tail -1 "$LOG/tsan_replay.log")"); $(esc "$(tail -2 "$LOG/tsan_fuzz.log" | head -1)")\","
  echo "  \"generator\": {"
  sed 's/^\(.*\)=\(.*\)$/    "\1": "\2",/' build/data/truth.txt | sed '$ s/,$//'
  echo "  },"
  echo "  \"replay_demos\": {"
  echo "    \"ab_1thread\": \"$(esc "$(tail -1 "$LOG/replay_ab.log")")\","
  echo "    \"ab_2thread\": \"$(esc "$(tail -1 "$LOG/replay_ab_2t.log")")\","
  echo "    \"line_a_only\": \"$(esc "$(grep -E 'gaps_declared' "$LOG/replay_a_only.log")"); $(esc "$(tail -1 "$LOG/replay_a_only.log")")\","
  echo "    \"no_snapshots\": \"$(esc "$(grep -E 'msgs:' "$LOG/replay_no_snapshots.log")"); $(esc "$(grep -E 'apply_errors' "$LOG/replay_no_snapshots.log")"); $(esc "$(tail -1 "$LOG/replay_no_snapshots.log")")\""
  echo "  },"
  echo "  \"bench\": $(sed 's/^/  /' build/eval/bench.json | sed '1s/^  //'),"
  echo "  \"eval_wall_seconds\": $((T1 - T0))"
  echo "}"
} > results.json
python3 -c "import json;json.load(open('results.json'))" 2>/dev/null && echo "results.json is valid JSON" || echo "(python3 not available or JSON invalid; check results.json)"
echo "eval finished in $((T1 - T0)) s; load avg start=$LOAD_START bench=$LOAD_BENCH end=$LOAD_END"
