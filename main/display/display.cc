#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {
}


void Display::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::ClearChatMessages() {
    // Default empty implementation, override in subclasses if needed
}

void Display::ShowVmrReadConfirm(const char* read_label, const char* unread_label) {
    ESP_LOGI(TAG, "VmrReadConfirm: %s | %s",
             read_label ? read_label : "?", unread_label ? unread_label : "?");
    SetStatus(Lang::Strings::VMR_CONFIRM_PROMPT);
    char buf[96];
    snprintf(buf, sizeof(buf), "← %s    %s →",
             read_label ? read_label : "Read",
             unread_label ? unread_label : "Unread");
    SetChatMessage("system", buf);
}

void Display::HideVmrReadConfirm() {
    SetChatMessage("system", "");
}

void Display::ShowVmrBanner(const char* text) {
    ESP_LOGI(TAG, "VmrBanner: %s", text ? text : "");
}

void Display::HideVmrBanner() {
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}

void Display::SetEncoderInfo(int volume, int year) {
    // Default empty implementation
}
