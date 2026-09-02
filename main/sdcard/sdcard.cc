#include "sdcard.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dirent.h>
#include <algorithm>
#include <cstring>

#if SOC_SDMMC_HOST_SUPPORTED
#include <driver/sdmmc_default_configs.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#define SDCARD_HAS_ON_CHIP_LDO 1
#endif
#endif

#define TAG "SdCard"

SdCard::SdCard(gpio_num_t clk, gpio_num_t cmd, gpio_num_t d0,
               gpio_num_t d1, gpio_num_t d2, gpio_num_t d3)
    : mode_(Mode::kSdmmc), clk_(clk), cmd_(cmd), d0_(d0), d1_(d1), d2_(d2), d3_(d3) {
}

SdCard* SdCard::CreateSpi(gpio_num_t mosi, gpio_num_t miso,
                          gpio_num_t sck, gpio_num_t cs) {
    // Fields: clk_=sck, cmd_=mosi, d0_=miso, d1_=cs
    return new SdCard(Mode::kSdspi, sck, mosi, miso, cs);
}

SdCard::SdCard(Mode mode, gpio_num_t a, gpio_num_t b, gpio_num_t c, gpio_num_t d,
               gpio_num_t e, gpio_num_t f)
    : mode_(mode), clk_(a), cmd_(b), d0_(c), d1_(d), d2_(e), d3_(f) {
}

SdCard::~SdCard() {
    Unmount();
}

bool SdCard::Mount() {
    if (mounted_) return true;
    if (mode_ == Mode::kSdspi) {
        return MountSdspi();
    }
    return MountSdmmc();
}

bool SdCard::MountSdspi() {
    // SPI3 keeps SPI2 free for QSPI LCD on ESP32-S3 boards
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = cmd_;
    bus_cfg.miso_io_num = d0_;
    bus_cfg.sclk_io_num = clk_;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }
    spi_bus_owned_ = true;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = d1_;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "SDSPI: MOSI=%d MISO=%d SCK=%d CS=%d",
             (int)cmd_, (int)d0_, (int)clk_, (int)d1_);

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        spi_bus_free((spi_host_device_t)host.slot);
        spi_bus_owned_ = false;
        card_ = nullptr;
        return false;
    }

    sdmmc_card_print_info(stdout, card_);
    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted at /sdcard (SPI)");
    return true;
}

bool SdCard::MountSdmmc() {
#if SOC_SDMMC_HOST_SUPPORTED
#ifdef SDCARD_HAS_ON_CHIP_LDO
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_handle = NULL;
    esp_err_t ldo_ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr_handle);
    if (ldo_ret != ESP_OK) {
        ESP_LOGW(TAG, "LDO VO4 enable failed: %s", esp_err_to_name(ldo_ret));
        pwr_handle = NULL;
    } else {
        ESP_LOGI(TAG, "LDO VO4 enabled");
    }
#endif

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_NUM_45,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_45, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
#ifdef SDCARD_HAS_ON_CHIP_LDO
    if (pwr_handle) {
        host.pwr_ctrl_handle = pwr_handle;
    }
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                            &mount_config, &card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD MMC mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        card_ = nullptr;
        return false;
    }

    sdmmc_card_print_info(stdout, card_);
    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted at /sdcard (SDMMC)");
    return true;
#else
    ESP_LOGE(TAG, "SDMMC not supported on this SoC");
    return false;
#endif
}

void SdCard::Unmount() {
    if (!mounted_) return;
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card_);
        card_ = nullptr;
    }
    if (spi_bus_owned_) {
        spi_bus_free(SPI3_HOST);
        spi_bus_owned_ = false;
    }
    mounted_ = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

std::vector<std::string> SdCard::ScanPcmFiles() {
    std::vector<std::string> files;
    if (!mounted_) return files;

    DIR* dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open /sdcard directory");
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) continue;
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len > 4 && (strcasecmp(name + len - 4, ".pcm") == 0)) {
            files.push_back(std::string(name));
        }
    }
    closedir(dir);

    std::sort(files.begin(), files.end());
    ESP_LOGI(TAG, "Found %d .pcm files", (int)files.size());
    return files;
}

std::vector<std::string> SdCard::ScanAudioFiles() {
    std::vector<std::string> files;
    if (!mounted_) return files;

    DIR* dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open /sdcard directory");
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) continue;
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len > 4) {
            const char* ext = name + len - 4;
            if (strcasecmp(ext, ".pcm") == 0 || strcasecmp(ext, ".wav") == 0) {
                files.push_back(std::string(name));
            }
        }
    }
    closedir(dir);

    std::sort(files.begin(), files.end());
    ESP_LOGI(TAG, "Found %d audio files (.pcm + .wav)", (int)files.size());
    return files;
}
