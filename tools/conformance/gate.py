#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""Usage: python3 -I tools/conformance/gate.py docs/conformance.md lir/test/*_test.cpp

Writes docs/conformance.md for the rewrite: every statement, and the tests
named after it. A test proves a statement when its name carries the
statement's identifier: MQTT_3_1_2_11 (5.0), MQTT311_3_1_2_11 (3.1.1), with
"_and_N" for another statement of the same section and "_to_N" for a range."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

TEST = re.compile(r"TEST(?:_F)?\((\w+),\s*(\w+)\)")
ID = re.compile(r"(MQTT311|MQTT)_(\d+)_(\d+)_(\d+)_(\d+)((?:_(?:and|to)_\d+)*)")


def ids_of(name):
    out = []
    for m in ID.finditer(name):
        version = "3.1.1" if m.group(1) == "MQTT311" else "5.0"
        section = f"{m.group(2)}.{m.group(3)}.{m.group(4)}"
        numbers = [int(m.group(5))]
        for op, n in re.findall(r"_(and|to)_(\d+)", m.group(6)):
            n = int(n)
            if op == "and":
                numbers.append(n)
            else:
                numbers.extend(range(numbers[-1] + 1, n + 1))
        out += [(version, f"MQTT-{section}-{k}") for k in numbers]
    return out


def main(out_path, tests):
    preamble = f"{HERE}/preamble.md"
    v31 = f"{HERE}/mqtt-3.1.md"
    proven = {}
    for path in tests:
        text = open(path, encoding="utf-8").read()
        short = path.split("/")[-1]
        for suite, name in TEST.findall(text):
            for key in ids_of(name):
                proven.setdefault(key, []).append(f"{short}:{suite}.{name}")
    stmts = {}
    for version, f in (("5.0", f"{HERE}/statements-5.0.tsv"), ("3.1.1", f"{HERE}/statements-3.1.1.tsv")):
        stmts[version] = [line.rstrip("\n").split("\t") for line in open(f, encoding="utf-8")]
    known = {(v, row[0]) for v, rows in stmts.items() for row in rows}
    stray = sorted(k for k in proven if k not in known)
    if stray:
        sys.exit(f"tests name statements that do not exist: {stray}")

    lines = [open(preamble, encoding="utf-8").read().rstrip("\n"), "", "## The figures", ""]
    lines += ["| Version | Statements | Proven by a test | Not yet |", "|---|---|---|---|"]
    for version in ("5.0", "3.1.1"):
        rows = stmts[version]
        done = sum(1 for r in rows if (version, r[0]) in proven)
        lines.append(f"| {version} | {len(rows)} | {done} | {len(rows) - done} |")
    lines += ["", open(v31, encoding="utf-8").read().rstrip("\n")]
    for version, title in (("5.0", "MQTT 5.0"), ("3.1.1", "MQTT 3.1.1")):
        lines += ["", f"## {title}, statement by statement", "", "| Id | Section | Statement | Test |", "|---|---|---|---|"]
        for ident, section, text in stmts[version]:
            tests_for = "<br>".join(f"`{t}`" for t in proven.get((version, ident), [])) or "not yet"
            cell = text.replace("|", "\\|")
            lines.append(f"| `{ident}` | {section.split(' ')[0]} | {cell} | {tests_for} |")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(("\n".join(lines) + "\n").replace("‑", "-"))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
