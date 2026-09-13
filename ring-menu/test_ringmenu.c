#include "ringmenu.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void move_to(RingMenu *menu, int slot, int count) {
    int cx, cy;
    float r0, r1;
    ringmenu_geometry(menu, &cx, &cy, &r0, &r1);
    float angle = -3.14159265358979323846f / 2.0f +
                  (float)slot * 2.0f * 3.14159265358979323846f / (float)count;
    float r = (r0 + r1) / 2.0f;
    ringmenu_motion(menu, (int)lroundf((float)cx + r * cosf(angle)),
                    (int)lroundf((float)cy + r * sinf(angle)));
}

static void test_grayed_item_stays_open(void) {
    RingMenuItem items[] = {
        {.label = "one"},
        {.label = "two", .state = RINGMENU_ITEM_GRAYED},
    };
    RingMenu *menu = ringmenu_create(items, 2);
    assert(menu);
    ringmenu_open(menu, 100, 100, 200, 200);
    move_to(menu, 1, 2);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == RINGMENU_NONE);
    assert(ringmenu_is_open(menu));
    assert(ringmenu_set_item_state(menu, 1, RINGMENU_ITEM_ACTIVE));
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == 2);
    ringmenu_destroy(menu);
}

static void test_group_selects_one(void) {
    RingMenuItem items[] = {{.label = "one"}, {.label = "two"}, {.label = "three"}};
    RingMenuGroup groups[] = {{.first = 0, .count = 3, .selected = 0}};
    RingMenu *menu = ringmenu_create_grouped(items, 3, groups, 1);
    assert(menu);
    ringmenu_open(menu, 100, 100, 200, 200);
    move_to(menu, 2, 3);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == 3);
    assert(ringmenu_group_selected(menu, 0) == 2);
    assert(!ringmenu_set_item_state(menu, 2, RINGMENU_ITEM_GRAYED));
    assert(ringmenu_set_group_selected(menu, 0, 0));
    assert(ringmenu_group_selected(menu, 0) == 0);
    assert(ringmenu_set_item_state(menu, 1, RINGMENU_ITEM_GRAYED));
    ringmenu_destroy(menu);
}

/* Touchpads and some mouse drivers deliver a right click's press and release
   together, so the menu must survive a release that never left the centre. */
static void test_click_keeps_open(void) {
    RingMenuItem items[] = {{.label = "one"}, {.label = "two"}};
    RingMenu *menu = ringmenu_create(items, 2);
    assert(menu);
    ringmenu_open(menu, 100, 100, 200, 200);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, true) == RINGMENU_NONE);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == RINGMENU_NONE);
    assert(ringmenu_is_open(menu));
    move_to(menu, 1, 2);
    assert(ringmenu_button(menu, RINGMENU_BTN_LEFT, true) == 2);
    assert(!ringmenu_is_open(menu));
    ringmenu_destroy(menu);
}

static void test_second_click_in_centre_cancels(void) {
    RingMenuItem items[] = {{.label = "one"}, {.label = "two"}};
    RingMenu *menu = ringmenu_create(items, 2);
    assert(menu);
    ringmenu_open(menu, 100, 100, 200, 200);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == RINGMENU_NONE);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, true) == RINGMENU_NONE);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == RINGMENU_CANCELLED);
    assert(!ringmenu_is_open(menu));
    ringmenu_destroy(menu);
}

static void test_drag_back_to_centre_cancels(void) {
    RingMenuItem items[] = {{.label = "one"}, {.label = "two"}};
    RingMenu *menu = ringmenu_create(items, 2);
    assert(menu);
    ringmenu_open(menu, 100, 100, 200, 200);
    move_to(menu, 0, 2);
    ringmenu_motion(menu, 100, 100);
    assert(ringmenu_button(menu, RINGMENU_BTN_RIGHT, false) == RINGMENU_CANCELLED);
    assert(!ringmenu_is_open(menu));
    ringmenu_destroy(menu);
}

int main(void) {
    test_grayed_item_stays_open();
    test_group_selects_one();
    test_click_keeps_open();
    test_second_click_in_centre_cancels();
    test_drag_back_to_centre_cancels();
    puts("ringmenu checks passed");
    return 0;
}
