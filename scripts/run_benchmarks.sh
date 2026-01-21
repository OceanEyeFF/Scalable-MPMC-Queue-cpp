#!/bin/bash
#
# Benchmark runner script that runs each test configuration in a separate process.
# This avoids EBR thread-local state pollution between test runs.
# Includes retry logic for intermittent failures.
#
# Usage:
#   ./scripts/run_benchmarks.sh [options]
#
# Options:
#   -e, --exe PATH         Benchmark executable (default: build/release/benchmarks/lscq_benchmarks.exe)
#   -o, --output PATH      Output JSON file (default: benchmark_results.json)
#   -f, --filter PATTERN   Filter pattern (e.g., "LSCQ_Pair")
#   -t, --threads LIST     Thread counts (default: "1 2 4 8 12 16 24")
#   --min-time SECONDS     Minimum time (default: 1.0)
#   --timeout SECONDS      Timeout per test (default: 120)
#   --retries N            Max retries per test (default: 3)

set -e

# Default values
BENCHMARK_EXE="build/release/benchmarks/lscq_benchmarks.exe"
OUTPUT="benchmark_results.json"
FILTER=""
THREADS="1 2 4 8 12 16 24"
MIN_TIME="1.0"
TIMEOUT=120
MAX_RETRIES=3
BENCHMARKS="BM_NCQ_Pair BM_SCQ_Pair BM_SCQP_Pair BM_LSCQ_Pair BM_MSQueue_Pair BM_MutexQueue_Pair"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -e|--exe)
            BENCHMARK_EXE="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        -f|--filter)
            FILTER="$2"
            shift 2
            ;;
        -t|--threads)
            THREADS="$2"
            shift 2
            ;;
        --min-time)
            MIN_TIME="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --retries)
            MAX_RETRIES="$2"
            shift 2
            ;;
        --benchmarks)
            BENCHMARKS="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check executable exists
if [[ ! -f "$BENCHMARK_EXE" ]]; then
    echo "Error: Benchmark executable not found: $BENCHMARK_EXE"
    exit 1
fi

# Apply filter
if [[ -n "$FILTER" ]]; then
    FILTERED_BENCHMARKS=""
    for b in $BENCHMARKS; do
        if [[ "$b" == *"$FILTER"* ]]; then
            FILTERED_BENCHMARKS="$FILTERED_BENCHMARKS $b"
        fi
    done
    BENCHMARKS="$FILTERED_BENCHMARKS"
fi

echo "============================================================"
echo "Benchmark Runner with Process Isolation & Retry Logic"
echo "============================================================"
echo "Executable: $BENCHMARK_EXE"
echo "Output: $OUTPUT"
echo "Benchmarks:$BENCHMARKS"
echo "Thread counts: $THREADS"
echo "Min time: ${MIN_TIME}s"
echo "Timeout: ${TIMEOUT}s"
echo "Max retries: $MAX_RETRIES"
echo "============================================================"
echo

# Temporary directory for individual results
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

TOTAL=0
COMPLETED=0
FAILED=0

# Function to run a single benchmark with retries
run_with_retry() {
    local BENCH="$1"
    local T="$2"
    local TEMP_FILE="$3"

    for ((retry=1; retry<=MAX_RETRIES; retry++)); do
        if timeout "$TIMEOUT" "$BENCHMARK_EXE" \
            "--benchmark_filter=${BENCH}/real_time/threads:${T}\$" \
            "--benchmark_min_time=${MIN_TIME}s" \
            "--benchmark_format=json" \
            "--benchmark_repetitions=1" \
            > "$TEMP_FILE" 2>&1; then

            # Check if result has benchmarks
            if grep -q '"benchmarks"' "$TEMP_FILE" && grep -q '"name"' "$TEMP_FILE"; then
                return 0  # Success
            fi
        fi

        if [[ $retry -lt $MAX_RETRIES ]]; then
            echo -n "(retry $((retry+1))/$MAX_RETRIES) "
        fi
        rm -f "$TEMP_FILE"
    done
    return 1  # All retries failed
}

# Run each benchmark
for BENCH in $BENCHMARKS; do
    echo
    echo "[$BENCH]"
    for T in $THREADS; do
        TOTAL=$((TOTAL + 1))
        TEMP_FILE="$TEMP_DIR/${BENCH}_${T}.json"

        printf "  threads:%-3d ... " "$T"

        START_TIME=$(date +%s.%N 2>/dev/null || date +%s)

        if run_with_retry "$BENCH" "$T" "$TEMP_FILE"; then
            END_TIME=$(date +%s.%N 2>/dev/null || date +%s)
            ELAPSED=$(echo "$END_TIME - $START_TIME" | bc 2>/dev/null || echo "?")
            echo "OK (${ELAPSED}s)"
            COMPLETED=$((COMPLETED + 1))
        else
            END_TIME=$(date +%s.%N 2>/dev/null || date +%s)
            ELAPSED=$(echo "$END_TIME - $START_TIME" | bc 2>/dev/null || echo "?")
            echo "FAILED after $MAX_RETRIES attempts (${ELAPSED}s)"
            FAILED=$((FAILED + 1))
        fi
    done
done

echo
echo "============================================================"
echo "Merging results..."
echo "============================================================"

# Merge all JSON results using jq if available, otherwise simple merge
if command -v jq &> /dev/null; then
    TEMP_MERGED="$TEMP_DIR/merged.json"
    echo '{"benchmarks":[]}' > "$TEMP_MERGED"

    for f in "$TEMP_DIR"/*.json; do
        if [[ -f "$f" && "$f" != "$TEMP_MERGED" ]]; then
            jq -s '.[0].benchmarks += .[1].benchmarks | .[0]' "$TEMP_MERGED" "$f" > "$TEMP_DIR/tmp.json" 2>/dev/null
            mv "$TEMP_DIR/tmp.json" "$TEMP_MERGED"
        fi
    done

    jq --arg total "$TOTAL" --arg completed "$COMPLETED" --arg failed "$FAILED" \
       '.run_info = {total: ($total|tonumber), completed: ($completed|tonumber), failed: ($failed|tonumber)}' \
       "$TEMP_MERGED" > "$OUTPUT" 2>/dev/null
else
    # Simple merge without jq
    echo '{"benchmarks":[' > "$OUTPUT"
    FIRST=1
    for f in "$TEMP_DIR"/*.json; do
        if [[ -f "$f" ]]; then
            CONTENT=$(grep -oP '"benchmarks"\s*:\s*\[\K[^\]]*' "$f" 2>/dev/null || echo "")
            if [[ -n "$CONTENT" && "$CONTENT" != "null" ]]; then
                if [[ $FIRST -eq 0 ]]; then
                    echo "," >> "$OUTPUT"
                fi
                FIRST=0
                echo "$CONTENT" >> "$OUTPUT"
            fi
        fi
    done
    echo '],"run_info":{"total":'$TOTAL',"completed":'$COMPLETED',"failed":'$FAILED'}}' >> "$OUTPUT"
fi

echo
echo "============================================================"
echo "Summary"
echo "============================================================"
echo "Total tests: $TOTAL"
echo "Completed: $COMPLETED"
echo "Failed: $FAILED"
echo "Results saved to: $OUTPUT"
echo "============================================================"

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
