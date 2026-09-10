# Splat

A painting desktop toy for Wayland — spray-wayland and splat-wayland
recombined into the one program they always wanted to be.

Runs as a borderless, fully transparent fullscreen overlay on top of your
desktop, with two tools sharing one canvas:

- **SPLAT** — click (or drag) to throw thick gooey enamel blobs: flat mats of
  paint a few millimeters proud of the glass, glossy where their edges face
  the light, merging when they touch.
- **SPRAY** — hold the left button to mist translucent paint onto the glass;
  the longer you hover the more opaque it gets, and colors mix in proportion.

## How the two paints layer

There is no layer bookkeeping — just one rule with painter's ordering as its
consequence. Spray lives in its own density model (4 floats per pixel:
density plus density-weighted color sums; opacity is `1 - exp(-density)`)
and is composited over the shaded splat mat, so mist dusts the enamel and
heavy soak covers it. The splat model is a persistent metaball field plus a
color map (touching blobs merge; shading is a pure function of both). When a
blob lands it wipes the mist inside its own silhouette — the enamel buries
what it falls on — so any mist visible over a blob is, by construction,
newer than that blob. The eraser thins mist and dissolves enamel alike; the
receding enamel silhouette re-glosses on its own because shading is pure.

## Controls

    Left button        spray (hold) / splat (click or drag)
    Scroll wheel       tool size (the cursor grows/shrinks to match)
    Right click        circular menu: colors, SPRAY, SPLAT, ERASER, GHOST,
                       CLEAR, QUIT — a toggle light marks the active mode;
                       release outside the ring to cancel. Drag past the rim
                       over a color to repaint that slot with the HSL field
    1-7                pick a color        E    toggle eraser
    T                  switch tool         C    clear the canvas
    [ / ]              tool size
    Space              toggle click-through (paint ghosts to 40%; the last
                       tool in hand parks as a badge in the top-right
                       corner — click it to pick it back up)
    Esc / Q / Ctrl-C   quit

Editing a palette slot's color (drag past the menu rim) repaints that slot's
splats — enamel is "the slot's color". Spray mist keeps its mixed pigment;
it left the can long ago.

## Building

Needs Wayland + EGL/GLESv2 and PipeWire development packages, plus
`wayland-scanner`. The native PipeWire spray-can hiss is managed by
`libtoyaudio`. Also uses the repository's shared
`ring-menu` (the circular menu) and `ace-packaging` (the Ace
menu category); override with `RINGMENU_DIR=` / `ACE_DIR=`.

    make            # splat + the paintbrush icons
    make run
    make test       # offscreen engine test: sprays, splats, layering,
                    # erasing, cursors — writes PNGs and checks the models

Install with `make install` at the repository root, which builds and
installs the `ace-toys` package.

## Lineage

The overlay skeleton (EGL alpha surface, lazy dirty-rect rendering,
buffer-age partial repaints, ring menu, GHOST mode) and the spray engine
come from spray-wayland; the metaball splat engine, gloss shading, brush
cursor, and app icon come from splat-wayland; both descend from the same
original fork. This repo supersedes both toys.
