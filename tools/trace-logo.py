#!/usr/bin/env python3
#
# Trace the brand artwork into the inline SVGs the dashboard uses.
#
# Everything is emitted as paths rather than as text in a webfont. A
# webfont would mean either a CDN this dashboard must not depend on at
# view time, or shipping a 330KB TTF to draw nine glyphs -- and either
# way a flash of fallback type on every load. Paths have neither
# problem and colour themselves from CSS.
#
# Two paths per image, not one, because each half is coloured
# independently: the dark half follows the page's text colour so it
# inverts with the theme, the cyan half keeps the brand colour in both.
# Both sources are clean two-colour rasters over transparency, so the
# separation is exact rather than a guess -- a pixel's blue channel
# says how much of it is cyan, its alpha how much of it is ink at all.
#
#   python3 tools/trace-logo.py
#
# Sources, both in design/:
#
#   cix-cache-logo.png      the artwork: the mark, and "ache" in script
#   cix-cache-wordmark.png  "cix-cache" set in Pacifico (SIL OFL), with
#                           "cix" in #000 and "-cache" in #00a8e0, at
#                           300px on transparency. Pacifico because it
#                           is the hand the artwork's "ache" is drawn
#                           in -- compared against it letter by letter
#                           before choosing.
#
# Outputs: web/favicon.svg directly, plus build/*-inline.svg to be
# pasted into web/index.html. They have to be inline in the document
# for currentColor to see the page's theme at all, which an <img> would
# not.
#
# Regenerate whenever the artwork changes; nothing else reads the PNGs.

import math
import re
import sys
from PIL import Image


# The mark is authored vector art, not traced. In cix-cache-logo.png its
# lower right stroke runs into the cyan of "ache", so separating that
# image by colour cuts the stroke where the two meet and the glyph comes
# out clipped -- which is what shipped before this. Nothing here traces
# the mark any more; it is copied from the drawn source.
SRC_TILE = "design/cix-tile.svg"

# The favicon cannot see the page's theme -- it is drawn by the browser
# chrome, not the document -- so it carries its own media query instead
# of custom properties. These are the same tokens the stylesheet uses,
# inverted the same way: the tile takes the text colour and the glyph
# the background.
FAVICON_LIGHT = ("#1c1c1e", "#f5f5f7")
FAVICON_DARK = ("#f2f2f2", "#121214")
OUT_MARK = "build/mark-inline.svg"
OUT_FAVICON = "web/favicon.svg"

CYAN = "#00a8e0"

# Douglas-Peucker tolerance, in source pixels. The wordmark renders around
# 420px wide from a 996px source, so 0.9px here is under half a device
# pixel on screen -- past the point where more precision is visible, and
# the difference between a 12KB inline SVG and a 40KB one.
EPS = 0.9
EPS_FAVICON = 3.0

# Below this the "corner" is really a smooth turn sampled coarsely, and
# smoothing through it looks right. Above it the artwork has an actual
# cusp -- the stroke ends in the script face -- and rounding it off is
# visible as a blunted tip.
CORNER_DEG = 58

# Marching-squares edge endpoints per case, as (edge, edge) pairs.
# Bit 0 = top-left corner inside, then clockwise.
CASES = {
	1:  [("L", "T")], 2:  [("T", "R")], 3:  [("L", "R")],
	4:  [("R", "B")], 6:  [("T", "B")], 7:  [("L", "B")],
	8:  [("B", "L")], 9:  [("B", "T")], 11: [("B", "R")],
	12: [("R", "L")], 13: [("R", "T")], 14: [("T", "L")],
}


def fields(im):
	"""Split the image into a black-ink and a cyan-ink coverage field.

	Returned as coverage rather than a boolean mask so the contour can be
	placed at the 50% crossing between samples: the artwork is
	antialiased, and rounding each pixel to in-or-out first would throw
	away exactly the sub-pixel information that keeps a traced curve
	smooth."""
	w, h = im.size
	px = im.load()
	blk = [[0.0] * (w + 2) for _ in range(h + 2)]
	cyn = [[0.0] * (w + 2) for _ in range(h + 2)]

	for y in range(h):
		for x in range(w):
			r, g, b, a = px[x, y]
			if a == 0:
				continue
			cov = a / 255.0
			# Black is (0,0,0) and cyan is (0,168,224), so along the
			# boundary where they meet the blue channel alone gives the
			# mix. Both fields cross 0.5 at the same place, so the two
			# traced outlines meet exactly -- no seam, no overlap.
			c = min(1.0, b / 224.0)
			blk[y + 1][x + 1] = cov * (1.0 - c)
			cyn[y + 1][x + 1] = cov * c

	return blk, cyn, w, h


def contours(g, w, h, iso=0.5):
	"""Marching squares over the padded field, chained into closed rings."""
	adj = {}

	def key(p):
		return (round(p[0], 4), round(p[1], 4))

	def add(p, q):
		a, b = key(p), key(q)
		if a == b:
			return
		adj.setdefault(a, []).append(b)
		adj.setdefault(b, []).append(a)

	for i in range(h + 1):
		for j in range(w + 1):
			tl, tr = g[i][j], g[i][j + 1]
			br, bl = g[i + 1][j + 1], g[i + 1][j]
			idx = ((1 if tl >= iso else 0) | (2 if tr >= iso else 0) |
			       (4 if br >= iso else 0) | (8 if bl >= iso else 0))
			if idx == 0 or idx == 15:
				continue

			x0, y0 = j - 0.5, i - 0.5

			def lerp(a, b):
				d = b - a
				return 0.5 if d == 0 else (iso - a) / d

			pts = {
				"T": (x0 + lerp(tl, tr), y0),
				"R": (x0 + 1.0, y0 + lerp(tr, br)),
				"B": (x0 + lerp(bl, br), y0 + 1.0),
				"L": (x0, y0 + lerp(tl, bl)),
			}

			if idx in (5, 10):
				# Saddle: the cell is ambiguous, so let the centre
				# sample decide which pair of corners is connected.
				mid = (tl + tr + br + bl) / 4.0
				joined = (mid >= iso)
				if (idx == 5) == joined:
					segs = [("L", "B"), ("T", "R")]
				else:
					segs = [("L", "T"), ("B", "R")]
			else:
				segs = CASES[idx]

			for a, b in segs:
				add(pts[a], pts[b])

	rings = []
	used = set()
	for start in adj:
		for first in adj[start]:
			if (start, first) in used or (first, start) in used:
				continue
			ring = [start]
			prev, cur = start, first
			used.add((prev, cur))
			used.add((cur, prev))
			while cur != start:
				ring.append(cur)
				nxt = None
				for cand in adj.get(cur, ()):
					if (cur, cand) not in used:
						nxt = cand
						break
				if nxt is None:
					break
				used.add((cur, nxt))
				used.add((nxt, cur))
				prev, cur = cur, nxt
			if len(ring) >= 4:
				rings.append(ring)
	return rings


def area(pts):
	s = 0.0
	n = len(pts)
	for i in range(n):
		x1, y1 = pts[i]
		x2, y2 = pts[(i + 1) % n]
		s += x1 * y2 - x2 * y1
	return abs(s) / 2.0


def dp(pts, eps):
	"""Douglas-Peucker on an open chain."""
	if len(pts) < 3:
		return list(pts)
	ax, ay = pts[0]
	bx, by = pts[-1]
	dx, dy = bx - ax, by - ay
	den = math.hypot(dx, dy)
	worst, wi = -1.0, 0
	for i in range(1, len(pts) - 1):
		px, py = pts[i]
		if den == 0:
			d = math.hypot(px - ax, py - ay)
		else:
			d = abs(dy * px - dx * py + bx * ay - by * ax) / den
		if d > worst:
			worst, wi = d, i
	if worst <= eps:
		return [pts[0], pts[-1]]
	return dp(pts[:wi + 1], eps)[:-1] + dp(pts[wi:], eps)


def simplify_ring(ring, eps):
	# Split the ring at its two most distant points so neither becomes a
	# hinge the simplifier is forbidden to move.
	n = len(ring)
	ax, ay = ring[0]
	far, fi = -1.0, 0
	for i in range(1, n):
		d = math.hypot(ring[i][0] - ax, ring[i][1] - ay)
		if d > far:
			far, fi = d, i
	a = dp(ring[:fi + 1], eps)
	b = dp(ring[fi:] + [ring[0]], eps)
	out = a[:-1] + b[:-1]
	return out if len(out) >= 3 else ring


def to_path(rings, prec=1):
	"""Emit rings as smooth cubics, keeping genuine cusps sharp."""
	def f(v):
		s = ("%%.%df" % prec) % v
		s = s.rstrip("0").rstrip(".") if "." in s else s
		return "0" if s in ("-0", "") else s

	out = []
	for pts in rings:
		n = len(pts)
		tan_out, tan_in = [], []
		for i in range(n):
			pp, p, pn = pts[(i - 1) % n], pts[i], pts[(i + 1) % n]
			v1 = (p[0] - pp[0], p[1] - pp[1])
			v2 = (pn[0] - p[0], pn[1] - p[1])
			turn = abs(math.degrees(math.atan2(
				v1[0] * v2[1] - v1[1] * v2[0],
				v1[0] * v2[0] + v1[1] * v2[1])))

			def unit(v):
				m = math.hypot(*v)
				return (0.0, 0.0) if m == 0 else (v[0] / m, v[1] / m)

			if turn > CORNER_DEG:
				tan_in.append(unit(v1))
				tan_out.append(unit(v2))
			else:
				t = unit((pn[0] - pp[0], pn[1] - pp[1]))
				tan_in.append(t)
				tan_out.append(t)

		d = ["M%s %s" % (f(pts[0][0]), f(pts[0][1]))]
		for i in range(n):
			p, q = pts[i], pts[(i + 1) % n]
			seg = math.hypot(q[0] - p[0], q[1] - p[1]) / 3.0
			j = (i + 1) % n
			c1 = (p[0] + tan_out[i][0] * seg, p[1] + tan_out[i][1] * seg)
			c2 = (q[0] - tan_in[j][0] * seg, q[1] - tan_in[j][1] * seg)
			d.append("C%s %s %s %s %s %s" % (
				f(c1[0]), f(c1[1]), f(c2[0]), f(c2[1]), f(q[0]), f(q[1])))
		d.append("Z")
		out.append("".join(d))
	return "".join(out)


def build(field, w, h, eps, min_area):
	rings = [r for r in contours(field, w, h) if area(r) >= min_area]
	return [simplify_ring(r, eps) for r in rings]


def bbox(groups):
	xs, ys = [], []
	for rings in groups:
		for r in rings:
			for x, y in r:
				xs.append(x)
				ys.append(y)
	return min(xs), min(ys), max(xs), max(ys)


def emit(rings_dark, rings_cyan, label, cls):
	"""Wraps two traced groups as one inline SVG, normalized to origin."""
	x0, y0, x1, y1 = bbox([rings_dark, rings_cyan] if rings_cyan else [rings_dark])
	pad = 2.0

	def shift(groups):
		return [[(x - x0 + pad, y - y0 + pad) for x, y in r] for r in groups]

	body = '<path class="%s-mark" fill-rule="evenodd" d="%s"/>' % (cls, to_path(shift(rings_dark)))
	if rings_cyan:
		body += '\n<path class="%s-word" fill-rule="evenodd" d="%s"/>' % (
			cls, to_path(shift(rings_cyan)))
	return ('<svg class="%s" viewBox="0 0 %.1f %.1f" role="img" aria-label="%s" '
	        'xmlns="http://www.w3.org/2000/svg">\n<title>%s</title>\n%s\n</svg>\n' % (
		        cls, (x1 - x0) + 2 * pad, (y1 - y0) + 2 * pad, label, label, body))


def read_tile():
	"""The tile's innards, straight from the drawn source."""
	svg = open(SRC_TILE).read()
	m = re.search(r"<svg[^>]*>(.*)</svg>", svg, re.S)
	if m is None:
		sys.stderr.write("%s: no svg element found\n" % SRC_TILE)
		sys.exit(1)
	return m.group(1).strip()


def main():
	inner = read_tile()

	# The mark alone, for the menu bar. The word is not in it: at menu
	# bar height the script would be an unreadable smudge, and the mark
	# is the part that identifies the page from a glance at a tab strip.
	# Inline in the document so its two classes resolve against the
	# page's own custom properties.
	open(OUT_MARK, "w").write(
		'<svg class="mark" viewBox="0 0 256 256" role="img" aria-label="cix" '
		'xmlns="http://www.w3.org/2000/svg">\n%s\n</svg>\n' % inner)

	open(OUT_FAVICON, "w").write(
		'<svg viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">\n'
		'<style>\n'
		'.tile-bg { fill: %s } .tile-fg { fill: %s }\n'
		'@media (prefers-color-scheme: dark) {\n'
		'.tile-bg { fill: %s } .tile-fg { fill: %s }\n'
		'}\n'
		'</style>\n%s\n</svg>\n'
		% (FAVICON_LIGHT[0], FAVICON_LIGHT[1], FAVICON_DARK[0], FAVICON_DARK[1], inner))

	for path in (OUT_MARK, OUT_FAVICON):
		sys.stderr.write("  %-28s %6d bytes\n" % (path, len(open(path).read())))


main()
