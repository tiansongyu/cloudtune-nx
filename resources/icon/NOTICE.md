# CloudTune NX icon

`cloudtune.svg` is the editable vector master. Its white musical glyph is
adapted from the scalable icon in
[netease-cloud-music-gtk](https://github.com/gmg137/netease-cloud-music-gtk/blob/225d14fb42bc0e0fdce1cbcb640577c307efc641/data/icons/hicolor/scalable/apps/com.gitee.gmg137.NeteaseCloudMusicGtk4.svg)
by gmg137 and contributors, under that project's GPL-3.0-or-later license.
The adaptation removes editor metadata, shadows and filters, and uses a
solid red background with a white glyph for small-screen legibility.

NetEase Cloud Music's name and identifying mark belong to their respective
owner. CloudTune NX is an independent, unofficial client; no affiliation or
endorsement is implied.

`icon.png` (512 × 512) is loaded by Borealis for desktop windows.
`icon.jpg` (256 × 256, opaque baseline JPEG) is embedded in the Switch NRO
for HBMenu and emulator library views. The SVG remains sharp at any size;
the platform-specific images are exports from the vector, not enlarged
screenshots.

Regenerate with `/usr/bin/python3 scripts/render-icons.py`. On Debian/Ubuntu,
the optional rendering dependencies are `python3-gi`, `python3-cairo`,
`gir1.2-rsvg-2.0` and `python3-pil`. They are unnecessary for ordinary builds
because both exports are committed.
