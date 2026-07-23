#!/usr/bin/env python3
"""Idempotently upserts routes.json into a running gateway's routing table.

Why this exists: the gateway's routes.json lives inside a named Docker volume
(docker-compose.qa-app.yml: gateway_data). docker-entrypoint.sh only seeds that
volume from the image on first run — after that, runtime CRUD (POST/PUT/DELETE
/routes) persists directly into the volume, and redeploys never touch it again.
That means a route added to routes.json in git silently never reaches an
existing QA/prod gateway until someone notices the 404 and intervenes by hand.

This script closes that gap: run it after every deploy (once the gateway is
up) to add any route present in the repo's routes.json but missing from the
live gateway. It only ever creates missing routes — it never updates or
deletes existing ones, so runtime-added/edited routes are left alone.

Usage: sync_gateway_routes.py <routes.json path> <gateway base URL>
"""
import json
import sys
import urllib.error
import urllib.request


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <routes.json path> <gateway base URL>", file=sys.stderr)
        return 1

    routes_path, gateway_url = sys.argv[1], sys.argv[2].rstrip("/")

    with open(routes_path, encoding="utf-8") as f:
        desired = json.load(f)

    with urllib.request.urlopen(f"{gateway_url}/routes") as resp:
        existing = json.load(resp)

    # Match on (httpMethod, httpPath) — the id assigned by POST /routes is
    # always server-generated (EventRouter::addRule ignores any id in the
    # body), so matching on git's stable ids would just re-create the same
    # route on every deploy.
    existing_keys = {(r.get("httpMethod"), r.get("httpPath")) for r in existing}

    created = 0
    for rule in desired:
        key = (rule.get("httpMethod"), rule.get("httpPath"))
        if key in existing_keys:
            continue

        body = json.dumps(rule).encode("utf-8")
        req = urllib.request.Request(
            f"{gateway_url}/routes",
            data=body,
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req) as resp:
                if resp.status not in (200, 201):
                    print(f"Failed to create route {rule.get('id')}: HTTP {resp.status}",
                          file=sys.stderr)
                    return 1
        except urllib.error.HTTPError as e:
            print(f"Failed to create route {rule.get('id')}: HTTP {e.code} {e.read().decode()}",
                  file=sys.stderr)
            return 1

        print(f"Created missing route: {rule.get('id')} ({key[0]} {key[1]})")
        created += 1

    skipped = len(desired) - created
    print(f"Route sync complete — {created} created, {skipped} already present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
