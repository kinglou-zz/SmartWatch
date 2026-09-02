#ifndef SDCARD_H_
#define SDCARD_H_

#include <driver/gpio.h>
#include <string>
#include <vector>
#include <sdmmc_cmd.h>

class SdCard {
public:
    /// SDMMC mode (e.g. ESP32-P4 TF via Slot0)
    SdCard(gpio_num_t clk, gpio_num_t cmd, gpio_num_t d0,
           gpio_num_t d1, gpio_num_t d2, gpio_num_t d3);

    /// SDSPI mode (e.g. Waveshare ESP32-S3-Touch-AMOLED-2.06 TF)
    static SdCard* CreateSpi(gpio_num_t mosi, gpio_num_t miso,
                             gpio_num_t sck, gpio_num_t cs);

    ~SdCard();

    bool Mount();
    void Unmount();
    bool IsMounted() const { return mounted_; }

    // Scan root directory for .pcm files, returns sorted list
    std::vector<std::string> ScanPcmFiles();

    // Scan root directory for .pcm + .wav audio files
    std::vector<std::string> ScanAudioFiles();

private:
    enum class Mode { kSdmmc, kSdspi };

    SdCard(Mode mode, gpio_num_t a, gpio_num_t b, gpio_num_t c, gpio_num_t d,
           gpio_num_t e = GPIO_NUM_NC, gpio_num_t f = GPIO_NUM_NC);

    bool MountSdmmc();
    bool MountSdspi();

    Mode mode_ = Mode::kSdmmc;
    gpio_num_t clk_ = GPIO_NUM_NC;   // SDMMC CLK / SPI SCK
    gpio_num_t cmd_ = GPIO_NUM_NC;   // SDMMC CMD / SPI MOSI
    gpio_num_t d0_ = GPIO_NUM_NC;    // SDMMC D0  / SPI MISO
    gpio_num_t d1_ = GPIO_NUM_NC;    // SDMMC D1  / SPI CS
    gpio_num_t d2_ = GPIO_NUM_NC;
    gpio_num_t d3_ = GPIO_NUM_NC;
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
    bool spi_bus_owned_ = false;
};

#endif // SDCARD_H_
