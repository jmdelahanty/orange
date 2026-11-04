#!/usr/bin/env python3
"""Writer ownership ratchet: every file Orange creates must belong to the sudo invoker.

Orange runs as root under sudo. A file it creates is root-owned unless the
writing thread holds an orange::ScopedFsuid (src/fsuid_guard.h; setfsuid is
per thread) or the writer sets the owner itself. A root-owned file in a
recording folder fails the GUI validator and blocks the invoker's tools
(orange_local_control.events.jsonl, 2026-10-16 to 2026-11-04).

For every call that creates or replaces a file or directory, this check wants a
ScopedFsuid declared earlier in a block that is still open, inside the same
function. A lambda handed to a thread or a log-writer job (std::thread,
std::async, .Post) starts a new function: the guard of the code that created
it does not run on that thread.

Sites that are correct without a guard (helpers whose callers hold one, files
that must stay root-owned) are listed in contracts/writer_ownership_allowlist.json
under "allowed": "file|function|kind" with a count and a reason. A helper that
writes for guarded callers is also named under "helpers": every call to it is
then a writer site of kind helper:<name>, so its callers are checked too. The check fails on:
an unguarded site that is not listed, more sites than a listed count, an empty
reason, and an entry whose count is now too high (lower it: the list only
shrinks). Textual heuristics, like the output ratchet: it reads the sources,
it does not compile them.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_output_names import REPO, producer_files, strip_comments  # noqa: E402

ALLOWLIST = REPO / "contracts" / "writer_ownership_allowlist.json"

# kind -> pattern on comment-stripped source. Reads, pipes and in-memory
# streams do not match.
WRITER_PATTERNS: dict[str, re.Pattern[str]] = {
    # A stream constructed with a path; a default-constructed member opens
    # nothing until stream_open.
    "ofstream": re.compile(r"\b(?:std::)?(?:ofstream|fstream)\b(?!\s*[&*>])(?:\s+\w+\s*[({]\s*[^)}\s]|\s*[({]\s*[^)}\s])"),
    "stream_open": re.compile(r"\w(?:\.|->)open\s*\("),
    "fopen_write": re.compile(r"\bfopen\s*\([^;]*?,\s*\"[wa]"),
    "open_creat": re.compile(r"\b(?:::)?open(?:at)?\s*\([^;]*O_CREAT"),
    "mkstemp": re.compile(r"\bmkstemps?\s*\("),
    "mkdir": re.compile(r"(?:\bcreate_director(?:y|ies)\s*\(|\b(?:::)?mkdir(?:at)?\s*\()"),
    "copy_file": re.compile(r"\bfilesystem::copy(?:_file)?\s*\("),
    "imwrite": re.compile(r"\bcv::imwrite\s*\("),
    "avio_open": re.compile(r"\bavio_open2?\s*\("),
    "shm_open": re.compile(r"\bshm_open\s*\("),
    "atomic_writer": re.compile(r"\b(?:write_file_atomic|write_json_atomic|create_file_once)\s*\("),
}
GUARD = re.compile(r"\bScopedFsuid\s+\w+")
# A lambda whose body runs on another thread.
THREAD_LAMBDA = re.compile(r"(?:std::thread|std::jthread|std::async|\.Post|\bPost)\s*\(")
FUNCTION_NAME = re.compile(r"(~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*"
                           r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:->\s*[\w:<>,\s*&]+)?"
                           r"(?::[^{;]*)?$")
CONTROL = {"if", "for", "while", "switch", "catch", "do", "else", "return", "sizeof", "decltype"}


def strip_strings(line: str) -> str:
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', line)


def function_name(header: str) -> str:
    """Name of the function whose body opens after this header text."""
    header = " ".join(strip_strings(header).split())
    m = FUNCTION_NAME.search(header)
    if not m:
        return "?"
    name = m.group(1).split("::")[-1]
    return "?" if name in CONTROL else name


class Block:
    __slots__ = ("guarded", "function", "is_function")

    def __init__(self, function: str | None, is_function: bool) -> None:
        self.guarded = False
        self.function = function
        self.is_function = is_function


def writer_patterns() -> dict[str, re.Pattern[str]]:
    patterns = dict(WRITER_PATTERNS)
    for name in load_helpers():
        patterns[f"helper:{name}"] = re.compile(rf"\b{re.escape(name)}\s*\(")
    return patterns


def scan(path: Path) -> list[tuple[int, str, str, bool]]:
    """(line, function, kind, guarded) for every writer site in one file."""
    patterns = writer_patterns()
    text = strip_comments(path.read_text(errors="replace"))
    lines = text.split("\n")
    sites: list[tuple[int, str, str, bool]] = []
    stack: list[Block] = []
    header = ""  # text since the last ; { or } at this level, for naming functions
    for number, raw in enumerate(lines, start=1):
        line = strip_strings(raw)
        # Writer sites and guards on this line are judged at the depth the line starts in.
        guarded_here = False
        for b in reversed(stack):
            if b.guarded:
                guarded_here = True
                break
            if b.is_function:
                break
        current_function = next((b.function for b in reversed(stack) if b.is_function), None)
        # Default arguments and empty initialisers are not blocks.
        line = line.replace("{}", "()")
        if GUARD.search(line) and stack:
            stack[-1].guarded = True
            guarded_here = True
        # Declarations at file or class scope write nothing.
        for kind, pattern in patterns.items():
            if current_function is None:
                break
            if kind == f"helper:{current_function}":
                continue  # the helper's own recursion or overloads
            for _ in pattern.finditer(raw if kind == "fopen_write" else line):
                sites.append((number, current_function, kind, guarded_here))
        for ch in line:
            if ch == "{":
                h = header.strip()
                if THREAD_LAMBDA.search(h) and re.search(r"\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?$", h):
                    stack.append(Block(f"{current_function or '?'}::<thread lambda>", True))
                elif not any(b.is_function for b in stack) and re.search(r"\)\s*(?:const\s*)?(?:noexcept\s*)?"
                                                                          r"(?:override\s*)?(?:->[^{;]*)?(?::[^{;]*)?$", h) \
                        and not re.match(r"^(?:namespace|struct|class|enum|union|extern)\b", h):
                    stack.append(Block(function_name(h), True))
                else:
                    stack.append(Block(None, False))
                header = ""
            elif ch == "}":
                if stack:
                    stack.pop()
                header = ""
            elif ch == ";":
                header = ""
            else:
                header += ch
        header += " "
    return sites


def load_allowlist() -> dict[str, dict]:
    data = json.loads(ALLOWLIST.read_text())
    return data.get("allowed", {})


def load_helpers() -> dict[str, str]:
    if not ALLOWLIST.exists():
        return {}
    return json.loads(ALLOWLIST.read_text()).get("helpers", {})


def current_unguarded() -> Counter:
    counts: Counter = Counter()
    for path in producer_files():
        rel = path.relative_to(REPO).as_posix()
        for _, function, kind, guarded in scan(path):
            if not guarded:
                counts[f"{rel}|{function}|{kind}"] += 1
    return counts


def violations() -> list[str]:
    allowed = load_allowlist()
    counts = current_unguarded()
    problems: list[str] = []
    for name, reason in sorted(load_helpers().items()):
        if not str(reason).strip():
            problems.append(f"{ALLOWLIST.name}: helpers: {name}: empty reason")
    for key, entry in sorted(allowed.items()):
        if not str(entry.get("reason", "")).strip():
            problems.append(f"{ALLOWLIST.name}: {key}: empty reason")
        if counts.get(key, 0) < int(entry.get("count", 0)):
            problems.append(f"{ALLOWLIST.name}: {key}: count {entry.get('count')} but {counts.get(key, 0)} "
                            "unguarded now; lower it (the list only shrinks)")
    lines_by_key: dict[str, list[int]] = {}
    for path in producer_files():
        rel = path.relative_to(REPO).as_posix()
        for number, function, kind, guarded in scan(path):
            if not guarded:
                lines_by_key.setdefault(f"{rel}|{function}|{kind}", []).append(number)
    for key, n in sorted(counts.items()):
        limit = int(allowed.get(key, {}).get("count", 0))
        if n > limit:
            rel = key.split("|", 1)[0]
            where = ",".join(str(x) for x in lines_by_key.get(key, []))
            problems.append(f"{rel}:{where}: {key.split('|', 1)[1]}: {n} unguarded writer site(s), "
                            f"{limit} allowed. Hold an orange::ScopedFsuid (src/fsuid_guard.h) on this "
                            "thread, or list it with a reason")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--list", action="store_true", help="print every unguarded site and exit 0")
    args = parser.parse_args()
    if args.list:
        for path in producer_files():
            rel = path.relative_to(REPO).as_posix()
            for number, function, kind, guarded in scan(path):
                if not guarded:
                    print(f"{rel}:{number}: {function} {kind}")
        return 0
    problems = violations()
    for p in problems:
        print(p)
    if problems:
        print(f"check_writer_ownership: {len(problems)} problem(s)")
        return 1
    print("check_writer_ownership: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
