#include "e2e_base.h"
#include "ui/settings/panes/pref_fps.h"

static int fps = 30;
/* The fractional refresh rate the dropdown keeps beside the frame rate (hundredths of a Hz; 0 = none), as
 * Settings -> Basic passes it (basic.pane.c). */
static int refresh_x100 = 0;

/* The first object of @p cls under @p parent (depth first). */
static lv_obj_t *find_class(lv_obj_t *parent, const lv_obj_class_t *cls) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t) i);
        if (lv_obj_check_type(child, cls)) {
            return child;
        }
        lv_obj_t *found = find_class(child, cls);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void tap_obj(lv_obj_t *obj, int x_num, int x_den) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    fakeTap(area.x1 + (area.x2 - area.x1) * x_num / x_den, (area.y1 + area.y2) / 2);
}

void test_basic() {
    const static int options[] = {30, 60, 90, 120, 144, 240, 0};
    fps = 30;
    refresh_x100 = 0;
    lv_obj_t *dropdown = pref_dropdown_fps(app.ui.container, options, 120, &fps, &refresh_x100);

    lv_obj_set_width(dropdown, LV_PCT(100));

    fakeTap(100, 48); // Simulate a tap on the dropdown

    waitFor(100);

    fakeTap(175, 489); // Click on "Custom" option

    waitFor(100);

    /* The dialog (on the top layer): its text field starts with the current rate ("30") and takes a
     * fractional one. */
    lv_obj_t *msgbox = find_class(lv_layer_top(), &lv_msgbox_class);
    TEST_ASSERT_NOT_NULL_MESSAGE(msgbox, "The Custom FPS dialog did not open");
    lv_obj_t *field = find_class(msgbox, &lv_textarea_class);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_EQUAL_STRING("30", lv_textarea_get_text(field));

    tap_obj(field, 9, 10); // Into the field, after its text
    fakeKeyPress(SDLK_BACKSPACE);
    fakeKeyPress(SDLK_BACKSPACE);
    fakeInput("59.94");
    TEST_ASSERT_EQUAL_STRING("59.94", lv_textarea_get_text(field));

    tap_obj(lv_msgbox_get_btns(msgbox), 3, 4); // "OK", the right-hand button

    waitFor(100);

    /* The stream asks for 60 FPS, rounded to nearest so the host derives the same, and keeps 59.94 Hz. */
    UNITY_TEST_ASSERT_EQUAL_INT32(60, fps, __LINE__, "FPS value did not match expected value after interaction");
    UNITY_TEST_ASSERT_EQUAL_INT32(5994, refresh_x100, __LINE__, "The custom refresh rate was not kept");
}

void test_120fps() {
    const static int options[] = {30, 60, 90, 120, 144, 240, 0};
    refresh_x100 = 5994;   /* a custom 59.94 Hz chosen earlier */
    lv_obj_t *dropdown = pref_dropdown_fps(app.ui.container, options, 120, &fps, &refresh_x100);

    lv_obj_set_width(dropdown, LV_PCT(100));

    fakeTap(100, 48); // Simulate a tap on the dropdown

    waitFor(100);

    fakeTap(175, 400); // Click on "120 FPS" option

    waitFor(100);

    UNITY_TEST_ASSERT_EQUAL_INT32(120, fps, __LINE__, "FPS value did not match expected value after interaction");
    UNITY_TEST_ASSERT_EQUAL_INT32(0, refresh_x100, __LINE__, "A listed frame rate must clear a custom refresh rate");
}

void test_100fps() {
    const static int options[] = {30, 60, 90, 120, 144, 240, 0};
    lv_obj_t *dropdown = pref_dropdown_fps(app.ui.container, options, 100, &fps, &refresh_x100);

    lv_obj_set_width(dropdown, LV_PCT(100));

    fakeTap(100, 48); // Simulate a tap on the dropdown

    waitFor(100);

    fakeTap(175, 400); // Click on "100 FPS" option

    waitFor(100);

    UNITY_TEST_ASSERT_EQUAL_INT32(100, fps, __LINE__, "FPS value did not match expected value after interaction");
}

void initSettings(app_settings_t *settings) {
    settings->fullscreen = false;
    settings->window_state = (window_state_t) {
        .w = 800,
        .h = 720,
    };
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_basic);
    RUN_TEST(test_120fps);
    RUN_TEST(test_100fps);
    return UNITY_END();
}