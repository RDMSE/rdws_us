#!/usr/bin/env python3
"""
Generate a signed HS256 JWT for testing the rdws gateway.

Usage:
    python3 tools/generate_token.py
    python3 tools/generate_token.py --subject alice@company.com --role admin --ttl 86400
    python3 tools/generate_token.py --secret my-secret --issuer my-issuer

Environment variables (override defaults):
    RDWS_JWT_SECRET   — signing secret   (default: meu-segredo-dev)
    RDWS_JWT_ISSUER   — "iss" claim      (default: rdws-dev)
    RDWS_JWT_AUDIENCE — "aud" claim      (default: rdws-gateway)
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import time


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def generate_token(secret: str, subject: str, issuer: str, audience: str,
                   ttl: int, extra_claims: dict) -> str:
    header  = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())

    payload = {
        "sub": subject,
        "iss": issuer,
        "aud": audience,
        "iat": int(time.time()),
        "exp": int(time.time()) + ttl,
        **extra_claims,
    }
    payload_b64 = b64url(json.dumps(payload, separators=(",", ":")).encode())

    signing_input = f"{header}.{payload_b64}"
    sig = b64url(
        hmac.new(secret.encode(), signing_input.encode(), hashlib.sha256).digest()
    )

    return f"{signing_input}.{sig}"


def main():
    parser = argparse.ArgumentParser(description="Generate a test JWT for the rdws gateway.")
    parser.add_argument("--secret",   default=os.getenv("RDWS_JWT_SECRET",   "meu-segredo-dev"))
    parser.add_argument("--issuer",   default=os.getenv("RDWS_JWT_ISSUER",   "rdws-dev"))
    parser.add_argument("--audience", default=os.getenv("RDWS_JWT_AUDIENCE", "rdws-gateway"))
    parser.add_argument("--subject",  default="developer")
    parser.add_argument("--role",     default="")
    parser.add_argument("--ttl",      type=int, default=3600, help="Validity in seconds (default: 3600)")
    parser.add_argument("--bearer",   action="store_true", help="Print with 'Bearer ' prefix")
    args = parser.parse_args()

    extra = {}
    if args.role:
        extra["role"] = args.role

    token = generate_token(
        secret=args.secret,
        subject=args.subject,
        issuer=args.issuer,
        audience=args.audience,
        ttl=args.ttl,
        extra_claims=extra,
    )

    exp_ts = int(time.time()) + args.ttl
    exp_str = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(exp_ts))

    print(f"Subject  : {args.subject}")
    print(f"Issuer   : {args.issuer}")
    print(f"Audience : {args.audience}")
    if args.role:
        print(f"Role     : {args.role}")
    print(f"Expires  : {exp_str} (+{args.ttl}s)")
    print()
    if args.bearer:
        print(f"Bearer {token}")
    else:
        print(token)


if __name__ == "__main__":
    main()
