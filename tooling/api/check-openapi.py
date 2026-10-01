#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that docs/openapi.yaml describes the routes the device actually has.

A specification written alongside an implementation drifts from it the first
time somebody adds a route in a hurry, and the only thing worse than no API
documentation is documentation that is confidently wrong. This reads the
router and the specification and insists they agree.

    python3 tooling/api/check-openapi.py

Deliberately parses the YAML with a regex rather than importing a library.
The check has to run in CI without installing anything, and it only needs the
top-level path keys - a full YAML parse would be a dependency bought for one
line of value.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROUTER = ROOT / "firmware" / "src" / "api" / "Http.cpp"
SPEC = ROOT / "docs" / "openapi.yaml"


def routes_from_router(source: str) -> set:
    """Every path matchRoute() will resolve, in OpenAPI's path syntax."""
    routes = set()

    # Three segments: /api/v1/<head>
    #     if (head == "device") match.resource = ...
    for head in re.findall(r'head == "(\w+)"\)\s*match\.resource', source):
        routes.add("/" + head)

    # Four segments, fixed: /api/v1/<head>/<tail>
    #     if (head == "system" && parts[3] == "reboot") {
    for head, tail in re.findall(
        r'(?<!parts\.size\(\) == 5 && )head == "(\w+)" && parts\[3\] == "(\w+)"\)', source
    ):
        routes.add("/%s/%s" % (head, tail))

    # Five segments, fixed: /api/v1/glucose/alarm/test
    for head, middle, tail in re.findall(
        r'parts\.size\(\) == 5 && head == "(\w+)" && parts\[3\] == "(\w+)" && parts\[4\] == "(\w+)"',
        source,
    ):
        routes.add("/%s/%s/%s" % (head, middle, tail))

    # Four segments, an id: the collections that take one.
    #     if (head == "apps") { match.resource = Resource::AppItem;
    for head in re.findall(r'head == "(\w+)"\)\s*\{\s*match\.resource = Resource::\w+Item', source):
        routes.add("/%s/{id}" % head)

    # Five segments: /api/v1/apps/{id}/activate
    for head, tail in re.findall(
        r'parts\.size\(\) == 5 && head == "(\w+)" && parts\[4\] == "(\w+)"', source
    ):
        routes.add("/%s/{id}/%s" % (head, tail))

    return routes


def paths_from_spec(text: str) -> set:
    """Top-level keys under `paths:`, which are the documented routes."""
    paths = set()
    in_paths = False
    for line in text.splitlines():
        if re.match(r"^paths:\s*$", line):
            in_paths = True
            continue
        if in_paths:
            # A new top-level key ends the section.
            if re.match(r"^\S", line):
                break
            m = re.match(r"^  (/\S*):\s*$", line)
            if m:
                paths.add(m.group(1))
    return paths


def main() -> int:
    if not ROUTER.exists():
        print("check-openapi: cannot find %s" % ROUTER, file=sys.stderr)
        return 2
    if not SPEC.exists():
        print("check-openapi: cannot find %s" % SPEC, file=sys.stderr)
        return 2

    implemented = routes_from_router(ROUTER.read_text(encoding="utf-8"))
    documented = paths_from_spec(SPEC.read_text(encoding="utf-8"))

    # A sanity check on the extraction itself, because "the router has no
    # routes" should read as a broken checker rather than a broken router.
    if len(implemented) < 10:
        print(
            "check-openapi: only found %d routes in the router, which means this "
            "checker is broken rather than the API" % len(implemented),
            file=sys.stderr,
        )
        return 2

    undocumented = sorted(implemented - documented)
    invented = sorted(documented - implemented)

    if not undocumented and not invented:
        print("check-openapi: %d routes, all documented" % len(implemented))
        return 0

    if undocumented:
        print("Routes the device serves but openapi.yaml does not describe:", file=sys.stderr)
        for route in undocumented:
            print("  %s" % route, file=sys.stderr)
    if invented:
        print("Routes openapi.yaml describes that the device does not serve:", file=sys.stderr)
        for route in invented:
            print("  %s" % route, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
