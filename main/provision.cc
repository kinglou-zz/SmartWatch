#include "provision.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "Provision"
#define NVS_NS  "websocket"

void Provision::SaveWsUrl(const std::string& url) {
    Settings settings(NVS_NS, true);
    settings.SetString("url", url);
    ESP_LOGI(TAG, "WS URL saved: %s", url.c_str());
}

void Provision::SaveToken(const std::string& token) {
    Settings settings(NVS_NS, true);
    settings.SetString("token", token);
    ESP_LOGI(TAG, "Token saved (%zu chars)", token.size());
}

void Provision::SaveDeviceId(const std::string& device_id) {
    Settings settings(NVS_NS, true);
    settings.SetString("device_id", device_id);
    ESP_LOGI(TAG, "Device-Id saved: %s", device_id.c_str());
}

void Provision::SaveProtocolVersion(int version) {
    Settings settings(NVS_NS, true);
    settings.SetInt("version", version);
    ESP_LOGI(TAG, "Protocol version saved: %d", version);
}

void Provision::ApplyDefaultsIfNeeded() {
    Settings settings(NVS_NS, false);

    std::string url = settings.GetString("url");
    std::string fallback_url = settings.GetString("fallback_url");
    std::string token = settings.GetString("token");
    std::string device_id = settings.GetString("device_id");

    bool needs_write = false;

    if (url.empty()) {
        SaveWsUrl(STAGING_WS_URL);
        needs_write = true;
    } else {
        ESP_LOGI(TAG, "WS URL already set: %s", url.c_str());
    }

    if (fallback_url.empty()) {
        Settings ws(NVS_NS, true);
        ws.SetString("fallback_url", STAGING_WS_URL_FALLBACK);
        ESP_LOGI(TAG, "Fallback URL saved: %s", STAGING_WS_URL_FALLBACK);
        needs_write = true;
    } else {
        ESP_LOGI(TAG, "Fallback URL already set: %s", fallback_url.c_str());
    }

    if (token.empty()) {
        SaveToken(STAGING_DEVICE_JWT);
        needs_write = true;
    } else {
        ESP_LOGI(TAG, "Token already set (%zu chars)", token.size());
    }

    if (device_id.empty()) {
        SaveDeviceId(STAGING_DEVICE_UID);
        needs_write = true;
    } else {
        ESP_LOGI(TAG, "Device-Id already set: %s", device_id.c_str());
    }

    // Protocol version: only write if not set (0 is the default, meaning "not set")
    int version = settings.GetInt("version");
    if (version == 0) {
        SaveProtocolVersion(STAGING_PROTOCOL_VERSION);
        needs_write = true;
    }

    if (needs_write) {
        ESP_LOGI(TAG, "Provisioning defaults applied");
    } else {
        ESP_LOGI(TAG, "Already provisioned, nothing to do");
    }
}

void Provision::ApplyDefaults() {
    SaveWsUrl(STAGING_WS_URL);
    SaveToken(STAGING_DEVICE_JWT);
    SaveDeviceId(STAGING_DEVICE_UID);
    SaveProtocolVersion(STAGING_PROTOCOL_VERSION);
    ESP_LOGI(TAG, "Provisioning defaults force-applied");
}

bool Provision::IsStagingProvisioned() {
    Settings settings(NVS_NS, false);
    std::string url = settings.GetString("url");
    // Consider staging active if URL matches our staging WS URL or fallback
    if (url == STAGING_WS_URL || url == STAGING_WS_URL_FALLBACK) {
        return true;
    }
    // Also check token — if the staging JWT is in NVS, we're in staging mode
    std::string token = settings.GetString("token");
    if (token == STAGING_DEVICE_JWT) {
        return true;
    }
    return false;
}
