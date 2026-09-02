#include "settings.h"

#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "Settings"

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            esp_err_t err = nvs_commit(nvs_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns_.c_str(), esp_err_to_name(err));
            }
        }
        nvs_close(nvs_handle_);
    }
}

bool Settings::Commit() {
    if (nvs_handle_ == 0 || !read_write_ || !dirty_) {
        return true;
    }
    esp_err_t err = nvs_commit(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns_.c_str(), esp_err_to_name(err));
        return false;
    }
    dirty_ = false;
    return true;
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    if (nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length) != ESP_OK) {
        return default_value;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

bool Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s/%s) failed: %s (len=%u)",
                 ns_.c_str(), key.c_str(), esp_err_to_name(err),
                 (unsigned)value.size());
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::GetBlob(const std::string& key, std::vector<uint8_t>& out) {
    out.clear();
    if (nvs_handle_ == 0) {
        return false;
    }
    size_t length = 0;
    if (nvs_get_blob(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK || length == 0) {
        return false;
    }
    out.resize(length);
    if (nvs_get_blob(nvs_handle_, key.c_str(), out.data(), &length) != ESP_OK) {
        out.clear();
        return false;
    }
    out.resize(length);
    return true;
}

bool Settings::SetBlob(const std::string& key, const void* data, size_t length) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    esp_err_t err = nvs_set_blob(nvs_handle_, key.c_str(), data, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s/%s) failed: %s (len=%u)",
                 ns_.c_str(), key.c_str(), esp_err_to_name(err),
                 (unsigned)length);
        return false;
    }
    dirty_ = true;
    return true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

bool Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs_handle_, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s/%s) failed: %s",
                 ns_.c_str(), key.c_str(), esp_err_to_name(err));
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

bool Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s/%s) failed: %s",
                 ns_.c_str(), key.c_str(), esp_err_to_name(err));
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::EraseKey(const std::string& key) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    auto ret = nvs_erase_key(nvs_handle_, key.c_str());
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key(%s/%s) failed: %s",
                 ns_.c_str(), key.c_str(), esp_err_to_name(ret));
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::EraseAll() {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return false;
    }
    esp_err_t err = nvs_erase_all(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_all(%s) failed: %s", ns_.c_str(), esp_err_to_name(err));
        return false;
    }
    dirty_ = true;
    return true;
}
