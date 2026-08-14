#include <stdint.h>
#include <stdio.h>

#include "ui/ui_button_gesture.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    d1l_ui_button_gesture_t gesture = {0};

    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 100U) ==
          D1L_UI_BUTTON_ACTION_ACTIVITY);
    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 150U) ==
          D1L_UI_BUTTON_ACTION_NONE);
    CHECK(d1l_ui_button_gesture_update(&gesture, false, false, 200U) ==
          D1L_UI_BUTTON_ACTION_NONE);
    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 300U) ==
          D1L_UI_BUTTON_ACTION_ADVERT);
    CHECK(d1l_ui_button_gesture_update(&gesture, false, false, 350U) ==
          D1L_UI_BUTTON_ACTION_NONE);

    CHECK(d1l_ui_button_gesture_update(&gesture, true, true, 1000U) ==
          D1L_UI_BUTTON_ACTION_WAKE);
    CHECK(!gesture.first_press_pending);
    CHECK(d1l_ui_button_gesture_update(&gesture, false, false, 1050U) ==
          D1L_UI_BUTTON_ACTION_NONE);

    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 2000U) ==
          D1L_UI_BUTTON_ACTION_ACTIVITY);
    CHECK(d1l_ui_button_gesture_update(&gesture, false, false, 2100U) ==
          D1L_UI_BUTTON_ACTION_NONE);
    CHECK(d1l_ui_button_gesture_update(&gesture, false, false, 2600U) ==
          D1L_UI_BUTTON_ACTION_NONE);
    CHECK(!gesture.first_press_pending);
    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 2700U) ==
          D1L_UI_BUTTON_ACTION_ACTIVITY);

    gesture = (d1l_ui_button_gesture_t){0};
    CHECK(d1l_ui_button_gesture_update(
              &gesture, true, false, UINT32_MAX - 200U) ==
          D1L_UI_BUTTON_ACTION_ACTIVITY);
    CHECK(d1l_ui_button_gesture_update(
              &gesture, false, false, UINT32_MAX - 100U) ==
          D1L_UI_BUTTON_ACTION_NONE);
    CHECK(d1l_ui_button_gesture_update(&gesture, true, false, 100U) ==
          D1L_UI_BUTTON_ACTION_ADVERT);

    puts("ui_button_gesture_test: ok");
    return 0;
}
