#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""Usage: python3 -I tools/conformance/extract.py <specification.html> > statements-<version>.tsv

Extracts the normative statements of an MQTT specification (HTML) as TSV:
id, section, statement text. Each statement is the text of its paragraph that
precedes its identifier, after any earlier identifier in the same paragraph."""
import html
import re
import sys
from html.parser import HTMLParser

BLOCK = {"p", "li", "h1", "h2", "h3", "h4", "h5", "td", "tr", "div", "table"}
# 3.1.1 misprints three: 3.3.1.-1, 3-8.3-4 and 3.1.2.7 (for 3.3.1-1, 3.8.3-4, 3.1.2-7).
ID = re.compile(r"\[MQTT-(\d+(?:[.-]\d+)*?)\.?[-.](\d+)\]")
HEAD = re.compile(r"^(\d+(?:\.\d+)*)\s+(.*)$")


class Blocks(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks = []  # (tag, text)
        self.buf = []
        self.tag = "p"
        self.skip = 0

    def flush(self):
        text = re.sub(r"\s+", " ", "".join(self.buf)).strip()
        if text:
            self.blocks.append((self.tag, text))
        self.buf = []

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.skip += 1
        if tag in BLOCK:
            self.flush()
            self.tag = tag

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.skip -= 1
        if tag in BLOCK:
            self.flush()

    def handle_data(self, data):
        if not self.skip:
            self.buf.append(data)


def main(path):
    parser = Blocks()
    raw = open(path, "rb").read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("cp1252", errors="replace")  # 3.1.1 is published in Windows-1252
    text = text.replace("‑", "-").replace(" ", " ")
    parser.feed(text)
    parser.flush()
    section = ""
    seen = set()
    prev = ""
    for tag, text in parser.blocks:
        if tag in ("h1", "h2", "h3", "h4", "h5"):
            m = HEAD.match(text)
            if m:
                section = m.group(1) + " " + m.group(2)
            continue
        last = 0
        for m in ID.finditer(text):
            ident = "MQTT-" + m.group(1).replace("-", ".") + "-" + m.group(2)
            stmt = text[last:m.start()].strip(" .;:")
            if len(stmt) < 15 and last == 0:
                stmt = (prev + " " + stmt).strip(" .;:")  # the statement is the block before
            last = m.end()
            if ident in seen:
                continue  # cited again in the conformance appendix
            seen.add(ident)
            print(f"{ident}\t{section}\t{stmt}")
        prev = ID.sub("", text).strip()


if __name__ == "__main__":
    main(sys.argv[1])
