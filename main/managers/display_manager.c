#include "managers/display_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include <stdlib.h>

#ifdef CONFIG_USE_CARDPUTER
#include "vendor/keyboard_handler.h"
#include "vendor/m5/m5gfx_wrapper.h"
#endif

#ifdef CONFIG_HAS_BATTERY
#include "vendor/drivers/axp2101.h"
#endif

#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif

#ifdef CONFIG_USE_7_INCHER
#include "vendor/drivers/ST7262.h"
#endif

#ifdef CONFIG_JC3248W535EN_LCD
#include "axs15231b/esp_bsp.h"
#include "axs15231b/lv_port.h"
#include "vendor/drivers/axs15231b.h"
#endif

#ifndef CONFIG_TFT_WIDTH
#define CONFIG_TFT_WIDTH 240
#endif

#ifndef CONFIG_TFT_HEIGHT
#define CONFIG_TFT_HEIGHT 320
#endif

#define LVGL_TASK_PERIOD_MS 5
static const char *TAG = "DisplayManager";
DisplayManager dm = {.current_view = NULL, .previous_view = NULL};

lv_obj_t *status_bar;
lv_obj_t *wifi_label = NULL;
lv_obj_t *bt_label = NULL;
lv_obj_t *sd_label = NULL;
lv_obj_t *battery_label = NULL;
lv_obj_t *mainlabel = NULL;

bool display_manager_init_success = false;

#define FADE_DURATION_MS 10
#define DEFAULT_DISPLAY_TIMEOUT_MS 30000

uint32_t display_timeout_ms = DEFAULT_DISPLAY_TIMEOUT_MS;

void set_display_timeout(uint32_t timeout_ms) {
  display_timeout_ms = timeout_ms;
}

#ifdef CONFIG_USE_CARDPUTER
Keyboard_t gkeyboard;

void m5stack_lvgl_render_callback(lv_disp_drv_t *drv, const lv_area_t *area,
                                  lv_color_t *color_p) {
  int32_t x1 = area->x1;
  int32_t y1 = area->y1;
  int32_t x2 = area->x2;
  int32_t y2 = area->y2;

  m5gfx_write_pixels(x1, y1, x2, y2, (uint16_t *)color_p);

  lv_disp_flush_ready(drv);
}

#endif

void fade_out_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

void fade_in_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

void rainbow_effect_cb(lv_timer_t *timer) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) {
    return;
  }


  rainbow_hue = (rainbow_hue + 5) % 360;

  lv_color_t color = lv_color_hsv_to_rgb(rainbow_hue, 100, 100);

  lv_obj_set_style_border_color(status_bar, color, 0);

  if (wifi_label && lv_obj_is_valid(wifi_label)) {
    lv_obj_set_style_text_color(wifi_label, color, 0);
  }
  if (bt_label && lv_obj_is_valid(bt_label)) {
    lv_obj_set_style_text_color(bt_label, color, 0);
  }
  if (sd_label && lv_obj_is_valid(sd_label)) {
    lv_obj_set_style_text_color(sd_label, color, 0);
  }
  if (battery_label && lv_obj_is_valid(battery_label)) {
    lv_obj_set_style_text_color(battery_label, color, 0);
  }
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    lv_obj_set_style_text_color(mainlabel, color, 0);
  }

  lv_obj_invalidate(status_bar);
}


void display_manager_fade_out(lv_obj_t *obj, lv_anim_ready_cb_t ready_cb,
                              View *view) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, obj);
  lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&anim, FADE_DURATION_MS);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_out_cb);
  lv_anim_set_ready_cb(&anim, ready_cb);
  lv_anim_set_user_data(&anim, view);
  lv_anim_start(&anim);
}

void display_manager_fade_in(lv_obj_t *obj) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, obj);
  lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&anim, FADE_DURATION_MS);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_in_cb);
  lv_anim_start(&anim);
}

void fade_out_ready_cb(lv_anim_t *anim) {
  display_manager_destroy_current_view();

  View *new_view = (View *)anim->user_data;
  if (new_view) {
    dm.previous_view = dm.current_view;
    dm.current_view = new_view;

    if (new_view->get_hardwareinput_callback) {
      new_view->input_callback(&dm.current_view->input_callback);
    }

    new_view->create();
    display_manager_fade_in(new_view->root);
    display_manager_fade_in(status_bar);
  }
}

lv_color_t hex_to_lv_color(const char *hex_str) {
  if (hex_str[0] == '#') {
    hex_str++;
  }

  if (strlen(hex_str) != 6) {
    printf("Invalid hex color format. Expected 6 characters.\n");
    return lv_color_white();
  }

  // Parse the hex string into RGB values
  char r_str[3] = {hex_str[0], hex_str[1], '\0'};
  char g_str[3] = {hex_str[2], hex_str[3], '\0'};
  char b_str[3] = {hex_str[4], hex_str[5], '\0'};

  uint8_t r = (uint8_t)strtol(r_str, NULL, 16);
  uint8_t g = (uint8_t)strtol(g_str, NULL, 16);
  uint8_t b = (uint8_t)strtol(b_str, NULL, 16);

  return lv_color_make(r, g, b);
}

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted,
  int batteryPercentage) {
// Update visibility of status icons
if (sd_card_mounted) {
lv_obj_clear_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
} else {
lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
}

if (bt_enabled) {
lv_obj_clear_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
} else {
lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
}

if (wifi_enabled) {
lv_obj_clear_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
} else {
lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
}

// Update battery icon and percentage
const char *battery_symbol;
#ifdef CONFIG_HAS_BATTERY
if (axp202_is_charging()) {
battery_symbol = LV_SYMBOL_CHARGE;
} else {
battery_symbol = (batteryPercentage > 75) ? LV_SYMBOL_BATTERY_FULL :
(batteryPercentage > 50) ? LV_SYMBOL_BATTERY_3 :
(batteryPercentage > 25) ? LV_SYMBOL_BATTERY_2 :
(batteryPercentage > 10) ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
}
#else
battery_symbol = LV_SYMBOL_BATTERY_FULL; // Default for non-battery configs
#endif
lv_label_set_text_fmt(battery_label, "%s %d%%", battery_symbol, batteryPercentage);

// Invalidate the status bar to ensure redraw
lv_obj_invalidate(status_bar);
}


void display_manager_add_status_bar(const char *CurrentMenuName) {
  status_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(status_bar, LV_HOR_RES, 20);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x333333), LV_PART_MAIN); // Dark gray background
  lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_bar, 1, LV_PART_MAIN); // Thinner border
  lv_obj_set_style_border_color(
      status_bar, hex_to_lv_color(settings_get_accent_color_str(&G_Settings)),
      LV_PART_MAIN);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

  // Left container for menu name
  lv_obj_t *left_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(left_container);
  lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
  lv_obj_align(left_container, LV_ALIGN_LEFT_MID, 5, 0); // 5px padding from left

  // Right container for status icons
  lv_obj_t *right_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(right_container);
  lv_obj_set_size(right_container, LV_SIZE_CONTENT, 20);
  lv_obj_set_flex_flow(right_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_container, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_container, 5, 0); // 5px spacing between icons
  lv_obj_align(right_container, LV_ALIGN_RIGHT_MID, -5, 0); // 5px padding from right

  // Menu name (left-aligned)
  mainlabel = lv_label_create(left_container);
  lv_label_set_text(mainlabel, CurrentMenuName);
  lv_obj_set_style_text_color(mainlabel, lv_color_hex(0x999999), 0); // Lighter gray
  lv_obj_set_style_text_font(mainlabel, &lv_font_montserrat_14, 0);

  // Pre-create all status labels in right container (hidden by default)
  sd_label = lv_label_create(right_container);
  lv_label_set_text(sd_label, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_color(sd_label, lv_color_hex(0xCCCCCC), 0); // Light gray
  lv_obj_set_style_text_font(sd_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);

  bt_label = lv_label_create(right_container);
  lv_label_set_text(bt_label, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_color(bt_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(bt_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);

  wifi_label = lv_label_create(right_container);
  lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);

  battery_label = lv_label_create(right_container);
  lv_label_set_text(battery_label, ""); // Set dynamically in update_status_bar
  lv_obj_set_style_text_color(battery_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);

  // Initial status update
  bool HasBluetooth;
#ifndef CONFIG_IDF_TARGET_ESP32S2
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif

#ifdef CONFIG_HAS_BATTERY
  uint8_t power_level;
  axp2101_get_power_level(&power_level);
  bool is_charging = axp202_is_charging();
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    is_charging ? power_level : power_level);
#else
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized, 100);
#endif
}

void display_manager_init(void) {
#ifndef CONFIG_JC3248W535EN_LCD
  lv_init();
#ifdef CONFIG_USE_CARDPUTER
  init_m5gfx_display();
#else
  lvgl_driver_init();
#endif
#endif // CONFIG_JC3248W535EN_LCD

#if !defined(CONFIG_USE_7_INCHER) && !defined(CONFIG_JC3248W535EN_LCD)
#ifdef CONFIG_IDF_TARGET_ESP32
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 5] __attribute__((aligned(
      4))); // We do this due to Dram Memory Constraints on ESP32 WROOM Modules
  static lv_color_t buf2[CONFIG_TFT_WIDTH * 5]
      __attribute__((aligned(4))); // Any other devices like ESP32S3 Etc Should
                                   // be able to handle the * 20 Double Buffer
#else
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 20] __attribute__((aligned(4)));
  static lv_color_t buf2[CONFIG_TFT_WIDTH * 20] __attribute__((aligned(4)));
#endif

  static lv_disp_draw_buf_t disp_buf;
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, CONFIG_TFT_WIDTH * 5);

  /* Initialize the display */
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = CONFIG_TFT_WIDTH;
  disp_drv.ver_res = CONFIG_TFT_HEIGHT;

#ifdef CONFIG_USE_CARDPUTER
  disp_drv.flush_cb = m5stack_lvgl_render_callback;
#else
  disp_drv.flush_cb = disp_driver_flush;
#endif
  disp_drv.draw_buf = &disp_buf;
  lv_disp_drv_register(&disp_drv);
#elif defined(CONFIG_JC3248W535EN_LCD)
  esp_err_t ret = lcd_axs15231b_init();
  if (ret != ESP_OK) {
    printf("LCD initialization failed");
    return;
  }
#else

  esp_err_t ret = lcd_st7262_init();
  if (ret != ESP_OK) {
    printf("LCD initialization failed");
    return;
  }

  ret = lcd_st7262_lvgl_init();
  if (ret != ESP_OK) {
    printf("LVGL initialization failed");
    return;
  }

#endif

  dm.mutex = xSemaphoreCreateMutex();
  if (dm.mutex == NULL) {
    printf("Failed to create mutex\n");
    return;
  }

  input_queue = xQueueCreate(10, sizeof(InputEvent));
  if (input_queue == NULL) {
    printf("Failed to create input queue\n");
    return;
  }

#ifdef CONFIG_USE_CARDPUTER
  keyboard_init(&gkeyboard);
  keyboard_begin(&gkeyboard);
#endif

#ifdef CONFIG_HAS_BATTERY
  axp2101_init();
#ifdef CONFIG_HAS_RTC_CLOCK
  pcf8563_init(I2C_NUM_1, 0x51);
#endif
#endif

  display_manager_init_success = true;

#ifndef CONFIG_JC3248W535EN_LCD // JC3248W535EN has its own lvgl task
  xTaskCreate(lvgl_tick_task, "LVGL Tick Task", 4096, NULL,
              RENDERING_TASK_PRIORITY, NULL);
#endif
  if (xTaskCreate(hardware_input_task, "RawInput", 2048, NULL,
                  HARDWARE_INPUT_TASK_PRIORITY, NULL) != pdPASS) {
    printf("Failed to create RawInput task\n");
  }
}

bool display_manager_register_view(View *view) {
  if (view == NULL || view->create == NULL || view->destroy == NULL) {
    return false;
  }
  return true;
}

void display_manager_switch_view(View *view) {
  if (view == NULL)
    return;

#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_lock(0);
#endif

  if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    printf("Switching view from %s to %s\n",
           dm.current_view ? dm.current_view->name : "NULL", view->name);

    if (dm.current_view && dm.current_view->root) {
      if (status_bar) {
        lv_obj_del(status_bar);
        wifi_label = NULL;
        bt_label = NULL;
        sd_label = NULL;
        battery_label = NULL;
        status_bar = NULL;
      }
      display_manager_fade_out(dm.current_view->root, fade_out_ready_cb, view);
    } else {
      dm.previous_view = dm.current_view;
      dm.current_view = view;

      if (view->get_hardwareinput_callback) {
        view->input_callback(&dm.current_view->input_callback);
      }

      view->create();
      display_manager_fade_in(view->root);
    }

    xSemaphoreGive(dm.mutex);
  } else {
    printf("Failed to acquire mutex for switching view\n");
  }

#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_unlock();
#endif
}

void display_manager_destroy_current_view(void) {
  if (dm.current_view) {
    if (dm.current_view->destroy) {
      dm.current_view->destroy();
    }

    dm.current_view = NULL;
  }
}

View *display_manager_get_current_view(void) { return dm.current_view; }

void display_manager_fill_screen(lv_color_t color) {
  static lv_style_t style;
  lv_style_init(&style);
  lv_style_set_bg_color(&style, color);
  lv_style_set_bg_opa(&style, LV_OPA_COVER);
  lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_style(lv_scr_act(), &style, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void set_backlight_brightness(uint8_t percentage) {

  if (percentage > 1) {
    percentage = 1;
  }

  gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, percentage);
}

void hardware_input_task(void *pvParameters) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);

  lv_indev_drv_t touch_driver;
  lv_indev_data_t touch_data;
  uint16_t calData[5] = {339, 3470, 237, 3438, 2};
  bool touch_active = false;
  int screen_width = LV_HOR_RES;
  int screen_height = LV_VER_RES;
  TickType_t last_touch_time = xTaskGetTickCount();
  bool is_backlight_dimmed = false;

  while (1) {
#ifdef CONFIG_USE_CARDPUTER
    keyboard_update_key_list(&gkeyboard);
    keyboard_update_keys_state(&gkeyboard);
    if (gkeyboard.key_list_buffer_len > 0) {
      for (size_t i = 0; i < gkeyboard.key_list_buffer_len; ++i) {
        Point2D_t key_pos = gkeyboard.key_list_buffer[i];
        uint8_t key_value = keyboard_get_key(&gkeyboard, key_pos);

        if (key_value != 0 && !touch_active) {
          touch_active = true;
          last_touch_time = xTaskGetTickCount();
          InputEvent event;
          event.type = INPUT_TYPE_JOYSTICK;

          printf("Unhandled key value: %d\n", key_value);

          switch (key_value) {
          case 180:
            event.data.joystick_index = 1;
            break;
          case 39:
            event.data.joystick_index = 0;
            break;
          case 158:
            event.data.joystick_index = 2;
            break;
          case 30:
            event.data.joystick_index = 3;
            break;
          case 56:
            event.data.joystick_index = 4;
            break;
          default:
            printf("Unhandled key value: %d\n", key_value);
            continue;
          }

          if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
            printf("Failed to send button input to queue\n");
          }

          vTaskDelay(pdMS_TO_TICKS(300));
        } else if (touch_active) {
          touch_active = false;
        }
      }
    }
#endif

#ifdef CONFIG_USE_JOYSTICK
    for (int i = 0; i < 5; i++) {
      if (joysticks[i].pin >= 0) {
        if (joystick_just_pressed(&joysticks[i])) {
          last_touch_time = xTaskGetTickCount();
          InputEvent event;
          event.type = INPUT_TYPE_JOYSTICK;
          event.data.joystick_index = i;

          if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
            printf("Failed to send joystick input to queue\n");
          }
        }
      }
    }
#endif

#ifdef CONFIG_USE_TOUCHSCREEN

#ifdef CONFIG_JC3248W535EN_LCD
    touch_driver_read_axs15231b(&touch_driver, &touch_data);
#else
    touch_driver_read(&touch_driver, &touch_data);
#endif

    if (touch_data.state == LV_INDEV_STATE_PR && !touch_active) {
      bool skip_event = false;
      last_touch_time = xTaskGetTickCount();
#ifdef CONFIG_HAS_BATTERY
      if (is_backlight_dimmed) {
        set_backlight_brightness(1);
        is_backlight_dimmed = false;
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(100));
      }
#endif
      if (!skip_event) {
        touch_active = true;
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.data.touch_data.point.x = touch_data.point.x;
        event.data.touch_data.point.y = touch_data.point.y;
        event.data.touch_data.state = touch_data.state;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          printf("Failed to send touch input to queue\n");
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_REL && touch_active) {
      last_touch_time = xTaskGetTickCount();
      InputEvent event;
      event.type = INPUT_TYPE_TOUCH;
      event.data.touch_data = touch_data;
      if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
        printf("Failed to send touch input to queue\n");
      }
      touch_active = false;
    }

#endif

    uint32_t current_timeout = G_Settings.display_timeout_ms > 0 ? G_Settings.display_timeout_ms : DEFAULT_DISPLAY_TIMEOUT_MS;
    if (!is_backlight_dimmed && (xTaskGetTickCount() - last_touch_time > pdMS_TO_TICKS(current_timeout))) {
      ESP_LOGD(TAG, "Display timeout check: last_touch=%lu, timeout=%lu",
               (unsigned long)last_touch_time, (unsigned long)current_timeout);
      ESP_LOGI(TAG, "Display timeout reached, dimming backlight");
      set_backlight_brightness(0);
      is_backlight_dimmed = true;
    }

    vTaskDelay(tick_interval);
  }

  vTaskDelete(NULL);
}

void processEvent() {
  // do not process events until the display manager is up
  if (!display_manager_init_success) {
    return;
  }

  InputEvent event;

  if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      View *current = dm.current_view;
      void (*input_callback)(InputEvent *) = NULL;
      const char *view_name = "NULL";

      if (current) {
        view_name = current->name;
        input_callback = current->input_callback;
      } else {
        printf("[WARNING] Current view is NULL in input_processing_task\n");
      }

      xSemaphoreGive(dm.mutex);

      printf("[INFO] Input event type: %d, Current view: %s\n", event.type,
             view_name);

      if (input_callback) {
        input_callback(&event);
      }
    }
  }
}

void lvgl_tick_task(void *arg) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);
  while (1) {
      processEvent();
      lv_timer_handler();
      lv_tick_inc(10);
      vTaskDelay(tick_interval);
  }
  vTaskDelete(NULL);
}
