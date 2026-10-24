#!/usr/bin/env python3
"""Fail when a relative link or image in a Markdown file points at a missing file or heading."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
SKIP_DIRS = {"node_modules", ".git", "build", ".pio", ".superpowers"}


def slug(heading):
    s = re.sub(r"[`*_]", "", heading.strip().lower())
    s = re.sub(r"[^\w\s-]", "", s)
    return re.sub(r"\s", "-", s)


def anchors(path):
    out = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^#{1,6}\s+(.*)$", line)
        if m:
            out.add(slug(m.group(1)))
    return out


def main():
    bad = []
    for md in ROOT.rglob("*.md"):
        if SKIP_DIRS & set(md.relative_to(ROOT).parts):
            continue
        text = re.sub(r"```.*?```", "", md.read_text(encoding="utf-8"), flags=re.S)
        for target in LINK.findall(text):
            if re.match(r"^(https?:|mailto:)", target):
                continue
            file_part, _, frag = target.partition("#")
            dest = md if not file_part else (md.parent / file_part)
            if not dest.exists():
                bad.append(f"{md.relative_to(ROOT)}: missing file {target}")
            elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                bad.append(f"{md.relative_to(ROOT)}: missing heading #{frag} in {dest.relative_to(ROOT)}")
    print("\n".join(bad) or "all relative links resolve")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
