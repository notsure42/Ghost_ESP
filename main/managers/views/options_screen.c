#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/number_pad_screen.h"
#include "managers/wifi_manager.h"
#include <stdio.h>

EOptionsMenuType SelectedMenuType = OT_Wifi;
int selected_item_index = 0;
lv_obj_t *root = NULL;
lv_obj_t *menu_container = NULL;
int num_items = 0;
unsigned long createdTimeInMs = 0;
static int opt_touch_start_x;
static int opt_touch_start_y;
static bool opt_touch_started = false;
static const int OPT_SWIPE_THRESHOLD_RATIO = 10;
static bool option_fired = false;
static bool option_invoked = false;

static void select_option_item(int index); // Forward Declaration

const char *options_menu_type_to_string(EOptionsMenuType menuType) {
    switch (menuType) {
    case OT_Wifi:
        return "Wi-Fi";
    case OT_Bluetooth:
        return "BLE";
    case OT_GPS:
        return "GPS";
    case OT_Settings:
        return "Settings";
    default:
        return "Unknown";
    }
}

static const char *wifi_options[] = {"Scan Access Points",
                                     "Select AP",
                                     "Scan LAN Devices",
                                     "Select LAN",
                                     "Scan All (AP & Station)",
                                     "Start Deauth Attack",
                                     "Beacon Spam - Random",
                                     "Beacon Spam - Rickroll",
                                     "Beacon Spam - List",
                                     "Start Evil Portal",
                                     "Capture Probe",
                                     "Capture Deauth",
                                     "Capture Beacon",
                                     "Capture Raw",
                                     "Capture Eapol",
                                     "Capture WPS",
                                     "Capture PWN",
                                     "TV Cast (Dial Connect)",
                                     "Power Printer",
                                     "TP Link Test",
                                     "PineAP Detection",
                                     "Scan Open Ports",
                                     "Reset AP Credentials",
                                     "Channel Congestion",
                                     "Go Back",
                                     NULL};

static const char *bluetooth_options[] = {"Find Flippers",   "Start AirTag Scanner",
                                          "Raw BLE Scanner", "BLE Skimmer Detect",
                                          "Go Back",         NULL};

static const char *gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving",   "Go Back",         NULL};

static const char *settings_options[] = {"Set RGB Mode - Stealth", "Set RGB Mode - Normal",
                                         "Set RGB Mode - Rainbow", "Go Back", NULL};


static void up_down_event_cb(lv_event_t *e) {
int direction = (int)(intptr_t)lv_event_get_user_data(e);
select_option_item(selected_item_index + direction);
}


void options_menu_create() {
    option_invoked = false;
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    display_manager_fill_screen(lv_color_hex(0x121212));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = lv_obj_create(lv_scr_act());
    options_menu_view.root = root;
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);

    menu_container = lv_list_create(root);
    lv_obj_set_size(menu_container, screen_width, screen_height - 20);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);

    const char **options = NULL;
    switch (SelectedMenuType) {
    case OT_Wifi: options = wifi_options; break;
    case OT_Bluetooth: options = bluetooth_options; break;
    case OT_GPS: options = gps_options; break;
    case OT_Settings: options = settings_options; break;
    default: options = NULL; break;
    }

    if (options == NULL) {
        display_manager_switch_view(&main_menu_view);
        return;
    }

    num_items = 0;
    int button_height = is_small_screen ? 40 : 60;
    for (int i = 0; options[i] != NULL; i++) {
        lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, options[i]);
        lv_obj_set_height(btn, button_height);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
           lv_obj_set_style_text_font(label, is_small_screen ? &lv_font_montserrat_14 : &lv_font_montserrat_16, 0);
           lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }

        lv_obj_set_user_data(btn, (void *)options[i]);
        lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)options[i]);

        num_items++;
    }

    select_option_item(0);
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));

    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static void select_option_item(int index) {
    printf("select_option_item called with index: %d, num_items: %d\n", index, num_items);

    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;

    printf("Adjusted index: %d\n", index);

    int previous_index = selected_item_index;
    selected_item_index = index;

    printf("Previous index: %d, New selected index: %d\n", previous_index, selected_item_index);

    if (previous_index != selected_item_index) {
        lv_obj_t *previous_item = lv_obj_get_child(menu_container, previous_index);
        if (previous_item) {
            lv_obj_set_style_bg_color(previous_item, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(previous_item, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(previous_item, LV_GRAD_DIR_NONE, LV_PART_MAIN);
        }
    }

    lv_obj_t *current_item = lv_obj_get_child(menu_container, selected_item_index);
    if (current_item) {
        lv_color_t orange_start = lv_color_hex(0xFF5722);
        lv_color_t orange_end = lv_color_hex(0xD81B60);
        lv_obj_set_style_bg_color(current_item, orange_start, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(current_item, orange_end, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(current_item, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(current_item, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_scroll_to_view(current_item, LV_ANIM_OFF);
    } else {
        printf("Error: Current item not found for index %d\n", selected_item_index);
    }
}

void handle_hardware_button_press_options(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            opt_touch_started = true;
            opt_touch_start_x = data->point.x;
            opt_touch_start_y = data->point.y;
            return;
        }
        if (data->state == LV_INDEV_STATE_REL && opt_touch_started) {
            int dx = data->point.x - opt_touch_start_x;
            int dy = data->point.y - opt_touch_start_y;
            opt_touch_started = false;
            int threshold = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            if (abs(dy) > threshold && abs(dy) > abs(dx)) {
                lv_obj_scroll_by(menu_container, 0, dy, LV_ANIM_OFF);
                return;
            }
            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    select_option_item(i);
                    handle_option_directly((const char *)lv_obj_get_user_data(btn));
                    return;
                }
            }
        }
        return;
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) {
            select_option_item(selected_item_index - 1);
        } else if (button == 4) {
            select_option_item(selected_item_index + 1);
        } else if (button == 1) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                if (selected_option) {
                    handle_option_directly(selected_option);
                }
            }
        }
    }
}

void option_event_cb(lv_event_t *e) {
    if (option_invoked) return;
    option_invoked = true;
    bool view_switched = false; 

    static const char *last_option = NULL;
    static unsigned long last_time_ms = 0;
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    
    if (now_ms - createdTimeInMs <= 500) {
        option_invoked = false; 
        return;
    }
    const char *Selected_Option = (const char *)lv_event_get_user_data(e);
    if (Selected_Option == last_option && now_ms - last_time_ms < 200) {
        option_invoked = false; 
        return;
    }
    last_option = Selected_Option;
    last_time_ms = now_ms;


    if (strcmp(Selected_Option, "Scan Access Points") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan All (AP & Station)") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanall");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Deauth Attack") == 0) {
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            TERMINAL_VIEW_ADD_TEXT("No APs scanned. Please run 'Scan Access Points' first.\\n");
            
        } else {
            simulateCommand("attack -d");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Scan Stations") == 0) {
        if (strlen((const char *)selected_ap.ssid) > 0) {
            display_manager_switch_view(&terminal_view);
            simulateCommand("scansta");
            view_switched = true;
        } else {
            error_popup_create("You Need to Select a Scanned AP First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Random") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Rickroll") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -rr");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan LAN Devices") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanlocal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - List") == 0) {
        if (scanned_aps) {
            display_manager_switch_view(&terminal_view);
            simulateCommand("beaconspam -l");
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan AP's First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -deauth");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Probe") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -probe");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -beacon");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Raw") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -raw");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Eapol") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -eapol");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture WPS") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -wps");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("dialconnect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Power Printer") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("powerprinter");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Evil Portal") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startportal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd -s");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -a");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -f");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Stealth") == 0) {
        simulateCommand("setsetting 1 1");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Normal") == 0) {
        simulateCommand("setsetting 1 2");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Rainbow") == 0) {
        simulateCommand("setsetting 1 3");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Go Back") == 0) {
        selected_item_index = 0;
        num_items = 0;
        menu_container = NULL;
        root = NULL;
        
        display_manager_switch_view(&main_menu_view);
        view_switched = true; 
        return; 
    } else if (strcmp(Selected_Option, "Capture PWN") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -pwn");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TP Link Test") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("tplinktest");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -r");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -skimmer");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "GPS Info") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("gpsinfo");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blewardriving");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("pineap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanports local -C");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("apcred -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Select AP") == 0) {
        if (scanned_aps) {
            set_number_pad_mode(NP_MODE_AP);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan APs First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Select LAN") == 0) {
        set_number_pad_mode(NP_MODE_LAN);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Channel Congestion") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("congestion");
        view_switched = true;
    }

    else {
        printf("Unhandled Option selected: %s\\n", Selected_Option);
        
    }

    
    if (!view_switched) {
        option_invoked = false;
    }
}

void handle_option_directly(const char *Selected_Option) {
    lv_event_t e;
    e.user_data = (void *)Selected_Option;
    option_event_cb(&e);
}

void options_menu_destroy(void) {
    if (options_menu_view.root) {
        if (menu_container) {
            lv_obj_clean(menu_container);
            menu_container = NULL;
        }

        lv_obj_clean(options_menu_view.root);
        lv_obj_del(options_menu_view.root);
        options_menu_view.root = NULL;

        selected_item_index = 0;
        num_items = 0;
    }
}

void get_options_menu_callback(void **callback) { *callback = options_menu_view.input_callback; }

View options_menu_view = {.root = NULL,
                          .create = options_menu_create,
                          .destroy = options_menu_destroy,
                          .input_callback = handle_hardware_button_press_options,
                          .name = "Options Screen",
                          .get_hardwareinput_callback = get_options_menu_callback};