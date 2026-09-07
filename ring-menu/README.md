# ringmenu

A tiny circular (pie) menu for full-screen desktop toys. One `.c`/`.h` pair,
no dependencies beyond libm. `make` builds `libringmenu.a`; toys add this
directory to their include path and link the archive:

```make
RINGMENU_DIR ?= $(HOME)/ring-menu
CFLAGS += -I$(RINGMENU_DIR)
$(RINGMENU_DIR)/libringmenu.a:
	$(MAKE) -C $(RINGMENU_DIR)
```

The library never talks to the display. The toy feeds it pointer events and
composites the pixels it renders over its own straight-alpha RGBA canvas, so
it works the same under Wayland, SDL, or anything else.

## Behavior

- Right button press: call `ringmenu_open()` at the pointer. The menu is a
  ring of equal wedges around the pointer, sized just big enough to hold its
  content, nudged to stay on screen.
- Drag with the right button held: `ringmenu_motion()` highlights the wedge
  under the pointer.
- Release the right button (or press the left button) over a wedge: that item
  is selected — `ringmenu_button()` returns its 1-based index.
- Release over the center hole or outside the ring, or press the middle
  button: cancelled — returns `0` (`RINGMENU_CANCELLED`).
- While nothing has been decided, events return `RINGMENU_NONE` (-1).

## Items

Up to `RINGMENU_MAX_ITEMS` (16) items, each either:

- a label — at most 10 characters from the built-in small-caps pixel font
  (26 letters, 10 digits, dash, space; case folds together), or
- an RGBA image up to 64x64.

Both are copied by `ringmenu_create()`.

## Grayed items

Set `item.state` to `RINGMENU_ITEM_GRAYED`, or call
`ringmenu_set_item_state()`. Grayed items are dimmed, including supplied
images, cannot highlight or select, and leave the menu open when clicked.
The setter returns false when asked to gray a group's selected item.

## Groups

`ringmenu_create_grouped()` accepts contiguous, non-overlapping
`RingMenuGroup` ranges. Each range has one enabled `selected` item; indices
are zero-based. Selecting another member updates that group automatically.
Read it with `ringmenu_group_selected()` or synchronize an external mode with
`ringmenu_set_group_selected()`.

Each group has a tinted outer-rim arc. Its selected item has an inner radio
dot. A group needs at least two items.

## Toggle lights

Any slot can carry an optional **toggle light** — a small indicator centered on
the slot, between its label and the menu center. A slot either has one or not,
and if it has one it is either off or on:

- **off** — a dull, dark glassy orb inside a dark-silver band, and
- **on** — that same band around a bright yellow-gold glow that spills a little
  past the band into the slot.

Set a slot's initial state with `item.led` (`RINGMENU_LED_NONE` /`_OFF` /`_ON`)
and change it any time with `ringmenu_set_led(menu, index, state)`. Room for the
light is reserved at create time for any slot that started non-`NONE`, so a slot
that may ever light should be created `OFF` (or `ON`); toggling among the three
states afterward never moves the layout. Typical use is a status light on a
mutually-exclusive mode (light the active one, `NONE` the rest) or a plain
on/off state (a mute or a running effect).

## Drawing

Poll `ringmenu_take_dirty()`; when true, re-composite with `ringmenu_draw()`
over the region reported by `ringmenu_rect()`. The rect stays valid after the
menu closes so you can restore the pixels underneath. Pixels are RGBA8888,
straight alpha, bytes in memory order R,G,B,A.

## Sketch

```c
RingMenuItem items[] = {
    { .label = "small" },
    { .label = "large" },
    { .image = pixels, .image_w = 24, .image_h = 24 },
};
RingMenu *menu = ringmenu_create(items, 3);

// in the pointer handlers:
if (ringmenu_is_open(menu)) {
    int r = motion ? ringmenu_motion(menu, x, y)
                   : ringmenu_button(menu, RINGMENU_BTN_..., pressed);
    if (r >= 0) { /* closed: restore rect; r == 0 cancel, else item r */ }
} else if (right_button_pressed) {
    ringmenu_open(menu, x, y, screen_w, screen_h);
}

// in the frame loop, while open:
if (ringmenu_take_dirty(menu)) { /* redraw rect */ }
ringmenu_draw(menu, canvas, w, h, 0, 0);
```

## Color field

For menus whose leading slots are colors, the color-field extension turns
each color slot into a drag-out HSL picker:

- `ringmenu_color_drop()` renders a slot's icon — a paint drop of the current
  color whose tail points at the menu center (swap it live with
  `ringmenu_update_image()`).
- While the menu is open and the pointer crosses the outer rim over a color
  slot (`ringmenu_field_slot()`), draw the field with `ringmenu_field_draw()`
  and map the pointer to a replacement color with `ringmenu_field_color()`
  (hue along the arc, lightness across it).
- `ringmenu_field_radius()` bounds the field for damage/scratch rects.

The library owns the geometry, rendering, and pointer-to-color mapping; the
toy owns which slots are colors, when to lock onto one, and what committing
a color means.

spray (`spray-wayland`) and Poingo both integrate this way: right-click menu,
color drops in the leading slots, drag past the rim to repaint a slot.
