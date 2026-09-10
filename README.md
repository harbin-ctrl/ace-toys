# Ace

Three interactive desktop toys for Wayland.  Coded, tested, and optimized on a Raspberry Pi 400, but they will run on any modern Linux Wayland system.

## Included Toys

1. **Splat** (`splat`): transparent fullscreen paint overlay with two
   tools on one canvas — SPLAT throws flat glossy enamel blobs, SPRAY is your own graffiti-ready spraycan!
2. **Poingo** (`poingo`): a ball bounces around your desktop with
   a crashing boing sound.  Retro memories!
3. **Balloons** (`balloons`): colorful balloons drift up the desktop,
   with occasional breezes.  Sometimes, the storms come. Pop them if you want to!

Right click for the menu.  You can "GHOST" any of them, they will spookily get on with it in the background while you work.  Click on the big icon in the upper right of your screen to play with them again!

## Build and Installation

```bash
make -j4            # build all toys
make install        # build the ace-toys package and install it
```
