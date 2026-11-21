#!/usr/bin/env python3
"""Ensure the English catalog is complete and safe to switch to."""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
POT = ROOT / "po" / "fmon.pot"
EN = ROOT / "po" / "en.po"

CJK = re.compile(r"[\u3400-\u9fff]")
FMT = re.compile(r"%(?:\d+\$)?[-+0-9.#]*[A-Za-z%]")


def unquote(s: str) -> str:
    s = s.strip()
    if s.startswith('"') and s.endswith('"'):
        s = s[1:-1]
    return s.replace(r"\"", '"').replace(r"\\", "\\")


def parse_po(path: Path):
    entries = []
    cur = {"msgid": "", "msgstr": "", "fuzzy": False, "msgctxt": None}
    field = None
    started = False

    def flush():
        nonlocal cur, field, started
        if started and (cur["msgid"] or cur["msgstr"]):
            entries.append(cur)
        cur = {"msgid": "", "msgstr": "", "fuzzy": False, "msgctxt": None}
        field = None
        started = False

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#"):
            if line.startswith("#,") and "fuzzy" in line:
                cur["fuzzy"] = True
            continue
        if line.startswith("msgctxt "):
            if started:
                flush()
            cur["msgctxt"] = unquote(line[8:])
            started = True
            field = "msgctxt"
        elif line.startswith("msgid "):
            if started and field != "msgid":
                flush()
            cur["msgid"] += unquote(line[6:])
            started = True
            field = "msgid"
        elif line.startswith("msgstr "):
            cur["msgstr"] += unquote(line[7:])
            started = True
            field = "msgstr"
        elif line.startswith('"'):
            if field:
                cur[field] += unquote(line)
    flush()
    return entries


def fmt_tokens(s: str):
    return [m.group(0) for m in FMT.finditer(s) if m.group(0) != "%%"]


def main():
    if not POT.exists():
        print(f"missing {POT}")
        return 1
    if not EN.exists():
        print(f"missing {EN}")
        return 1

    pot = [e for e in parse_po(POT) if e["msgid"]]
    en = [e for e in parse_po(EN) if e["msgid"]]
    by_id = {e["msgid"]: e for e in en}

    errors = []
    for p in pot:
        mid = p["msgid"]
        e = by_id.get(mid)
        if e is None:
            errors.append(f"missing translation: {mid[:60]!r}")
            continue
        if e["fuzzy"]:
            errors.append(f"fuzzy translation: {mid[:60]!r}")
        if not e["msgstr"].strip():
            errors.append(f"empty translation: {mid[:60]!r}")
            continue
        if CJK.search(e["msgstr"]):
            errors.append(f"Chinese text left in English: {mid[:60]!r}")
        if fmt_tokens(mid) != fmt_tokens(e["msgstr"]):
            errors.append(
                f"format mismatch: {mid[:60]!r} -> {e['msgstr'][:60]!r}"
            )

    # Even strings outside the POT (historical raw UI text) must be clean.
    for e in en:
        if e["fuzzy"]:
            errors.append(f"fuzzy catalog entry: {e['msgid'][:60]!r}")
        if not e["msgstr"].strip():
            errors.append(f"empty catalog entry: {e['msgid'][:60]!r}")
        if CJK.search(e["msgstr"]):
            errors.append(f"Chinese left in catalog: {e['msgid'][:60]!r}")

    msgfmt = shutil.which("msgfmt")
    if msgfmt:
        r = subprocess.run(
            [msgfmt, "-c", "-o", "/tmp/fmon-en-check.mo", str(EN)],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            errors.append(f"msgfmt failed:\n{r.stderr}")

    if errors:
        print("\n".join(errors[:40]))
        print(f"FAILED: {len(errors)} problem(s), {len(pot)} source strings")
        return 1

    print(f"OK: {len(pot)} English translations complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
