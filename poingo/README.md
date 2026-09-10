# Poingo

Poingo is a bouncing-ball toy and demo homage: a ball that bounces around your
desktop as a native Wayland overlay.

It is designed to feel simple and direct while still adapting to modern hardware
realities such as variable refresh rates, external displays, and high-latency
audio paths.

## Features
- Up to six independently styled balls with hollow-shell collisions.
- A pure-Wayland client: its own `wl_surface` + EGL/GLES renderer, native
  `wl_pointer`/`wl_keyboard` input, and `xkbcommon` for the keymap. No SDL, no
  toolkit.
- Predictive audio scheduling for modern output latency.
- Audio directly through PipeWire; the shared mixer/DSP lives in `libtoyaudio`, the pie
  menu in `libringmenu`.
- A built-in `nostalgia mode`.

## Build
Poingo requires a Wayland compositor. Run:

```sh
make          # builds ./poingo
./poingo      # bounce
```

Development packages: `wayland-client`, `wayland-egl`, `egl`, `glesv2`,
`xkbcommon`, and PipeWire (`libpipewire-0.3`; audio).
Install with `make install` at the repository root, which builds and
installs the `ace-toys` package; its `debian/control` lists the build
dependencies.

## Options
```
--mute                 Start with audio muted
--light-color <color>  Light ball color (R,G,B or #RRGGBB)
--dark-color <color>   Dark ball color (R,G,B or #RRGGBB)
--start-size <scale>   Initial ball size (0.25 to 1.5)
--debug                Print FPS to stderr and show the FPS HUD
--help, -h             Show help
```

## Controls
- Right-click a ball for `+ BALL`, `- BALL`, and its style controls
- Drag a ball; use the wheel or `[` / `]` while holding it to resize it
- `M` mute
- `,` / `.` or `<` / `>` speed down / up
- `C` randomize the ball under the pointer
- `D` toggle debug HUD
- `SPACE` toggle help
- `A` / `P` style the ball under the pointer

## Audio and latency
Most of Poingo's audio machinery exists for one reason: the ball strikes a wall
at a *known instant*, and a bounce sound that arrives even a couple hundredths of
a second late reads as wrong. A continuous or fire-and-forget sound can absorb
that slack; a sharp impact synced to a visible collision cannot.

With one ball, Poingo plays wall sounds *ahead of time*. With interacting balls,
impacts play immediately because another ball can invalidate a prediction. Its
native PipeWire stream reads the stream timing
state and combines the queued samples, buffered samples, and remaining graph
delay into an end-to-end output-latency estimate. The predictive scheduler
queues each bounce that much earlier, so the sound leaves the speakers exactly
as the ball meets the wall. A small fixed onset-compensation trim accounts for
the sound's own attack, so the *perceived* impact — not the first sample — lands
on the frame.

Poingo requests interleaved 48 kHz floating-point stereo with an explicit
`FL,FR` channel map. The shared DSP and mixer live in `libtoyaudio`; PipeWire is
the device and latency-measurement layer.

The sibling toys (splat, balloons) carry none of this: a spray hiss is a
continuous stream and a balloon pop is fire-and-forget, so neither has a visual
instant to hit, and a few milliseconds of slack is inaudible.

## Notes
Poingo follows a simple priority order under load:
1. Movement and collision timing.
2. Rotation and visual smoothness.
3. Resolution and secondary presentation details.

The project intentionally keeps the visible behavior simple, even where the
underlying platform adaptation has to be more complex.
