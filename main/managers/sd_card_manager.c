#include "managers/sd_card_manager.h"
#include "core/utils.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "vendor/drivers/CH422G.h"
#include "vendor/pcap.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "nvs_flash.h"
#include "nvs.h"

static const char *SD_TAG = "SD_Card_Manager";
static const char *NVS_NAMESPACE = "sd_config";

sd_card_manager_t sd_card_manager = { // Change this based on board config
    .card = NULL,
    .is_initialized = false,
    .clkpin = 19,
    .cmdpin = 18,
    .d0pin = 20,
    .d1pin = 21,
    .d2pin = 22,
    .d3pin = 23,
#ifdef CONFIG_USING_SPI
    .spi_cs_pin = CONFIG_SD_SPI_CS_PIN,
    .spi_clk_pin = CONFIG_SD_SPI_CLK_PIN,
    .spi_miso_pin = CONFIG_SD_SPI_MISO_PIN,
    .spi_mosi_pin = CONFIG_SD_SPI_MOSI_PIN
#endif
};

static void get_next_pcap_file_name(char *file_name_buffer,
                                    const char *base_name) {
  int next_index = get_next_pcap_file_index(base_name);
  snprintf(file_name_buffer, 128, "/mnt/ghostesp/pcaps/%s_%d.pcap", base_name,
           next_index);
}

void list_files_recursive(const char *dirname, int level) {
  DIR *dir = opendir(dirname);
  if (!dir) {
    printf("Failed to open directory: %s\n", dirname);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);

    if (written < 0 || written >= sizeof(path)) {
      printf("Path was truncated: %s/%s\n", dirname, entry->d_name);
      continue;
    }

    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
      for (int i = 0; i < level; i++) {
        printf("  ");
      }

      if (S_ISDIR(statbuf.st_mode)) {
        printf("[Dir] %s/\n", entry->d_name);
        list_files_recursive(path, level + 1);
      } else {
        printf("[File] %s\n", entry->d_name);
      }
    }
  }
  closedir(dir);
}

static void sdmmc_card_print_info(const sdmmc_card_t *card) {
  if (card == NULL) {
    printf("Card is NULL\n");
    return;
  }

  printf("Name: %s\n", card->cid.name);
  printf("Type: %s\n", (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
  printf("Capacity: %lluMB\n", ((uint64_t)card->csd.capacity) *
                                   card->csd.sector_size / (1024 * 1024));
  printf("Sector size: %dB\n", card->csd.sector_size);
  printf("Speed: %s\n",
         (card->csd.tr_speed > 25000000) ? "high speed" : "default speed");

  if (card->is_mem) {
    printf("Card is memory card\n");
    printf("CSD version: %d\n", card->csd.csd_ver);
    printf("Manufacture ID: %02x\n", card->cid.mfg_id);
    printf("Serial number: %08x\n", card->cid.serial);
  } else {
    printf("Card is not a memory card\n");
  }
}

esp_err_t sd_card_init(void) {
  esp_err_t ret = ESP_FAIL;

  // Load configuration from NVS first
  sd_card_load_config();
  sd_card_print_config(); // Print loaded/default config

  // Backup current config in case init fails
  sd_card_manager_t backup_config = sd_card_manager;

#ifdef CONFIG_USING_MMC_1_BIT
  printf("Initializing SD card in SDMMC mode (1-bit) using configured pins...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;

  slot_config.clk = CONFIG_SD_MMC_CLK;
  slot_config.cmd = CONFIG_SD_MMC_CMD;
  slot_config.d0 = CONFIG_SD_MMC_D0;

  gpio_set_pull_mode(CONFIG_SD_MMC_D0, GPIO_PULLUP_ONLY);  // CLK
  gpio_set_pull_mode(CONFIG_SD_MMC_CLK, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(CONFIG_SD_MMC_CMD, GPIO_PULLUP_ONLY); // D0

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();

#elif defined(CONFIG_USING_MMC)

  printf("Initializing SD card in SDMMC mode (4-bit) using configured pins...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  slot_config.clk = sd_card_manager.clkpin;
  slot_config.cmd = sd_card_manager.cmdpin; // SDMMC_CMD -> GPIO 16
  slot_config.d0 = sd_card_manager.d0pin;   // SDMMC_D0  -> GPIO 14
  slot_config.d1 = sd_card_manager.d1pin;   // SDMMC_D1  -> GPIO 17
  slot_config.d2 = sd_card_manager.d2pin;   // SDMMC_D2  -> GPIO 21
  slot_config.d3 = sd_card_manager.d3pin;   // SDMMC_D3  -> GPIO 18

  host.flags = SDMMC_HOST_FLAG_4BIT;

  gpio_set_pull_mode(sd_card_manager.clkpin, GPIO_PULLUP_ONLY); // CLK
  gpio_set_pull_mode(sd_card_manager.cmdpin, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(sd_card_manager.d0pin, GPIO_PULLUP_ONLY);  // D0
  gpio_set_pull_mode(sd_card_manager.d1pin, GPIO_PULLUP_ONLY);  // D1
  gpio_set_pull_mode(sd_card_manager.d2pin, GPIO_PULLUP_ONLY);  // D2
  gpio_set_pull_mode(sd_card_manager.d3pin, GPIO_PULLUP_ONLY);  // D3

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();
#elif CONFIG_USING_SPI

  printf("Initializing SD card in SPI mode using configured pins...\n");

#ifdef CONFIG_Waveshare_LCD
#define I2C_NUM I2C_NUM_0
#define I2C_ADDRESS 0x24
#define EXIO4_BIT (1 << 4)
#define EXIO1_BIT (1 << 1)

  esp_io_expander_ch422g_t *ch422g_dev = NULL;
  esp_err_t err;

  err = ch422g_new_device(I2C_NUM, I2C_ADDRESS, &ch422g_dev);
  if (err != ESP_OK) {
    printf("Failed to initialize CH422G: %s\n", esp_err_to_name(err));
    return err;
  }

  uint32_t direction, output_value;

  err = ch422g_read_direction_reg(ch422g_dev, &direction);
  if (err != ESP_OK) {
    printf("Failed to read direction register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial direction register: 0x%03lX\n", direction);

  err = ch422g_read_output_reg(ch422g_dev, &output_value);
  if (err != ESP_OK) {
    printf("Failed to read output register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial output register: 0x%03lX\n", output_value);

  direction &= ~EXIO1_BIT;
  output_value |= EXIO1_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  direction &= ~EXIO4_BIT;
  output_value &= ~EXIO4_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  printf("Final direction register: 0x%03lX\n", direction);
  printf("Final output register: 0x%03lX\n", output_value);

  cleanup_resources(ch422g_dev, I2C_NUM);
#endif

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();

  spi_bus_config_t bus_config;

  memset(&bus_config, 0, sizeof(spi_bus_config_t));

  bus_config.miso_io_num = sd_card_manager.spi_miso_pin;
  bus_config.mosi_io_num = sd_card_manager.spi_mosi_pin;
  bus_config.sclk_io_num = sd_card_manager.spi_clk_pin;

#ifdef CONFIG_IDF_TARGET_ESP32
  int dmabus = 2;
#elif CONFIG_IDF_TARGET_ESP32S3
  int dmabus = SPI_DMA_CH_AUTO;
#else
  int dmabus = SPI_DMA_CH_AUTO;
#endif

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
  ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    printf("Failed to initialize SPI2 bus: %s\n", esp_err_to_name(ret));
    return ret;
  }
#else
  ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    printf("Failed to initialize SPI2 bus: %s\n", esp_err_to_name(ret));
    return ret;
  }
#endif

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_card_manager.spi_cs_pin;
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
  slot_config.host_id = SPI2_HOST;
#else
  slot_config.host_id = SPI2_HOST;
#endif

  ret = esp_vfs_fat_sdspi_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    printf("Failed to mount filesystem: %s\n", esp_err_to_name(ret));
    spi_bus_free(host.slot);
    return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully in SPI mode.\n");

  sd_card_setup_directory_structure();

#endif

  // Common failure handling
  if (ret != ESP_OK) {
      // Restore backup config if init failed with loaded pins
      sd_card_manager = backup_config;
      printf("SD Card init failed with loaded pins. Check configuration.\n");
      // Optionally: attempt init with known defaults here as a fallback?
      return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();

  return ESP_OK;
}

void sd_card_unmount(void) {
#if SOC_SDMMC_HOST_SUPPORTED && SOC_SDMMC_USE_GPIO_MATRIX
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdmmc_unmount();
    printf("SD card unmounted\n");
    sd_card_manager.is_initialized = false;
  }
#else
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdcard_unmount("/mnt", sd_card_manager.card);
    spi_bus_free(SPI2_HOST);
    printf("SD card unmounted\n");
  }
#endif
}

esp_err_t sd_card_append_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("SD card is not initialized. Cannot append to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "ab");
  if (f == NULL) {
    printf("Failed to open file for appending\n");
    return ESP_FAIL;
  }
  fwrite(data, 1, size, f);
  fclose(f);
  printf("Data appended to file: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("SD card is not initialized. Cannot write to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    printf("Failed to open file for writing\n");
    return ESP_FAIL;
  }
  fwrite(data, 1, size, f);
  fclose(f);
  printf("File written: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("SD card is not initialized. Cannot read from file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    printf("Failed to open file for reading\n");
    return ESP_FAIL;
  }
  char line[64];
  while (fgets(line, sizeof(line), f) != NULL) {
    printf("%s", line);
  }
  fclose(f);
  printf("File read: %s\n", path);
  return ESP_OK;
}

static bool has_full_permissions(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if ((st.st_mode & 0777) == 0777) {
      return true;
    }
  }
  return false;
}

esp_err_t sd_card_create_directory(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("SD card is not initialized. Cannot create directory.\n");
    return ESP_FAIL;
  }

  if (sd_card_exists(path)) {
    printf("Directory already exists: %s\n", path);

    if (!has_full_permissions(path)) {
      printf("Directory %s does not have full permissions. Deleting and "
             "recreating.\n",
             path);

      if (rmdir(path) != 0) {
        printf("Failed to remove directory: %s\n", path);
        return ESP_FAIL;
      }

      int res = mkdir(path, 0777);
      if (res != 0) {
        printf("Failed to create directory: %s\n", path);
        return ESP_FAIL;
      }

      printf("Directory created: %s\n", path);

    } else {
      printf("Directory %s has correct permissions.\n", path);
      return ESP_OK;
    }
    return ESP_OK;
  }

  int res = mkdir(path, 0777);
  if (res != 0) {
    printf("Failed to create directory: %s\n", path);
    return ESP_FAIL;
  }

  printf("Directory created: %s\n", path);
  return ESP_OK;
}

bool sd_card_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return true;
  } else {
    return false;
  }
}

esp_err_t sd_card_setup_directory_structure() {
  const char *root_dir = "/mnt/ghostesp";
  const char *debug_dir = "/mnt/ghostesp/debug";
  const char *pcaps_dir = "/mnt/ghostesp/pcaps";
  const char *scans_dir = "/mnt/ghostesp/scans";
  const char *gps_dir = "/mnt/ghostesp/gps";
  const char *games_dir = "/mnt/ghostesp/games";
  const char *evil_portal_dir = "/mnt/ghostesp/evil_portal";

  if (!sd_card_exists(root_dir)) {
    printf("Creating directory: %s\n", root_dir);
    esp_err_t ret = sd_card_create_directory(root_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", root_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", root_dir);
  }

  if (!sd_card_exists(games_dir)) {
    printf("Creating directory: %s\n", games_dir);
    esp_err_t ret = sd_card_create_directory(games_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", games_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", games_dir);
  }

  if (!sd_card_exists(gps_dir)) {
    printf("Creating directory: %s\n", gps_dir);
    esp_err_t ret = sd_card_create_directory(gps_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", gps_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", gps_dir);
  }

  if (!sd_card_exists(debug_dir)) {
    printf("Creating directory: %s\n", debug_dir);
    esp_err_t ret = sd_card_create_directory(debug_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", debug_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", debug_dir);
  }

  if (!sd_card_exists(pcaps_dir)) {
    printf("Creating directory: %s\n", pcaps_dir);
    esp_err_t ret = sd_card_create_directory(pcaps_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", pcaps_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", pcaps_dir);
  }

  if (!sd_card_exists(scans_dir)) {
    printf("Creating directory: %s\n", scans_dir);
    esp_err_t ret = sd_card_create_directory(scans_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", scans_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", scans_dir);
  }

  // Create evil_portal directory
  if (!sd_card_exists(evil_portal_dir)) {
    printf("Creating directory: %s\n", evil_portal_dir);
    esp_err_t ret = sd_card_create_directory(evil_portal_dir);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", evil_portal_dir,
             esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", evil_portal_dir);
  }

  printf("Directory structure successfully set up.\n");
  return ESP_OK;
}

// New SD card pin configuration functions

esp_err_t sd_card_set_mmc_pins(int clk, int cmd, int d0, int d1, int d2, int d3) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.clkpin = clk;
  sd_card_manager.cmdpin = cmd;
  sd_card_manager.d0pin = d0;
  sd_card_manager.d1pin = d1;
  sd_card_manager.d2pin = d2;
  sd_card_manager.d3pin = d3;
  
  printf("SD card MMC pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_set_spi_pins(int cs, int clk, int miso, int mosi) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.spi_cs_pin = cs;
  sd_card_manager.spi_clk_pin = clk;
  sd_card_manager.spi_miso_pin = miso;
  sd_card_manager.spi_mosi_pin = mosi;
  
  printf("SD card SPI pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_save_config() {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    return err;
  }

  // Write MMC pins
  nvs_set_i32(nvs_handle, "mmc_clk", sd_card_manager.clkpin);
  nvs_set_i32(nvs_handle, "mmc_cmd", sd_card_manager.cmdpin);
  nvs_set_i32(nvs_handle, "mmc_d0", sd_card_manager.d0pin);
  nvs_set_i32(nvs_handle, "mmc_d1", sd_card_manager.d1pin);
  nvs_set_i32(nvs_handle, "mmc_d2", sd_card_manager.d2pin);
  nvs_set_i32(nvs_handle, "mmc_d3", sd_card_manager.d3pin);

  // Write SPI pins
  nvs_set_i32(nvs_handle, "spi_cs", sd_card_manager.spi_cs_pin);
  nvs_set_i32(nvs_handle, "spi_clk", sd_card_manager.spi_clk_pin);
  nvs_set_i32(nvs_handle, "spi_miso", sd_card_manager.spi_miso_pin);
  nvs_set_i32(nvs_handle, "spi_mosi", sd_card_manager.spi_mosi_pin);

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) committing NVS changes!\n", esp_err_to_name(err));
  }
  else {
      printf("SD card pin configuration saved to NVS.\n");
  }

  // Close NVS handle
  nvs_close(nvs_handle);

  return err; // Return the result of nvs_commit or nvs_open
}

esp_err_t sd_card_load_config() {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("NVS namespace '%s' not found. Using default SD pins.\n", NVS_NAMESPACE);
        // Keep default pins already set in sd_card_manager struct definition
        return ESP_OK; // Not an error if first boot
    } else {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return err;
    }
  }

  int32_t temp_val;

  // Read MMC pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "mmc_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.clkpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_cmd", &temp_val);
  if (err == ESP_OK) sd_card_manager.cmdpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d0", &temp_val);
  if (err == ESP_OK) sd_card_manager.d0pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d1", &temp_val);
  if (err == ESP_OK) sd_card_manager.d1pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d2", &temp_val);
  if (err == ESP_OK) sd_card_manager.d2pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d3", &temp_val);
  if (err == ESP_OK) sd_card_manager.d3pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Read SPI pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "spi_cs", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_cs_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_clk_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_miso", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_miso_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_mosi", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_mosi_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Success path
  printf("SD card pin configuration loaded from NVS.\n");
  nvs_close(nvs_handle);
  return ESP_OK;

read_error:
  printf("Error (%s) reading NVS key! Using default SD pins.\n", esp_err_to_name(err));
  nvs_close(nvs_handle);
  // Keep default pins already set in sd_card_manager struct definition
  return err; // Return the actual read error
}

void sd_card_print_config() {
  printf("SD Card Pin Configuration:\n");
  printf("MMC Mode:\n");
  printf("  CLK: GPIO%d\n", sd_card_manager.clkpin);
  printf("  CMD: GPIO%d\n", sd_card_manager.cmdpin);
  printf("  D0:  GPIO%d\n", sd_card_manager.d0pin);
  printf("  D1:  GPIO%d\n", sd_card_manager.d1pin);
  printf("  D2:  GPIO%d\n", sd_card_manager.d2pin);
  printf("  D3:  GPIO%d\n", sd_card_manager.d3pin);
  printf("SPI Mode:\n");
  printf("  CS:   GPIO%d\n", sd_card_manager.spi_cs_pin);
  printf("  CLK:  GPIO%d\n", sd_card_manager.spi_clk_pin);
  printf("  MISO: GPIO%d\n", sd_card_manager.spi_miso_pin);
  printf("  MOSI: GPIO%d\n", sd_card_manager.spi_mosi_pin);
}