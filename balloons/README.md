# Balloons (Wayland Demo)

Plays APNG balloons floating up on the desktop: no window, no border, transparent
wherever the APNG is transparent, and click-through everywhere except the
sprites themselves. 30 big balloons in eight colors drift up the desktop, each
inflated to its own random size (a ~20% spread) so no two look stamped.

Occasionally a breeze picks up and blows the flotilla left or right for a few seconds;
balloons blown off one side wrap in from the other, and each balloon answers
the wind a little differently. Every 7 to 11 minutes a storm rolls in on its
own — the same long, dark, balloon-claiming gale as the menu's STORM.

Storms carry lightning. No bolt is drawn: a strike is the dark veil snapping
bright for a few strokes, washing the desktop faintly white, usually timed to
a casualty's pop — the storm visibly takes that balloon. Far strikes flash
dimmer than close ones, and a soft, distant roll of thunder follows each one —
later and quieter the farther the strike, the sound lagging the light the way
it does in the sky.

It uses the Poingo "freerange" trick: a maximized `xdg_toplevel` with a NULL
opaque region and an ARGB EGL surface, with the Wayland input region
restricted to the sprites' bounding boxes each frame.

Balloon bodies, strings, and pop bursts are drawn procedurally in C when the program starts, so the executable needs no external sprite files and the build has no asset-generation stage. Each balloon *body* is then scaled *down* to its own random size at draw time; the pop bursts share the bodies' native size so a popped balloon's burst matches its size.

The hanging **string is not part of the balloon body** — it is one shared, color-independent animation drawn as its own constant-size quad, hung from the tie-off under each body. That is deliberate: the toy simulates balloons blown up to different sizes, and a piece of string is the same length whichever balloon it is tied to, so it must not scale (or stretch) with the body. This is also why any future body *ovalness* is a safe, free per-balloon knob — the string is decoupled and never distorts.

## Build

Install the build dependencies in `debian/control` (`dpkg-checkbuilddeps`
names any that are missing), then run `make` from the repository root. To
install, run `make install` there; it builds and installs the `ace-toys`
package.

## Usage

Simply run:

```bash
balloons
```

Controls:
- **Left-click** a balloon to bop it (it will bob and react to the poke, with a soft thump).
- **Hold and drag** to grab and carry it — it hangs from the pointer until you let go, then resumes floating.
- **Right-click** a balloon to pop it (with a bang).
- **Middle-click** to quit.
- **Esc** or **Q** also quit when the surface has keyboard focus, as does **Ctrl-C** in the terminal.
- **M** toggles mute; **Up/Down** adjust the master volume (keyboard focus required).

## Sound

The audio subsystem is lifted wholesale from Poingo, with balloon-specific
sounds synthesized at runtime from the same idioms: popping gives a very
short, bright crack with a tight tail; bopping gives a darker, longer,
much quieter thump. Both are
finished with Poingo's hall reverb shrunk to a few milliseconds — too fast
to register as a room, it just reads as physical "feel". Each sound is
rendered into a detuned two-layer sample pair and replayed through a
16-voice snapshot mixer with master volume and soft clipping. On quit the
window vanishes immediately and anything still playing — a mid-gust
breeze included — is squelched with a fast fade rather than cut or left
to ring out.

Each breeze also gets a whoosh, synthesized fresh per gust: its envelope
is the exact easing curve the breeze physics drives the wind through, so
the swell and die-down you hear are precisely what the balloons do. The
sound and the motion are aligned by inverting Poingo's predictive-audio
trick — Poingo predicts its physics and sends sound early to beat the
output latency; balloons owns the gust clock, so it sends the whoosh on
time and holds the visible wind back by the measured latency instead.
Nobody knows when a breeze was "supposed" to start, so the hold is free —
and on high-latency outputs like Bluetooth it keeps the wind and the
whoosh landing together.

Thunder is procedural too: four deterministic, seeded variations are generated
at startup from overlapping slow swells, independently filtered low-frequency
noise bands, broad resonances, gentle diffusion, and soft saturation. The
strike's distance sets how long the thunder lags the flash and how soft it
lands.

Output goes through the native PipeWire stream managed by `libtoyaudio`.
Balloons sends explicit `FL,FR` floating-point stereo at 48 kHz and reads
PipeWire's queued, buffered, graph, and device delay for the gust timing
described above.

## Performance notes

- **Damage hints.** Renders the full buffer but submits only the
  changed rectangles (each sprite's old ∪ new position) via
  `eglSwapBuffersWithDamage{EXT,KHR}`, so the compositor re-composites just
  those tiles instead of the whole screen. Big win when sprites are sparse.
- **Adaptive partial repaint.** With `EGL_EXT_buffer_age` the client only
  repaints the union of the buffer's stale sprite rects — but only when that
  union is under half the screen. On V3D a scissored clear forces a load of
  every tile in the rect before the store, while an unscissored clear is a
  free fast-clear with no loads; measured on a Pi 400, full-clear beats a
  large scissor by ~20%, so the scissor is reserved for sparse frames.
- **All sound synthesis runs on a worker thread.** A gust's whoosh takes
  ~30 ms to synthesize (two detuned multi-second layers plus reverb) — two
  dropped frames if done on the render thread, which it once was. The gust
  physics already waits for the audio latency, so it simply waits for
  synthesis the same way (see below). Pop/thump buffers pregenerate on the
  worker at startup; no sample is ever generated while holding the mixer
  lock, so the audio callback can't underrun against synthesis.
- **Decoded frames are freed after upload** (`malloc_trim` included, since the
  ~117 KB frames sit under glibc's mmap threshold and would otherwise linger
  on the heap): 106 MB → 76 MB RSS.

## Notes

- The sprite layer is an ordinary toplevel, so clicking another window raises
  that window above the sprites — same behavior as Poingo freerange.
