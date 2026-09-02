#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);
    virtual void SetEncoderInfo(int volume, int year) override;
    void SetPlaybackInfo(const char* info);
    void SetGramophoneInfo(int volume, int year, const char* date, const char* time);
    void SetGramophoneOverlay(const char* status, const char* message);

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    lv_obj_t *encoder_panel_ = nullptr;
    lv_obj_t *encoder_label_ = nullptr;
    lv_obj_t *playback_label_ = nullptr;

    // Gramophone UI elements
    lv_obj_t *gramo_page_ = nullptr;
    lv_obj_t *gramo_title_ = nullptr;
    lv_obj_t *gramo_year_label_ = nullptr;
    lv_obj_t *gramo_vol_label_ = nullptr;
    lv_obj_t *gramo_date_label_ = nullptr;
    lv_obj_t *gramo_time_label_ = nullptr;
    lv_obj_t *gramo_overlay_ = nullptr;
    lv_obj_t *gramo_overlay_emoji_ = nullptr;
    lv_obj_t *gramo_overlay_status_ = nullptr;
    lv_obj_t *gramo_overlay_msg_ = nullptr;
    bool gramo_chat_active_ = false;
    lv_obj_t *gramo_chat_page_ = nullptr;
    lv_obj_t *gramo_chat_emoji_ = nullptr;
    lv_obj_t *gramo_chat_status_ = nullptr;
    lv_obj_t *gramo_chat_msg_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


#endif
