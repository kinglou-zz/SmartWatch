#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "utils/sha256_util.h"

#define TAG "main"

// ============================================================
// Phase 0 Test 1: SHA256 string hash verification
// Remove this function after testing.
// ============================================================
static void test_sha256_string() {
    // Test 1: known string
    Sha256Hasher h1;
    h1.Start();
    h1.Update("hello world", 11);
    std::string r1 = h1.Finish();
    ESP_LOGI("TEST_SHA256", "hash('hello world') = %s", r1.c_str());
    ESP_LOGI("TEST_SHA256", "expected          = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    if (r1 == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9") {
        ESP_LOGI("TEST_SHA256", ">>> Test 1 PASSED <<<");
    } else {
        ESP_LOGE("TEST_SHA256", ">>> Test 1 FAILED <<<");
    }

    // Test 2: empty string
    Sha256Hasher h2;
    h2.Start();
    h2.Update("", 0);
    std::string r2 = h2.Finish();
    ESP_LOGI("TEST_SHA256", "hash('') = %s", r2.c_str());
    ESP_LOGI("TEST_SHA256", "expected  = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    if (r2 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        ESP_LOGI("TEST_SHA256", ">>> Test 2 PASSED <<<");
    } else {
        ESP_LOGE("TEST_SHA256", ">>> Test 2 FAILED <<<");
    }

    // Test 3: split Update should equal single Update
    Sha256Hasher h3;
    h3.Start();
    h3.Update("hello ", 6);
    h3.Update("world", 5);
    std::string r3 = h3.Finish();
    ESP_LOGI("TEST_SHA256", "hash('hello '+'world') = %s", r3.c_str());
    if (r3 == r1) {
        ESP_LOGI("TEST_SHA256", ">>> Test 3 PASSED <<<");
    } else {
        ESP_LOGE("TEST_SHA256", ">>> Test 3 FAILED <<<");
    }
}

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ====== Phase 0 Test 1: SHA256 ======
    test_sha256_string();
    // ===================================

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
