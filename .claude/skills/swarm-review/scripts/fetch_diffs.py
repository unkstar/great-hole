#!/usr/bin/env python3
"""
Swarm Diff Fetcher — fetch all file diffs for a review via Swarm web diff endpoint.

Usage:
  python3 fetch_diffs.py <review_id> --user USER --ticket TICKET [--output FILE]
  python3 fetch_diffs.py <review_id> --user USER --ticket TICKET --file //depot/path/file.cpp

Auth: uses env vars SWARM_USER and SWARM_TICKET, or --user/--ticket args

How it works:
  1. Fetches review metadata from /api/v10/reviews/<id>
     - Determines if the review has committed changes, a pending shelved change, or both
  2. Fetches file list from /api/v10/reviews/<id>/files
  3. For each file, fetches diffs in two parts:
     a. Committed changes: left=file#(cur_rev - num_committed_changes), right=file#cur_rev
        (skipped if no committed changes, or if only 1 committed change → use cur_rev-1)
     b. Pending shelved change: left=file#cur_rev, right=file@=<pending_change_number>
        (only when versions list contains a pending=True entry)
  4. Parses HTML diff output into line-numbered rows

Output format per file:
  === filename.cpp (committed rev N-k → N  [+ pending @CHANGE]) ===
  --- Committed changes ---
  L  42   +    added line
  L  41  -    deleted line
  --- Pending shelved changes ---
  L  50   +    new line in shelve
"""

import sys
import os
import re
import json
import html as html_mod
import urllib.request
import urllib.error
import urllib.parse
import base64
import argparse


BASE_URL = "https://timi-swarm.ces.qq.com"


def get_headers(user, ticket):
    b64 = base64.b64encode(f"{user}:{ticket}".encode()).decode()
    return {"Authorization": f"Basic {b64}"}


def api_get(url, user, ticket):
    req = urllib.request.Request(url, headers=get_headers(user, ticket))
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}: {e.read().decode()[:200]}"}
    except Exception as e:
        return {"error": str(e)}


def fetch_review_info(base_url, review_id, user, ticket):
    """
    Returns (committed_changes, pending_change_number, versions).
    - committed_changes: list of committed change numbers (may be empty)
    - pending_change_number: int if there is a pending shelved change, else None
    - versions: raw versions list from API

    Logic:
    - versions[i].pending=True  → that version is an un-submitted shelve
    - versions[i].pending=False → that version is a committed change
    - The review's 'changes' array lists all associated committed changes
    """
    url = f"{base_url}/api/v10/reviews/{review_id}"
    data = api_get(url, user, ticket)
    if data.get("error"):
        raise RuntimeError(f"fetch_review_info: {data['error']}")
    reviews = data.get("data", {}).get("reviews", [])
    if not reviews:
        raise RuntimeError("fetch_review_info: no reviews returned")
    review = reviews[0]

    versions = review.get("versions", [])

    # Find committed and pending versions by inspecting the versions list directly.
    # review.changes[] can include historical/related changes and must NOT be used
    # to count committed rounds — only the versions list is authoritative.
    #
    # Each version entry has pending=True (shelved, not submitted) or pending=False (committed).
    # Swarm UI shows:
    #   - pending-only review  → shelved@change vs head rev  (no committed diff section)
    #   - committed-only       → rev(cur-N) → rev(cur)
    #   - mixed                → both sections
    committed_versions = [v for v in versions if not v.get("pending", False)]
    pending_versions   = [v for v in versions if v.get("pending", False)]

    # Take the LAST pending version — versions are listed oldest-first, so [-1] is the latest shelve.
    # e.g. if versions=[{change:2029113, pending:True}, {change:2029112, pending:True}],
    # we want 2029112 (the author's most recent reshelve), not 2029113 (stale first shelve).
    pending_change = pending_versions[-1].get("change") if pending_versions else None
    committed_changes = [v.get("change") for v in committed_versions if v.get("change")]

    return committed_changes, pending_change, versions


def fetch_file_list(base_url, review_id, user, ticket):
    """Returns list of {depotFile, rev, action} dicts."""
    url = f"{base_url}/api/v10/reviews/{review_id}/files"
    data = api_get(url, user, ticket)
    if data.get("error"):
        raise RuntimeError(f"fetch_file_list: {data['error']}")
    return data.get("data", {}).get("files", [])


def parse_diff_html(html_content):
    """
    Parse Swarm diff HTML into list of:
      {left_line, right_line, type: 'add'|'delete'|'same', text}
    """
    result = []
    for row in re.findall(r'<tr[^>]*class="diff[^"]*"[^>]*>(.*?)</tr>', html_content, re.DOTALL):
        left_num_m = re.search(r'line-num-left"[^>]*data-num="(\d*)"', row)
        right_num_m = re.search(r'line-num-right"[^>]*data-num="(\d*)"', row)
        val_m = re.search(r'class="line-value">(.*?)</td>', row, re.DOTALL)
        if not val_m:
            continue
        text = re.sub(r'<[^>]+>', '', val_m.group(1))
        text = html_mod.unescape(text)
        l_n = int(left_num_m.group(1)) if left_num_m and left_num_m.group(1) else None
        r_n = int(right_num_m.group(1)) if right_num_m and right_num_m.group(1) else None
        dtype = 'add' if text.startswith('+') else ('delete' if text.startswith('-') else 'same')
        result.append({'left': l_n, 'right': r_n, 'text': text, 'type': dtype})
    return result


def fetch_diff_html(base_url, review_id, depot_file, left_spec, right_spec, user, ticket):
    """
    Fetch Swarm HTML diff.
    left_spec / right_spec are the raw p4 filespec suffixes, e.g.:
      "#5"      → submitted revision 5
      "@=12345" → shelved in change 12345
    Returns (parsed_lines, error_string_or_None)
    """
    right = urllib.parse.quote(f"{depot_file}{right_spec}")
    left  = urllib.parse.quote(f"{depot_file}{left_spec}")
    url   = f"{base_url}/diff?left={left}&right={right}&ignoreWs=0&type=file&reviewId={review_id}&action=edit"
    req   = urllib.request.Request(url, headers=get_headers(user, ticket))
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            html = r.read().decode("utf-8", errors="replace")
        return parse_diff_html(html), None
    except urllib.error.HTTPError as e:
        return [], f"HTTP {e.code}"
    except Exception as e:
        return [], str(e)


def format_diff_lines(lines, context=3, full=False):
    """Format diff lines with context around changes, for AI consumption."""
    if full:
        out = []
        for l in lines:
            m = '+' if l['type'] == 'add' else ('-' if l['type'] == 'delete' else ' ')
            rn = l['right'] or l['left'] or '?'
            out.append(f"  L{str(rn):5} {m} {l['text']}")
        return '\n'.join(out)

    out = []
    changed_idxs = [i for i, l in enumerate(lines) if l['type'] != 'same']
    shown = set()
    for ci in changed_idxs:
        for i in range(max(0, ci - context), min(len(lines), ci + context + 1)):
            shown.add(i)
    prev_shown = None
    for i in sorted(shown):
        if prev_shown is not None and i > prev_shown + 1:
            out.append("  ...")
        l = lines[i]
        m = '+' if l['type'] == 'add' else ('-' if l['type'] == 'delete' else ' ')
        rn = l['right'] or l['left'] or '?'
        out.append(f"  L{str(rn):5} {m} {l['text']}")
        prev_shown = i
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description="Fetch Swarm review diffs")
    parser.add_argument("review_id", help="Review ID (numeric)")
    parser.add_argument("--user", default=os.environ.get("SWARM_USER", ""))
    parser.add_argument("--ticket", default=os.environ.get("SWARM_TICKET", ""))
    parser.add_argument("--base", default=BASE_URL, help="Swarm base URL")
    parser.add_argument("--output", "-o", help="Write output to file")
    parser.add_argument("--file", help="Only fetch diff for this depot file (partial match)")
    parser.add_argument("--context", type=int, default=3, help="Context lines around changes")
    parser.add_argument("--full", action="store_true", help="Show all lines, not just changed context")
    args = parser.parse_args()

    if not args.user or not args.ticket:
        print("Error: --user and --ticket required (or SWARM_USER/SWARM_TICKET env vars)", file=sys.stderr)
        sys.exit(1)

    review_id = args.review_id
    base_url  = args.base

    # ── Step 1: Determine review structure ──────────────────────────────────
    print(f"Fetching review metadata for #{review_id}...", file=sys.stderr)
    committed_changes, pending_change, versions = fetch_review_info(base_url, review_id, args.user, args.ticket)
    num_committed = len(committed_changes)

    print(f"  committed changes: {committed_changes}", file=sys.stderr)
    print(f"  pending shelved:   {pending_change}", file=sys.stderr)

    # ── Step 2: Fetch file list ──────────────────────────────────────────────
    print(f"Fetching file list...", file=sys.stderr)
    files = fetch_file_list(base_url, review_id, args.user, args.ticket)
    if not files:
        print("No files found.", file=sys.stderr)
        sys.exit(1)

    if args.file:
        files = [f for f in files if args.file in f.get("depotFile", "")]
        if not files:
            print(f"File not found: {args.file}", file=sys.stderr)
            sys.exit(1)

    # ── Step 3: Fetch diffs per file ─────────────────────────────────────────
    output_lines = []

    for f in files:
        depot_file = f.get("depotFile", "")
        cur_rev    = int(f.get("rev", 1))
        action     = f.get("action", "edit")
        fname      = depot_file.split("/")[-1]

        # Determine base revision for committed changes
        if num_committed > 0:
            base_rev = max(1, cur_rev - num_committed)
        else:
            # No committed changes — base is the current submitted rev (shelved only)
            base_rev = cur_rev

        # Build header
        committed_desc = f"rev{base_rev}→{cur_rev}" if num_committed > 0 else "no committed changes"
        shelved_desc   = f" + pending @{pending_change}" if pending_change else ""
        header = (
            f"\n{'='*70}\n"
            f"=== {fname}  [{action}]  ({committed_desc}{shelved_desc})\n"
            f"=== {depot_file}\n"
            f"{'='*70}"
        )
        output_lines.append(header)
        print(f"  Processing {fname}...", file=sys.stderr)

        has_any = False

        # ── Part A: Committed diff ───────────────────────────────────────────
        if num_committed > 0 and base_rev >= 1 and base_rev < cur_rev:
            committed_lines, err = fetch_diff_html(
                base_url, review_id, depot_file,
                f"#{base_rev}", f"#{cur_rev}",
                args.user, args.ticket
            )
            changed = [l for l in committed_lines if l['type'] != 'same']
            if err:
                output_lines.append(f"  [committed diff error: {err}]")
            elif changed:
                has_any = True
                output_lines.append(f"\n  --- Committed changes (rev{base_rev}→{cur_rev}) [{len(changed)} changed lines] ---")
                output_lines.append(format_diff_lines(committed_lines, context=args.context, full=args.full))

        # ── Part B: Pending shelved diff ─────────────────────────────────────
        if pending_change:
            shelved_lines, err = fetch_diff_html(
                base_url, review_id, depot_file,
                f"#{cur_rev}", f"@={pending_change}",
                args.user, args.ticket
            )
            changed = [l for l in shelved_lines if l['type'] != 'same']
            if err:
                output_lines.append(f"  [shelved diff error: {err}]")
            elif changed:
                has_any = True
                output_lines.append(f"\n  --- Pending shelved changes (@{pending_change}) [{len(changed)} changed lines] ---")
                output_lines.append(format_diff_lines(shelved_lines, context=args.context, full=args.full))

        if not has_any:
            output_lines.append("  [no changes detected]")

    result = '\n'.join(output_lines)

    if args.output:
        with open(args.output, 'w', encoding='utf-8') as fh:
            fh.write(result)
        print(f"Diffs written to {args.output}", file=sys.stderr)
    else:
        print(result)


if __name__ == "__main__":
    main()
