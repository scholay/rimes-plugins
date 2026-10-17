#!/usr/bin/env python3
# Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
"""Project the original macOS practice articles into the Windows adapter."""
import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / "native/macos/TypingTestArticles.swift").read_text(encoding="utf-8")
    pattern = (r'\.init\(id: "([^"]+)", version: (\d+), language: \.(\w+),\s*'
               r'title: "([^"]+)", theme: "([^"]+)", difficulty: "([^"]+)", text:\s*'
               r'(.*?)(?=\n        \.init|\n    \])')
    entries = []
    for match in re.finditer(pattern, source, re.S):
        identity, version, language, title, theme, difficulty, body = match.groups()
        text = "".join(json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', body))
        entries.append((identity, language, title, theme, difficulty, text, int(version)))
    if len(entries) != 8 or len({entry[0] for entry in entries}) != 8:
        raise SystemExit("Expected eight unique original macOS practice articles")
    output = ('// Generated from the macOS original practice articles. SPDX-License-Identifier: Apache-2.0\n'
              '#include "metrics.hpp"\nnamespace rimes::windows::metrics {\n'
              'const std::vector<Article>& Articles() {\nstatic const std::vector<Article> values = {\n')
    for entry in entries:
        output += "{" + ",".join(json.dumps(value, ensure_ascii=False) for value in entry) + "},\n"
    output += "}; return values; }\n}\n"
    destination = root / "native/windows/metrics_articles.cpp"
    if args.check:
        if not destination.exists() or destination.read_text(encoding="utf-8") != output:
            raise SystemExit("Windows articles are stale; run scripts/generate_windows_articles.py")
    else:
        destination.write_text(output, encoding="utf-8", newline="\n")
    print("Windows practice articles match the eight macOS originals.")


if __name__ == "__main__":
    main()
