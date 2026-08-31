# 0011 — the dashboard follows the Cix brand system

## Status

Accepted. Supersedes the ad-hoc palette and type stack the dashboard
shipped with.

## Context

The dashboard was built before the brand system was available to it. It
used a system sans stack, a blue accent (`#0b64c4`), and material-design
status colours — none of which came from anywhere. The centred wordmark
was worse: it was set in Pacifico, a brush script, and shipped as traced
outlines.

`itdlabs/cix` `docs/brand/brand-guidelines.md` v1.0 defines the system,
and carries an AUTHORITY clause: where a local design preference
conflicts with a principle in that document, the principle wins, and
exceptions must be intentional, documented and reversible.

A brush script is not a near miss. The brand's typography section names
"a disciplined grotesk" — Inter, with Inter Display for large headlines.

## Decision

### 1. Inter, self-hosted

`web/InterVariable.woff2`, 337 KB, served locally.

A fallback stack alone would not have delivered it. The guidelines list
"Noto Sans / system sans stack" only for *where Inter is unavailable*,
and Inter is installed almost nowhere — so naming it in a stack would
have quietly kept rendering the system font, which is the thing being
fixed.

One variable file rather than four static weights, because it carries
the `opsz` axis. That axis *is* Inter Display — the optical cut for
large text — so a single file covers both roles the brand asks for.

Self-hosted, not from a CDN, for the same reason every other asset here
is local: an operations dashboard has to work when other things do not.

### 2. The wordmark is type, not artwork

Set as live text in Inter at weight 700, lower case, with tracking left
close to normal — the guidelines call out avoiding ultra-tight tracking
at display sizes, and Inter's optical-size axis already compensates as
the size grows.

Deliberately **not** traced into outlines, though the tooling to do that
exists here and is used for the mark. The brand system states that final
logo artwork is still open and that official geometry must not be
invented from a text mockup. Shipping a typeset name as paths would be
doing precisely that, and would look like a settled wordmark. As live
text it is honestly what it is, and stays selectable and searchable.

`cix` takes the text colour and `-cache` the accent, which is the lockup
shape the brand describes: wordmark plus descriptor.

### 3. The palette is the brand palette

Carbon, Ferrite, Machined, Paper, Nickel, Copper, Phosphor, Amber and
Fault, in the one token block the stylesheet already kept them in.

Copper is the accent. It replaces a blue that meant nothing, and the
brand is explicit that Copper is the primary brand accent rather than a
universal status colour — so it is used for focus, selection and
emphasis, and not for state.

### 4. The one exception: light mode is derived, not inverted

Every signal colour in the brand palette is built for a dark ground.
Measured against Paper `#F3F0E7`:

| | on Carbon | on Paper |
|---|---|---|
| Nickel | 7.49:1 | **2.28:1** |
| Copper | 6.26:1 | **2.73:1** |
| Phosphor | 13.64:1 | **1.25:1** |
| Amber | 10.07:1 | **1.70:1** |
| Fault | 5.48:1 | **3.12:1** |

Dark mode therefore uses the brand hexes unchanged. Light mode cannot:
four of the five are below 3:1, which is unreadable rather than merely
off-tone.

So each light value is the brand colour darkened along its own hue,
keeping hue and saturation, until it clears 4.5:1 on Paper. Copper
`#D87945` becomes `#AD5424` at 4.53:1; Nickel `#98A2A8` becomes
`#646F76` at 4.52:1; and so on.

This is an exception of exactly the kind the AUTHORITY clause provides
for — intentional, documented, reversible — and it is also what the
Product UI section instructs directly: *never design dark mode first and
generate light mode by simple inversion*. Reversing it means editing one
token block.

## Consequences

337 KB of font on first load, cached thereafter, on a LAN dashboard.
That is the price of the brand face actually being the brand face, and
it is paid once.

The menu bar mark remains the tile artwork supplied for it. Worth
flagging against the brand's own criteria, which say a logo must work in
one colour and **must not depend on being enclosed in a rounded-square
app icon**. The current tile does depend on that. It is not changed here
because it is supplied artwork and the brand system says final logo work
is still open — but it is a known divergence, not an oversight.

The exact light-mode values are derived, so if the brand system later
publishes its own light-surface variants, those replace these rather
than being reconciled with them.
