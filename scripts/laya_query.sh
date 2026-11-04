#!/usr/bin/env bash
# ==============================================================================
# Laya AI Gateway - Lightweight Bash Query Wrapper (wraps laya_client)
# ==============================================================================
# Sourced or executed directly:
#   source scripts/laya_query.sh && laya_query "prompt"
# ==============================================================================

laya_query() {
    laya_client "$@"
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    laya_client "$@"
fi
