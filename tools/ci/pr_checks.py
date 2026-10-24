#!/usr/bin/env python3
"""Pull request checks: is the change meaningful, described and tested?

Run in CI (see .github/workflows/pr-checks.yml) and locally before opening a PR:

    PR_TITLE="feat(ui): ..." PR_BODY="$(cat body.md)" python3 tools/ci/pr_checks.py origin/main HEAD

Inputs: base and head revisions as arguments; PR_TITLE, PR_BODY and PR_LABELS
(comma separated) in the environment. Exit status 1 when a check fails.
Warnings never fail the run.
"""
import os
import re
import subprocess
import sys

TYPES = "feat|fix|docs|test|refactor|perf|build|ci|chore|revert"
TITLE_RE = re.compile(rf"^({TYPES})(\([a-z0-9._/-]+\))?!?: \S.{{4,}}$")
MAX_TITLE = 100
REQUIRED_SECTIONS = ("Summary", "Why", "How I tested it")
MIN_SECTION_CHARS = 20
WARN_LINES = 1500
FAIL_LINES = 6000
MAX_NEW_FILE_BYTES = 1_000_000
FORBIDDEN = (
    re.compile(r"(^|/)secrets\.ini$"),
    re.compile(r"(^|/)\.env($|\.)"),
    re.compile(r"\.(pem|key|p12|pfx)$"),
    re.compile(r"(^|/)(\.pio|node_modules|build)/"),
    re.compile(r"(^|/)helper\.env$"),
)
# Paths whose changes need a test change next to them.
NEEDS_TESTS = (
    (re.compile(r"^ui/src/.*\.(cpp|h)$"), re.compile(r"^sim/tests/"), "ui/src needs a test under sim/tests"),
    (re.compile(r"^helper/(src|api)/"), re.compile(r"^helper/test/"), "helper code needs a test under helper/test"),
)
HARDWARE_PATHS = re.compile(r"^firmware/app/src/")
NO_TESTS_LABEL = "no-tests-needed"
LARGE_LABEL = "large-change"


def sh(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def strip_comments(text):
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


def section(body, name):
    """Text under '## name' up to the next '## ' heading, comments removed."""
    body = strip_comments(body)
    m = re.search(rf"^##\s+{re.escape(name)}\s*$(.*?)(?=^##\s|\Z)", body, flags=re.S | re.M)
    return m.group(1).strip() if m else None


def check_title(title):
    errors = []
    if not TITLE_RE.match(title):
        errors.append(
            "Title must look like 'type(scope): what changed' with type one of "
            f"{TYPES.replace('|', ', ')}. Got: {title!r}"
        )
    if len(title) > MAX_TITLE:
        errors.append(f"Title is {len(title)} characters; keep it under {MAX_TITLE}.")
    return errors


def check_body(body):
    errors = []
    for name in REQUIRED_SECTIONS:
        text = section(body, name)
        if text is None:
            errors.append(f"Description is missing the '## {name}' section (see the PR template).")
        elif len(re.sub(r"\s+", " ", text)) < MIN_SECTION_CHARS:
            errors.append(f"'## {name}' needs a real answer (at least {MIN_SECTION_CHARS} characters).")
    if not re.search(r"^- \[[xX]\] .*no secrets", strip_comments(body), flags=re.M | re.I):
        errors.append("Tick the box confirming the change contains no secrets, tokens or Wi-Fi passwords.")
    return errors


def changed_files(base, head):
    out = sh("git", "diff", "--name-status", "--no-renames", f"{base}...{head}")
    files = []
    for line in out.splitlines():
        status, _, path = line.partition("\t")
        files.append((status, path))
    return files


def changed_lines(base, head):
    total = 0
    for line in sh("git", "diff", "--numstat", f"{base}...{head}").splitlines():
        a, b, _ = line.split("\t", 2)
        if a.isdigit() and b.isdigit():
            total += int(a) + int(b)
    return total


def blob_size(head, path):
    try:
        return int(sh("git", "cat-file", "-s", f"{head}:{path}").strip())
    except subprocess.CalledProcessError:
        return 0


def check_files(files, labels, body, lines, head, size_of=blob_size):
    errors, warnings = [], []
    paths = [p for s, p in files if s != "D"]
    for path in paths:
        for rx in FORBIDDEN:
            if rx.search(path):
                errors.append(f"{path}: this kind of file must never be committed.")
    for status, path in files:
        if status == "A" and size_of(head, path) > MAX_NEW_FILE_BYTES and not path.startswith("ui/fonts/"):
            errors.append(f"{path}: new file over {MAX_NEW_FILE_BYTES // 1000} KB. Link to it or compress it.")
    if NO_TESTS_LABEL not in labels:
        for code, test, msg in NEEDS_TESTS:
            touched = [p for p in paths if code.match(p)]
            if touched and not any(test.match(p) for p in paths):
                errors.append(f"{msg} (changed: {touched[0]}{' ...' if len(touched) > 1 else ''}). "
                              f"If a test is truly impossible, a maintainer can add the '{NO_TESTS_LABEL}' label.")
    if any(HARDWARE_PATHS.match(p) for p in paths):
        if not re.search(r"(board|hardware|device)", strip_comments(body), flags=re.I) or \
           not re.search(r"(tested on|flashed|ran on|verified on)", strip_comments(body), flags=re.I):
            warnings.append("Firmware changed: say which board revision you tested on and what you saw "
                            "(for example 'Tested on a V2 board: flashed env app, ...').")
    if lines > FAIL_LINES and LARGE_LABEL not in labels:
        errors.append(f"{lines} changed lines is too big to review. Split it, or ask a maintainer for '{LARGE_LABEL}'.")
    elif lines > WARN_LINES:
        warnings.append(f"{lines} changed lines: consider splitting this into smaller pull requests.")
    return errors, warnings


def main(argv, env):
    if len(argv) != 3:
        print(__doc__)
        return 2
    base, head = argv[1], argv[2]
    title, body = env.get("PR_TITLE", ""), env.get("PR_BODY", "")
    labels = {s.strip() for s in env.get("PR_LABELS", "").split(",") if s.strip()}
    files = changed_files(base, head)
    errors = check_title(title) + check_body(body)
    e2, warnings = check_files(files, labels, body, changed_lines(base, head), head)
    errors += e2
    for w in warnings:
        print(f"::warning::{w}")
    for e in errors:
        print(f"::error::{e}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Pull request checks\n")
            f.write("\n".join(f"- :x: {e}" for e in errors) or "- :white_check_mark: all checks passed")
            f.write("\n" + "\n".join(f"- :warning: {w}" for w in warnings) + "\n")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv, os.environ))
