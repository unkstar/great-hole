#!/usr/bin/env python3
"""
Swarm Review Fetcher
Usage: python3 swarm_review.py <review_url_or_id> [--user USER] [--ticket TICKET]
       python3 swarm_review.py <review_url_or_id> --comment "your comment" [--file path] [--line N]

Auth: uses env vars SWARM_USER and SWARM_TICKET, or --user/--ticket args
Base URL: auto-detected from URL, or defaults to https://timi-swarm.ces.qq.com
"""

import sys
import os
import re
import json
import urllib.request
import urllib.error
import base64
import argparse


def get_auth_header(user, ticket):
    creds = f"{user}:{ticket}"
    b64 = base64.b64encode(creds.encode()).decode()
    return {"Authorization": f"Basic {b64}", "Content-Type": "application/json"}


def api_get(base_url, path, user, ticket):
    url = f"{base_url}{path}"
    req = urllib.request.Request(url, headers=get_auth_header(user, ticket))
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        return {"error": f"HTTP {e.code}: {body}"}
    except Exception as e:
        return {"error": str(e)}


def api_post(base_url, path, user, ticket, data):
    url = f"{base_url}{path}"
    payload = json.dumps(data).encode()
    headers = get_auth_header(user, ticket)
    req = urllib.request.Request(url, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        return {"error": f"HTTP {e.code}: {body}"}
    except Exception as e:
        return {"error": str(e)}


def fetch_review(base_url, review_id, user, ticket):
    """Fetch review metadata"""
    data = api_get(base_url, f"/api/v10/reviews/{review_id}", user, ticket)
    if data.get("error"):
        return data
    reviews = data.get("data", {}).get("reviews", [])
    return reviews[0] if reviews else {"error": "Review not found"}


def fetch_files(base_url, review_id, user, ticket):
    """Fetch file list for a review"""
    data = api_get(base_url, f"/api/v10/reviews/{review_id}/files", user, ticket)
    if data.get("error"):
        return data
    return data.get("data", {}).get("files", [])


def fetch_comments(base_url, review_id, user, ticket):
    """Fetch comments for a review"""
    data = api_get(base_url, f"/api/v10/comments?topic=reviews/{review_id}&max=100", user, ticket)
    if data.get("error"):
        return data
    return data.get("data", {}).get("comments", [])


def post_comment(base_url, review_id, user, ticket, body, file_path=None, line=None):
    """Post a comment to a review"""
    payload = {
        "topic": f"reviews/{review_id}",
        "body": body,
        "silenceNotification": False
    }
    if file_path and line:
        payload["context"] = {
            "file": file_path,
            "leftLine": None,
            "rightLine": line,
            "content": ""
        }
    data = api_post(base_url, "/api/v10/comments", user, ticket, payload)
    return data


def format_review_summary(review):
    """Format review metadata as readable text"""
    lines = []
    lines.append(f"Review #{review.get('id')}")
    lines.append(f"Author:      {review.get('author')}")
    lines.append(f"State:       {review.get('stateLabel', review.get('state'))}")
    lines.append(f"Description: {review.get('description', '').strip()}")

    complexity = review.get("complexity", {})
    if complexity:
        lines.append(f"Complexity:  {complexity.get('files_modified')} files, "
                     f"+{complexity.get('lines_added')} -{complexity.get('lines_deleted')} "
                     f"~{complexity.get('lines_edited')} edited")

    participants = review.get("participantsData") or review.get("participants", {})
    if isinstance(participants, dict):
        votes = []
        for p, info in participants.items():
            if isinstance(info, dict) and info.get("vote"):
                v = info["vote"].get("value", 0)
                votes.append(f"{p}:{'✅' if v == 1 else '❌' if v == -1 else '⏳'}")
        if votes:
            lines.append(f"Votes:       {', '.join(votes)}")

    changes = review.get("changes", [])
    if changes:
        lines.append(f"Changes:     {', '.join(map(str, changes))}")

    return "\n".join(lines)


def format_files(files):
    """Format file list"""
    lines = [f"Files ({len(files)}):"]
    for f in files:
        depot = f.get("depotFile", "")
        action = f.get("action", "")
        size = f.get("fileSize", "")
        lines.append(f"  [{action:6}] {depot}  ({size} bytes)")
    return "\n".join(lines)


def format_comments(comments):
    """Format comments"""
    if not comments:
        return "No comments."
    lines = [f"Comments ({len(comments)}):"]
    for c in comments:
        author = c.get("user", "?")
        body = c.get("body", "").strip()
        ctx = c.get("context", {})
        loc = ""
        if ctx and ctx.get("file"):
            f = ctx["file"].split("/")[-1]
            line = ctx.get("rightLine") or ctx.get("leftLine", "")
            loc = f" [{f}:{line}]"
        lines.append(f"\n  @{author}{loc}:\n  {body}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Swarm Review Fetcher")
    parser.add_argument("review", help="Review URL or ID")
    parser.add_argument("--user", default=os.environ.get("SWARM_USER", ""))
    parser.add_argument("--ticket", default=os.environ.get("SWARM_TICKET", ""))
    parser.add_argument("--comment", help="Post a comment")
    parser.add_argument("--file", help="File path for inline comment")
    parser.add_argument("--line", type=int, help="Line number for inline comment")
    parser.add_argument("--json", action="store_true", help="Output raw JSON")
    args = parser.parse_args()

    # Parse URL or ID
    review_input = args.review
    base_url = "https://timi-swarm.ces.qq.com"
    url_match = re.match(r"(https?://[^/]+)/reviews/(\d+)", review_input)
    if url_match:
        base_url = url_match.group(1)
        review_id = url_match.group(2)
    elif re.match(r"^\d+$", review_input):
        review_id = review_input
    else:
        print(f"Error: invalid review URL or ID: {review_input}", file=sys.stderr)
        sys.exit(1)

    if not args.user or not args.ticket:
        print("Error: --user and --ticket required (or set SWARM_USER/SWARM_TICKET env vars)", file=sys.stderr)
        sys.exit(1)

    # Post comment mode
    if args.comment:
        result = post_comment(base_url, review_id, args.user, args.ticket,
                              args.comment, args.file, args.line)
        if args.json:
            print(json.dumps(result, indent=2))
        elif result.get("error"):
            print(f"Error: {result['error']}", file=sys.stderr)
            sys.exit(1)
        else:
            print(f"Comment posted successfully.")
        return

    # Fetch mode
    review = fetch_review(base_url, review_id, args.user, args.ticket)
    if review.get("error"):
        print(f"Error: {review['error']}", file=sys.stderr)
        sys.exit(1)

    files = fetch_files(base_url, review_id, args.user, args.ticket)
    comments = fetch_comments(base_url, review_id, args.user, args.ticket)

    if args.json:
        print(json.dumps({"review": review, "files": files, "comments": comments}, indent=2))
    else:
        print(format_review_summary(review))
        print()
        if isinstance(files, list):
            print(format_files(files))
        print()
        if isinstance(comments, list):
            print(format_comments(comments))


if __name__ == "__main__":
    main()
