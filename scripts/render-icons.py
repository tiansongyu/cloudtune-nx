#!/usr/bin/env python3
"""Render the vector master to the desktop PNG and Switch NRO JPEG."""
import io
import pathlib

import cairo
import gi
from PIL import Image

gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg

root = pathlib.Path(__file__).resolve().parents[1]
directory = root / "resources/icon"
svg = Rsvg.Handle.new_from_file(str(directory / "cloudtune.svg"))
surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, 1024, 1024)
context = cairo.Context(surface)
# The NRO icon must be opaque, including the rounded-corner surround.
context.set_source_rgb(230 / 255, 0, 38 / 255)
context.paint()
viewport = Rsvg.Rectangle()
viewport.x = viewport.y = 0
viewport.width = viewport.height = 1024
svg.render_document(context, viewport)
buffer = io.BytesIO()
surface.write_to_png(buffer)
master = Image.open(buffer).convert("RGB")
master.resize((512, 512), Image.Resampling.LANCZOS).save(directory / "icon.png", optimize=True)
master.resize((256, 256), Image.Resampling.LANCZOS).save(
    directory / "icon.jpg", quality=98, subsampling=0, optimize=True, progressive=False)
print("Rendered 512px desktop PNG and 256px baseline JPEG from cloudtune.svg")
