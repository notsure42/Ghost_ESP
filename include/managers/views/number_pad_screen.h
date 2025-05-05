#ifndef NUMBER_PAD_SCREEN_H
#define NUMBER_PAD_SCREEN_H

#include "lvgl/lvgl.h"
#include "managers/display_manager.h"

// Define the number pad modes here
typedef enum {
    NP_MODE_AP,
    NP_MODE_STA,
    NP_MODE_LAN,
    NP_MODE_AIRTAG
} ENumberPadMode;

extern View number_pad_view;

void set_number_pad_mode(ENumberPadMode mode);

#endif // NUMBER_PAD_SCREEN_H