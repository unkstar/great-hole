#!/usr/bin/env python3
"""
Swarm Comment Poster — post review-level comments via Swarm API v9 (form-encoded).

Usage:
  python3 post_comment.py <review_id> "comment body" --user USER --ticket TICKET
  python3 post_comment.py <review_id> --file comment.txt --user USER --ticket TICKET

Notes:
  - Uses /api/v9/comments with application/x-www-form-urlencoded (v10 POST returns 404)
  - Inline (line-level) comments require Perforce read permission on the depot file,
    which may not be available for protected depots. In that case, post review-level
    comments that reference file+line in the body text.
  - For multi-part long reviews, split into multiple calls.

Auth: SWARM_USER / SWARM_TICKET env vars, or --user/--ticket args
"""

import sys
import os
import json
import urllib.request
import urllib.error
import urllib.parse
import base64
import argparse


BASE_URL = "https://timi-swarm.ces.qq.com"


def post_comment(base_url, review_id, body, user, ticket):
    url = f"{base_url}/api/v9/comments"
    data = urllib.parse.urlencode({
        "topic": f"reviews/{review_id}",
        "body": body,
    }).encode()
    auth = base64.b64encode(f"{user}:{ticket}".encode()).decode()
    req = urllib.request.Request(url, data=data,
        headers={"Authorization": f"Basic {auth}",
                 "Content-Type": "application/x-www-form-urlencoded"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            resp = json.loads(r.read())
        c = resp.get("comment", {})
        return c.get("id"), None
    except urllib.error.HTTPError as e:
        return None, f"HTTP {e.code}: {e.read().decode()[:300]}"
    except Exception as e:
        return None, str(e)


def main():
    parser = argparse.ArgumentParser(description="Post a comment to a Swarm review")
    parser.add_argument("review_id", help="Review ID")
    parser.add_argument("body", nargs="?", help="Comment body text")
    parser.add_argument("--file", "-f", help="Read comment body from file")
    parser.add_argument("--user", default=os.environ.get("SWARM_USER", ""))
    parser.add_argument("--ticket", default=os.environ.get("SWARM_TICKET", ""))
    parser.add_argument("--base", default=BASE_URL)
    args = parser.parse_args()

    if not args.user or not args.ticket:
        print("Error: --user and --ticket required (or SWARM_USER/SWARM_TICKET)", file=sys.stderr)
        sys.exit(1)

    if args.file:
        with open(args.file, encoding="utf-8") as fh:
            body = fh.read().strip()
    elif args.body:
        body = args.body
    else:
        # Read from stdin
        body = sys.stdin.read().strip()

    if not body:
        print("Error: no comment body provided", file=sys.stderr)
        sys.exit(1)

    cid, err = post_comment(args.base, args.review_id, body, args.user, args.ticket)
    if err:
        print(f"Error: {err}", file=sys.stderr)
        sys.exit(1)
    print(f"Comment posted: id={cid}")


if __name__ == "__main__":
    main()
