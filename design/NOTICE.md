# Artwork and type

`cix-cache-logo.png` is the project's own artwork: the mark, and
"ache" set in script.

`cix-cache-wordmark.png` is the project name set in **Pacifico**, by
Vernon Adams, Jacques Le Bailly and the Pacifico Project Authors,
licensed under the **SIL Open Font License 1.1**
(<https://openfontlicense.org>). Pacifico was chosen by comparing
candidate script faces letter by letter against the artwork's own
"ache", which it matches.

The dashboard does not ship or serve the font. `tools/trace-logo.py`
traces this raster into outlines, and only those outlines are checked
in — the wordmark is artwork, not a redistributed font, and no glyph
set, font file or Reserved Font Name is redistributed here.

Regenerating the raster needs Pacifico locally; regenerating the SVGs
from the raster needs only Python and Pillow.
