#!/usr/bin/env python3
"""Give only the excluded IPC probe permission to constrain its own quota."""
import json
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
metadata = json.loads(source.read_text())
for capability in metadata["kernel_capabilities"]:
    if capability["type"] == "syscalls":
        capability["value"]["svcSetResourceLimitLimitValue"] = "0x7e"
output.write_text(json.dumps(metadata, indent=2) + "\n")
