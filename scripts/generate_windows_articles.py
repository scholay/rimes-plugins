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
        if body.lstrip().startswith("["):
            array, separator = body.split("].joined(separator:", 1)
            joiner = json.loads(re.search(r'"(?:[^"\\]|\\.)*"', separator).group(0))
            display = joiner.join(json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', array))
        else:
            display = "".join(json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', body))
        text = display.replace("\r", "").replace("\n", "")
        entries.append((identity, language, title, theme, difficulty, text, int(version), display))
    if len(entries) != 8 or len({entry[0] for entry in entries}) != 8:
        raise SystemExit("Expected eight unique original macOS practice articles")
    output = ('// Generated from the macOS original practice articles. SPDX-License-Identifier: Apache-2.0\n'
              '#include "metrics.hpp"\nnamespace rimes::windows::metrics {\n'
              'const std::vector<Article>& Articles() {\nstatic const std::vector<Article> values = {\n')
    for entry in entries:
        output += "{" + ",".join(json.dumps(value, ensure_ascii=False) for value in entry) + "},\n"
    output += "}; return values; }\n}\n"
    inventory = [{"id": row[0], "version": row[6], "text": row[5], "displayText": row[7]} for row in entries]
    raw = json.dumps(inventory, ensure_ascii=False)
    if ')article"' in raw: raise SystemExit("Article contains a reserved C++ delimiter")
    header = '// Generated from the macOS original article paragraphs. SPDX-License-Identifier: Apache-2.0\n#pragma once\n#include <string>\nnamespace rimes::practice::materials {\ninline const std::string& ArticleInventory() { static const std::string data = [] { std::string value;\n'
    for start in range(0, len(raw), 4000):
        header += 'value += R"article(' + raw[start:start+4000] + ')article";\n'
    header += 'return value; }(); return data; }\n}\n'
    inventory_file = root / 'native/shared/typing-practice/article_inventory.hpp'
    if args.check:
        if not inventory_file.exists() or inventory_file.read_text(encoding='utf-8') != header:
            raise SystemExit('Shared article paragraphs are stale; run scripts/generate_windows_articles.py')
    else:
        inventory_file.write_text(header, encoding='utf-8', newline='\n')
    destination = root / "native/windows/metrics_articles.cpp"
    if args.check:
        if not destination.exists() or destination.read_text(encoding="utf-8") != output:
            raise SystemExit("Windows articles are stale; run scripts/generate_windows_articles.py")
    else:
        destination.write_text(output, encoding="utf-8", newline="\n")
    print("Windows practice articles match the eight macOS originals.")


if __name__ == "__main__":
    main()
