
#ifndef RINGMENU_H
#define RINGMENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINGMENU_MAX_ITEMS 16
#define RINGMENU_MAX_GROUPS 8
#define RINGMENU_LABEL_MAX 10
#define RINGMENU_IMAGE_MAX 64   // max image width/height in px

enum {
    RINGMENU_BTN_LEFT = 0,
    RINGMENU_BTN_RIGHT = 1,
    RINGMENU_BTN_MIDDLE = 2,
};

typedef enum {
    RINGMENU_ITEM_ACTIVE,
    RINGMENU_ITEM_GRAYED,
} RingMenuItemState;

#define RINGMENU_NONE (-1)
#define RINGMENU_CANCELLED 0

enum {
    RINGMENU_LED_NONE = 0,  
    RINGMENU_LED_OFF  = 1,  
    RINGMENU_LED_ON   = 2,  
};

typedef struct {
    const char *label;      
    const uint32_t *image;  
    int image_w, image_h;   
    int led;                
    RingMenuItemState state;
} RingMenuItem;

typedef struct {
    int first;
    int count;
    int selected;
} RingMenuGroup;

typedef struct RingMenu RingMenu;

RingMenu *ringmenu_create(const RingMenuItem *items, int count);
RingMenu *ringmenu_create_grouped(const RingMenuItem *items, int item_count,
                                  const RingMenuGroup *groups, int group_count);
void ringmenu_destroy(RingMenu *m);

void ringmenu_open(RingMenu *m, int x, int y, int bounds_w, int bounds_h);
bool ringmenu_is_open(const RingMenu *m);

int ringmenu_motion(RingMenu *m, int x, int y);
int ringmenu_button(RingMenu *m, int button, bool pressed);

void ringmenu_rect(const RingMenu *m, int *x, int *y, int *w, int *h);
int ringmenu_size(const RingMenu *m);

void ringmenu_geometry(const RingMenu *m, int *cx, int *cy, float *r0, float *r1);

void ringmenu_update_image(RingMenu *m, int index, const uint32_t *image);

void ringmenu_set_led(RingMenu *m, int index, int led);
bool ringmenu_set_item_state(RingMenu *m, int index, RingMenuItemState state);
int ringmenu_group_selected(const RingMenu *m, int group);
bool ringmenu_set_group_selected(RingMenu *m, int group, int item);

bool ringmenu_take_dirty(RingMenu *m);

void ringmenu_draw(RingMenu *m, uint32_t *dst, int dst_w, int dst_h, int dst_x, int dst_y);


#define RINGMENU_FIELD_PAD 2.0f      // gap between the rim and the field
#define RINGMENU_FIELD_WIDTH 160.0f  // radial thickness of the field

uint32_t *ringmenu_color_drop(uint8_t r, uint8_t g, uint8_t b,
                              int slot, int count, int size);
bool ringmenu_color_drop_into(uint32_t *px_buf, size_t capacity,
                              uint8_t r, uint8_t g, uint8_t b,
                              int slot, int count, int size);

float ringmenu_field_radius(const RingMenu *m);

int ringmenu_field_slot(const RingMenu *m, int x, int y);

bool ringmenu_field_color(const RingMenu *m, int slot, int x, int y,
                          uint8_t *r, uint8_t *g, uint8_t *b);

void ringmenu_field_draw(const RingMenu *m, int slot, uint32_t *dst,
                         int dst_w, int dst_h, int dst_x, int dst_y);

#endif // RINGMENU_H
