# Artwork and type

`cix-cache-logo.png` is the project's own artwork: the mark, and
"ache" set in script. Nothing is generated from it any more -- it is
kept as the original.

`cix-tile.svg` is the mark as drawn vector art, and is the source for
both the menu bar tile and the favicon. It is deliberately not traced
from the raster: there the mark's lower right stroke runs into the cyan
of "ache", so a colour separation cuts the stroke and clips the glyph.

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

`web/InterVariable.woff2` is **Inter**, by Rasmus Andersson and the Inter
Project Authors, licensed under the **SIL Open Font License 1.1**
(`Inter-LICENSE.txt`). It is the typeface the Cix brand system names, and
is served from this repository rather than a CDN so the dashboard does
not depend on the network to render correctly. Redistributed unmodified,
with its licence, which the OFL permits.
