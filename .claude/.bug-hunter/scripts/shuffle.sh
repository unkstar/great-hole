#!/usr/bin/env bash
#
# shuffle.sh — Shuffle lines or diff file-blocks from stdin.
#
# Usage:
#   shuffle.sh lines      [-s SEED]   # Shuffle lines from stdin
#   shuffle.sh diff-files [-s SEED]   # Shuffle per-file blocks in a unified diff
#
# Options:
#   -s SEED   Random seed (integer). Same seed = same output order.
#
# Dependencies: prefers shuf (Linux) or gshuf (macOS/Homebrew).
#               Falls back to a pure-awk Fisher-Yates shuffle.

set -euo pipefail

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------
SHUF_CMD=""
SEED=""
CLEANUP_FILES=()
CLEANUP_DIRS=()

cleanup() {
    for f in "${CLEANUP_FILES[@]}"; do
        rm -f "$f" 2>/dev/null || true
    done
    for d in "${CLEANUP_DIRS[@]}"; do
        rm -rf "$d" 2>/dev/null || true
    done
}
trap cleanup EXIT

make_tmpfile() {
    local f
    f=$(mktemp)
    CLEANUP_FILES+=("$f")
    echo "$f"
}

make_tmpdir() {
    local d
    d=$(mktemp -d)
    CLEANUP_DIRS+=("$d")
    echo "$d"
}

# ---------------------------------------------------------------------------
# Detect a working shuffle command
# ---------------------------------------------------------------------------
detect_shuf() {
    if command -v shuf >/dev/null 2>&1; then
        SHUF_CMD="shuf"
    elif command -v gshuf >/dev/null 2>&1; then
        SHUF_CMD="gshuf"
    fi
}

# ---------------------------------------------------------------------------
# Shuffle lines in a file using shuf/gshuf
# ---------------------------------------------------------------------------
shuf_lines() {
    local input_file="$1"
    if [[ -n "$SEED" ]]; then
        # Disperse the seed to avoid collisions for close seed values (e.g., 1 vs 2)
        "$SHUF_CMD" --random-source=<(awk -v seed="$SEED" 'BEGIN{srand(seed * 1000003 + 7); while(1) printf "%c", int(rand()*256)}') "$input_file"
    else
        "$SHUF_CMD" "$input_file"
    fi
}

# ---------------------------------------------------------------------------
# Pure-awk Fisher-Yates shuffle (POSIX fallback)
# ---------------------------------------------------------------------------
awk_shuffle_lines() {
    local input_file="$1"
    local seed="${SEED:-$RANDOM}"
    awk -v seed="$seed" '
    BEGIN { srand(seed * 1000003 + 7) }
    { lines[NR] = $0 }
    END {
        n = NR
        for (i = n; i > 1; i--) {
            j = int(rand() * i) + 1
            tmp = lines[i]
            lines[i] = lines[j]
            lines[j] = tmp
        }
        for (i = 1; i <= n; i++) print lines[i]
    }
    ' "$input_file"
}

# ---------------------------------------------------------------------------
# Shuffle lines from a file (core helper used by both subcommands)
#
# When a seed is provided, always use awk Fisher-Yates for reliable per-seed
# differentiation. shuf --random-source has poor sensitivity to small seed
# changes with few items. Use shuf only for unseeded (pure random) shuffles.
# ---------------------------------------------------------------------------
shuffle_file() {
    local input_file="$1"
    if [[ -n "$SEED" ]]; then
        awk_shuffle_lines "$input_file"
    elif [[ -n "$SHUF_CMD" ]]; then
        shuf_lines "$input_file"
    else
        awk_shuffle_lines "$input_file"
    fi
}

# ---------------------------------------------------------------------------
# Subcommand: lines
# ---------------------------------------------------------------------------
cmd_lines() {
    local tmpfile
    tmpfile=$(make_tmpfile)

    cat > "$tmpfile"

    # Empty input → nothing to output
    if [[ ! -s "$tmpfile" ]]; then
        return
    fi

    shuffle_file "$tmpfile"
}

# ---------------------------------------------------------------------------
# Subcommand: diff-files
#
# Splits a unified diff into per-file blocks (delimited by ^diff --git)
# and shuffles the block order. Hunks within each block stay in original order.
# ---------------------------------------------------------------------------
cmd_diff_files() {
    local tmpfile
    tmpfile=$(make_tmpfile)

    cat > "$tmpfile"

    if [[ ! -s "$tmpfile" ]]; then
        return
    fi

    # Split into blocks using awk. Each block starts with "diff --git".
    local block_dir
    block_dir=$(make_tmpdir)

    awk -v dir="$block_dir" '
    BEGIN { block = 0; file = "" }
    /^diff --git / {
        block++
        file = sprintf("%s/block_%04d", dir, block)
    }
    {
        if (file != "") print >> file
    }
    ' "$tmpfile"

    local block_count
    block_count=$(find "$block_dir" -name 'block_*' 2>/dev/null | wc -l)
    block_count=$((block_count + 0))  # trim whitespace

    if [[ "$block_count" -eq 0 ]]; then
        # No "diff --git" lines found — output as-is
        cat "$tmpfile"
        return
    fi

    # Generate indices 1..N, write to a temp file, shuffle it
    local idx_file
    idx_file=$(make_tmpfile)
    seq 1 "$block_count" > "$idx_file"

    local shuffled_indices
    shuffled_indices=$(shuffle_file "$idx_file")

    # Output blocks in shuffled order
    while IFS= read -r idx; do
        local block_file
        block_file=$(printf "%s/block_%04d" "$block_dir" "$idx")
        if [[ -f "$block_file" ]]; then
            cat "$block_file"
        fi
    done <<< "$shuffled_indices"
}

# ---------------------------------------------------------------------------
# Main: parse arguments
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 {lines|diff-files} [-s SEED]" >&2
    exit 1
}

if [[ $# -lt 1 ]]; then
    usage
fi

SUBCMD="$1"
shift

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s)
            if [[ $# -lt 2 ]]; then
                echo "Error: -s requires a SEED value" >&2
                exit 1
            fi
            SEED="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

detect_shuf

case "$SUBCMD" in
    lines)
        cmd_lines
        ;;
    diff-files)
        cmd_diff_files
        ;;
    *)
        echo "Unknown subcommand: $SUBCMD" >&2
        usage
        ;;
esac
