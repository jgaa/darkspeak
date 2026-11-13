#!/usr/bin/env python3
"""Project dependency pins live in root CMake; synchronize Flatpak preparation."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
cmake = (root / "CMakeLists.txt").read_text()
pin = re.search(r"set\(DARKSPEAK_YAHAT_REVISION ([0-9a-f]{40})\)", cmake).group(1)
manifest = root / "flatpak/org.darkspeak.DarkSpeak.yaml"
text = manifest.read_text()
text, count = re.subn(r"(url: https://github.com/jgaa/yahat-cpp.git\n\s+commit: )[0-9a-f]{40}",
                      lambda match: match[1] + pin, text)
if count != 1:
    raise SystemExit("Expected exactly one yahat source declaration")
manifest.write_text(text)
