#!/usr/bin/env bash
# ==============================================================================
# Laya AI Gateway - Automated Regression Test Runner (via laya_client)
# ==============================================================================
# Fast, zero-dependency automated testing using laya_client and jq.
# Completely eliminates socat, raw byte packing, and socket timing hacks.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SUITE_FILE="${1:-/etc/laya/test_suite.json}"

if [ ! -f "$SUITE_FILE" ]; then
    SUITE_FILE="$REPO_ROOT/config/test_suite.json"
fi

if [ ! -f "$SUITE_FILE" ]; then
    echo "Error: Test suite JSON file not found at $SUITE_FILE"
    exit 2
fi

# Verify laya_client and jq are available
for tool in laya_client jq; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Error: Required tool '$tool' is not installed in PATH."
        exit 2
    fi
done

echo "=================================================================="
echo " Running Automated Regression Test Suite (via laya_client)"
echo " Suite File: $SUITE_FILE"
echo " Engine:     Native Laya Client CLI"
echo "=================================================================="

TOTAL_TESTS=$(jq '.test_cases | length' "$SUITE_FILE")
PASSED=0
FAILED=0

for i in $(seq 0 $((TOTAL_TESTS - 1))); do
    NAME=$(jq -r ".test_cases[$i].name" "$SUITE_FILE")
    PROMPT=$(jq -r ".test_cases[$i].prompt" "$SUITE_FILE")
    EXPECTED=$(jq -r ".test_cases[$i].expected_verdict" "$SUITE_FILE")

    # Call laya_client directly in quiet mode to get the verdict string
    VERDICT=$(laya_client -q "$PROMPT" 2>/dev/null || true)
    LATENCY=$(laya_client --field latency_ms "$PROMPT" 2>/dev/null || true)

    TEST_NUM=$((i + 1))
    if [ "$VERDICT" == "$EXPECTED" ]; then
        printf "[PASS] Test %2d: %-36s Expected: %-16s Got: %-16s (%s ms)\n" \
            "$TEST_NUM" "$NAME" "$EXPECTED" "$VERDICT" "$LATENCY"
        PASSED=$((PASSED + 1))
    else
        printf "[FAIL] Test %2d: %-36s Expected: %-16s Got: %-16s (%s ms)\n" \
            "$TEST_NUM" "$NAME" "$EXPECTED" "$VERDICT" "$LATENCY"
        FAILED=$((FAILED + 1))
    fi
done

echo "=================================================================="
echo " Results: $PASSED / $TOTAL_TESTS Passed. ($FAILED Failed)"
echo "=================================================================="

if [ "$FAILED" -eq 0 ]; then
    echo "Status: ALL AUTOMATED TESTS PASSED."
    exit 0
else
    echo "Status: SOME TESTS FAILED."
    exit 1
fi
