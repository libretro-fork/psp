#!/usr/bin/env python3
"""Translate MSVC /sourceDependencies JSON into GNU make dependencies."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def make_path(path):
    # cl.exe reports native Windows paths; make runs in the MSYS environment
    # already required by the MSVC platform's toolchain discovery.
    if re.match(r"^[A-Za-z]:", path) or path.startswith("\\\\"):
        path = subprocess.check_output(["cygpath", "-u", path], text=True).strip()
    path = path.replace("\\", "/")
    return path.replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:")


def main():
    source, target, output = sys.argv[1:]
    data = json.loads(Path(source).read_text(encoding="utf-8-sig"))["Data"]
    headers = list(dict.fromkeys([data["Source"], *data.get("Includes", [])]))
    escaped = [make_path(path) for path in headers]
    text = make_path(target) + ": " + " ".join(escaped) + "\n"
    text += "".join(path + ":\n" for path in escaped)
    temporary = Path(output + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, output)


if __name__ == "__main__":
    main()
