#include "managers/gps_manager.h"
#include "core/callbacks.h"
#include "driver/periph_ctrl.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "managers/settings_manager.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_periph.h"
#include "sys/time.h"
#include "vendor/GPS/MicroNMEA.h"
#include "vendor/GPS/gps_logger.h"
#include <managers/views/terminal_screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *GPS_TAG = "GPS";
static bool has_valid_cached_date = false;
static bool gps_connection_logged = false;
static TaskHandle_t gps_check_task_handle = NULL;
static bool gps_timeout_detected = false;
static void check_gps_connection_task(void *pvParameters);

nmea_parser_handle_t nmea_hdl;

gps_date_t cacheddate = {0};

static bool is_valid_date(const gps_date_t *date) {
    if (!date)
        return false;

    // Check year (0-99 represents 2000-2099)
    if (!gps_is_valid_year(date->year))
        return false;

    // Check month (1-12)
    if (date->month < 1 || date->month > 12)
        return false;

    // Check day (1-31 depending on month)
    uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Adjust February for leap years
    uint16_t absolute_year = gps_get_absolute_year(date->year);
    if ((absolute_year % 4 == 0 && absolute_year % 100 != 0) || (absolute_year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    if (date->day < 1 || date->day > days_in_month[date->month - 1])
        return false;

    return true;
}

void gps_manager_init(GPSManager *manager) {
    // If there's an existing check task, delete it
    if (gps_check_task_handle != NULL) {
        vTaskDelete(gps_check_task_handle);
        gps_check_task_handle = NULL;
    }

    // Reset connection logged state
    gps_connection_logged = false;

    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    uint8_t current_rx_pin = settings_get_gps_rx_pin(&G_Settings);

    if (current_rx_pin != 0) {
        printf("GPS RX: IO%d\n", current_rx_pin);
        TERMINAL_VIEW_ADD_TEXT("GPS RX: IO%d\n", current_rx_pin);

        // Only disable UART1 which we use for GPS
        periph_module_disable(PERIPH_UART1_MODULE);

        gpio_reset_pin(current_rx_pin);
        vTaskDelay(pdMS_TO_TICKS(10));

        periph_module_enable(PERIPH_UART1_MODULE);

        gpio_set_direction(current_rx_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(current_rx_pin, GPIO_FLOATING);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[current_rx_pin], UART_PIN_NO_CHANGE);

        config.uart.rx_pin = current_rx_pin;
        config.uart.uart_port = UART_NUM_1; // Explicitly set UART1 for GPS
    }

#ifdef CONFIG_IS_GHOST_BOARD
    config.uart.rx_pin = 2;
#endif

    nmea_hdl = nmea_parser_init(&config);
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);
    manager->isinitilized = true;
    xTaskCreate(check_gps_connection_task, "gps_check", 2048, NULL, 1, &gps_check_task_handle);
}

static void check_gps_connection_task(void *pvParameters) {
    const TickType_t timeout = pdMS_TO_TICKS(10000); // 10 second timeout
    TickType_t start_time = xTaskGetTickCount();

    while (xTaskGetTickCount() - start_time < timeout) {
        if (!nmea_hdl) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;

        if (!gps) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Check if we're receiving valid GPS data
        if (!gps_connection_logged &&
            (gps->tim.hour != 0 || gps->tim.minute != 0 || gps->tim.second != 0 ||
             gps->latitude != 0 || gps->longitude != 0)) {
            printf("GPS Module Connected\nReceiving Data\n");
            TERMINAL_VIEW_ADD_TEXT("GPS Module Connected\nReceiving Data\n");
            gps_connection_logged = true;
            gps_check_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // If we reach here, connection check timed out
    printf("GPS Module Connection Timeout\nCheck your connections\n");
    TERMINAL_VIEW_ADD_TEXT("GPS Module Connection Timeout\nCheck your connections\n");
    gps_timeout_detected = true;
    gps_check_task_handle = NULL;
    vTaskDelete(NULL);
}

void gps_manager_deinit(GPSManager *manager) {
    if (manager->isinitilized) {
        // If there's an existing check task, delete it
        if (gps_check_task_handle != NULL) {
            vTaskDelete(gps_check_task_handle);
            gps_check_task_handle = NULL;
        }

        nmea_parser_remove_handler(nmea_hdl, gps_event_handler);
        nmea_parser_deinit(nmea_hdl);
        manager->isinitilized = false;
        gps_connection_logged = false;
    }
}

#define GPS_STATUS_MESSAGE "GPS: %s\nSats: %u/%u\nSpeed: %.1f km/h\nAccuracy: %s\n"
#define GPS_UPDATE_INTERVAL 4 // Show status every 4th update (25% chance)

#define MIN_SPEED_THRESHOLD 0.1   // Minimum 0.1 m/s (~0.36 km/h)
#define MAX_SPEED_THRESHOLD 340.0 // Maximum 340 m/s (~1224 km/h)

esp_err_t gps_manager_log_wardriving_data(wardriving_data_t *data) {
    if (!data || !nmea_hdl) {
        return ESP_ERR_INVALID_ARG;
    }
    gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
    if (!data->ble_data.is_ble_device) {
        if (!gps->valid || strlen(data->ssid) <= 2) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        // For BLE entries, only check GPS validity
        if (!gps->valid || gps->fix < GPS_FIX_GPS || gps->fix_mode < GPS_MODE_2D ||
            gps->sats_in_use < 3 || gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Validate GPS data
    if (!is_valid_date(&gps->date)) {
        if (!has_valid_cached_date) {
            ESP_LOGW(GPS_TAG, "No valid GPS date available");
            return ESP_ERR_INVALID_STATE;
        }

        // Only log warning for good GPS fixes
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D &&
            gps->sats_in_use >= 3 && gps->sats_in_use <= GPS_MAX_SATELLITES_IN_USE &&
            rand() % 100 == 0) {
            ESP_LOGW(GPS_TAG,
                     "Invalid date despite good fix: %04d-%02d-%02d "
                     "(Fix: %d, Mode: %d, Sats: %d)",
                     gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day,
                     gps->fix, gps->fix_mode, gps->sats_in_use);
        }

        // Use cached date for validation
        ESP_LOGD(GPS_TAG, "Using cached date: %04d-%02d-%02d",
                 gps_get_absolute_year(cacheddate.year), cacheddate.month, cacheddate.day);
    } else if (!has_valid_cached_date) {
        // Valid date - update cache
        cacheddate = gps->date;
        has_valid_cached_date = true;
        ESP_LOGI(GPS_TAG, "Cached valid GPS date: %04d-%02d-%02d",
                 gps_get_absolute_year(cacheddate.year), cacheddate.month, cacheddate.day);
    }

    data->latitude = gps->latitude;
    data->longitude = gps->longitude;
    data->altitude = gps->altitude;
    data->accuracy = gps->dop_h * 5.0;

    // Initialize GPS quality data to avoid uninitialized fields
    populate_gps_quality_data(data, gps);

    // First, validate the current GPS date
    if (!is_valid_date(&gps->date)) {
        // Only show warning if we have a truly valid fix
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D &&
            gps->sats_in_use >= 3 &&
            gps->sats_in_use <= GPS_MAX_SATELLITES_IN_USE && // Should be ≤ 12
            rand() % 100 == 0) {
            printf("Warning: GPS date is out of range despite good fix: %04d-%02d-%02d "
                   "(Fix: %d, Mode: %d, Sats: %d)\n",
                   gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day, gps->fix,
                   gps->fix_mode, gps->sats_in_use);
        }
        return ESP_OK;
    }

    // Then, only if we don't have a cached date and the current date is valid,
    // cache it
    if (cacheddate.year <= 0) {
        cacheddate = gps->date;
    }

    if (gps->tim.hour > 23 || gps->tim.minute > 59 || gps->tim.second > 59) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS time is invalid: %02d:%02d:%02d\n", gps->tim.hour, gps->tim.minute,
                   gps->tim.second);
        }
        return ESP_OK;
    }

    if (gps->latitude < -90.0 || gps->latitude > 90.0 || gps->longitude < -180.0 ||
        gps->longitude > 180.0) {
        if (rand() % 20 == 0) {
            printf("GPS Error: Invalid location detected (Lat: %f, Lon: %f)\n", gps->latitude,
                   gps->longitude);
        }
        return ESP_OK;
    }

    if (gps->speed < 0.0 || gps->speed > 340.0) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS speed is out of range: %f m/s\n", gps->speed);
        }
        return ESP_OK;
    }

    if (gps->dop_h < 0.0 || gps->dop_p < 0.0 || gps->dop_v < 0.0 || gps->dop_h > 50.0 ||
        gps->dop_p > 50.0 || gps->dop_v > 50.0) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS DOP values are out of range: HDOP: %f, PDOP: %f, "
                   "VDOP: %f\n",
                   gps->dop_h, gps->dop_p, gps->dop_v);
        }
        return ESP_OK;
    }

    esp_err_t ret = csv_write_data_to_buffer(data);
    if (ret != ESP_OK) {
        ESP_LOGE(GPS_TAG, "Failed to write wardriving data to CSV buffer");
        return ret;
    }

    // Update display periodically
    if (rand() % GPS_UPDATE_INTERVAL == 0) {
        // Determine GPS fix status
        const char *fix_status = (!gps->valid || gps->fix == GPS_FIX_INVALID) ? "No Fix"
                                 : (gps->fix_mode == GPS_MODE_2D)             ? "Basic"
                                 : (gps->fix_mode == GPS_MODE_3D)             ? "Locked"
                                                                              : "Unknown";

        // Validate satellite counts (clamp between 0 and max)
        uint8_t sats_in_use = (gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE) ? 0 : gps->sats_in_use;

        // Determine accuracy based on HDOP
        const char *accuracy = (gps->dop_h < 0.0 || gps->dop_h > 50.0) ? "Invalid"
                               : (gps->dop_h <= 1.0)                   ? "Perfect"
                               : (gps->dop_h <= 2.0)                   ? "High"
                               : (gps->dop_h <= 5.0)                   ? "Good"
                               : (gps->dop_h <= 10.0)                  ? "Okay"
                                                                       : "Poor";

        // Convert speed from m/s to km/h for display with validation
        float speed_kmh = 0.0;
        if (gps->valid && gps->fix >= GPS_FIX_GPS) { // Only trust speed with a valid fix
            if (gps->speed >= MIN_SPEED_THRESHOLD && gps->speed <= MAX_SPEED_THRESHOLD) {
                speed_kmh = gps->speed * 3.6; // Convert m/s to km/h
            } else if (gps->speed < MIN_SPEED_THRESHOLD && gps->speed >= 0.0) {
                speed_kmh = 0.0; // Show as stopped if below threshold but not negative
            }
            // Speeds above MAX_SPEED_THRESHOLD remain at 0.0
        }

        // Add newline before status update for better readability
        printf("\n");
        printf(
            GPS_STATUS_MESSAGE, fix_status, data->gps_quality.satellites_used,
            GPS_MAX_SATELLITES_IN_USE,
            data->gps_quality.speed * 3.6, // Convert m/s to km/h
            get_gps_quality_string(data)); // Only keep the arguments that match the format string
        TERMINAL_VIEW_ADD_TEXT(GPS_STATUS_MESSAGE, fix_status, data->gps_quality.satellites_used,
                               GPS_MAX_SATELLITES_IN_USE, data->gps_quality.speed * 3.6,
                               get_gps_quality_string(data));
    }

    return ret;
}

bool gps_is_timeout_detected(void) { return gps_timeout_detected; }