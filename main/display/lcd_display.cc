#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (vmr_banner_ != nullptr) {
        lv_obj_del(vmr_banner_);
        vmr_banner_ = nullptr;
        vmr_banner_label_ = nullptr;
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Persistent VMR banner — sits just below the top status row */
    vmr_banner_ = lv_obj_create(screen);
    lv_obj_set_size(vmr_banner_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(vmr_banner_, 0, 0);
    lv_obj_set_style_bg_opa(vmr_banner_, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(vmr_banner_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(vmr_banner_, 0, 0);
    lv_obj_set_style_pad_top(vmr_banner_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_bottom(vmr_banner_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_left(vmr_banner_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(vmr_banner_, lvgl_theme->spacing(4), 0);
    lv_obj_set_scrollbar_mode(vmr_banner_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align_to(vmr_banner_, status_bar_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(vmr_banner_, LV_OBJ_FLAG_HIDDEN);

    vmr_banner_label_ = lv_label_create(vmr_banner_);
    lv_obj_set_width(vmr_banner_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(vmr_banner_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(vmr_banner_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(vmr_banner_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(vmr_banner_label_, "");
    lv_obj_center(vmr_banner_label_);

    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(12));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);

    /* Encoder info label - bottom center, just above bottom bar */
    /* Encoder info panel - bottom center, just above bottom bar */
    encoder_panel_ = lv_obj_create(screen);
    lv_obj_set_size(encoder_panel_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(encoder_panel_, lvgl_theme->spacing(3), 0);
    lv_obj_set_style_bg_opa(encoder_panel_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(encoder_panel_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(encoder_panel_, 0, 0);
    lv_obj_set_style_pad_top(encoder_panel_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_bottom(encoder_panel_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_left(encoder_panel_, lvgl_theme->spacing(6), 0);
    lv_obj_set_style_pad_right(encoder_panel_, lvgl_theme->spacing(6), 0);
    lv_obj_set_flex_flow(encoder_panel_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(encoder_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(encoder_panel_, lvgl_theme->spacing(2), 0);
    lv_obj_align(encoder_panel_, LV_ALIGN_BOTTOM_MID, 0, -text_font->line_height - lvgl_theme->spacing(14));

    /* Playback info label - above encoder panel, shows track info from SD card */
    playback_label_ = lv_label_create(screen);
    lv_label_set_text(playback_label_, "");
    lv_obj_set_style_text_font(playback_label_, icon_font, 0);
    lv_obj_set_style_text_color(playback_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(playback_label_, LV_ALIGN_BOTTOM_MID, 0, -text_font->line_height * 2 - lvgl_theme->spacing(20));
    lv_obj_add_flag(playback_label_, LV_OBJ_FLAG_HIDDEN);

    // Volume icon
    lv_obj_t* vol_icon = lv_label_create(encoder_panel_);
    lv_label_set_text(vol_icon, FONT_AWESOME_VOLUME_HIGH);
    lv_obj_set_style_text_font(vol_icon, icon_font, 0);
    lv_obj_set_style_text_color(vol_icon, lvgl_theme->text_color(), 0);

    // Encoder text (volume + year)
    encoder_label_ = lv_label_create(encoder_panel_);
    lv_label_set_text(encoder_label_, "");
    lv_obj_set_style_text_font(encoder_label_, text_font, 0);
    lv_obj_set_style_text_color(encoder_label_, lvgl_theme->text_color(), 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
    lv_label_set_text(chat_message_label_, content);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    // Stop any running GIF animation
    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr) {
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set loop delay to 1000ms
            gif_controller_->SetLoopDelay(3000);
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Retro color palette - warm wood tones
#define GRAMO_BG        lv_color_hex(0x1f0e05)  // dark espresso
#define GRAMO_GOLD      lv_color_hex(0xdaa520)  // warm gold
#define GRAMO_CARD_BG   lv_color_hex(0x2d140a)  // walnut
#define GRAMO_TEXT      lv_color_hex(0xfff8e7)  // warm cream
#define GRAMO_HINT      lv_color_hex(0xb8956a)  // light oak
#define GRAMO_GRAIN1    lv_color_hex(0x261208)
#define GRAMO_GRAIN2    lv_color_hex(0x2a1509)
#define GRAMO_GRAIN3    lv_color_hex(0x211007)

void LcdDisplay::SetupGramophoneUI() {
    DisplayLockGuard lock(this);
    if (gramo_page_ != nullptr) return;

    auto screen = lv_screen_active();
    int w = width_;
    int h = height_;

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    gramo_page_ = lv_obj_create(screen);
    lv_obj_set_size(gramo_page_, w, h);
    lv_obj_set_style_radius(gramo_page_, 0, 0);
    lv_obj_set_style_border_width(gramo_page_, 0, 0);
    lv_obj_set_style_pad_all(gramo_page_, 0, 0);
    lv_obj_set_style_bg_color(gramo_page_, GRAMO_BG, 0);
    lv_obj_add_flag(gramo_page_, LV_OBJ_FLAG_HIDDEN);

    // Wood grain texture - subtle horizontal stripes
    int grain_step = 20;
    for (int y = 0; y < h; y += grain_step) {
        lv_obj_t* grain = lv_obj_create(gramo_page_);
        lv_obj_set_size(grain, w, grain_step / 3);
        lv_obj_set_style_bg_color(grain, (y / grain_step) % 3 == 0 ? GRAMO_GRAIN1 :
                                         (y / grain_step) % 3 == 1 ? GRAMO_GRAIN2 : GRAMO_GRAIN3, 0);
        lv_obj_set_style_border_width(grain, 0, 0);
        lv_obj_set_style_radius(grain, 0, 0);
        lv_obj_set_style_pad_all(grain, 0, 0);
        lv_obj_align(grain, LV_ALIGN_TOP_LEFT, 0, y);
    }

    // === HEADER: Wood plank banner ===
    int header_h = 100;
    lv_obj_t* header = lv_obj_create(gramo_page_);
    lv_obj_set_size(header, w - 16, header_h);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x2d140a), 0);
    lv_obj_set_style_border_color(header, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_radius(header, 6, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 12);

    // Header grain lines
    for (int gy = 10; gy < header_h; gy += 18) {
        lv_obj_t* hg = lv_obj_create(header);
        lv_obj_set_size(hg, w - 20, 3);
        lv_obj_set_style_bg_color(hg, gy % 36 == 0 ? GRAMO_GRAIN1 : GRAMO_GRAIN2, 0);
        lv_obj_set_style_border_width(hg, 0, 0);
        lv_obj_set_style_radius(hg, 0, 0);
        lv_obj_align(hg, LV_ALIGN_TOP_MID, 0, gy);
    }

    gramo_title_ = lv_label_create(header);
    lv_label_set_text(gramo_title_, FONT_AWESOME_MUSIC "  AI-Gramophone  " FONT_AWESOME_MUSIC);
    lv_obj_set_style_text_color(gramo_title_, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(gramo_title_, text_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_title_, 512, 0);
    lv_obj_set_style_transform_scale_y(gramo_title_, 512, 0);
    lv_obj_align(gramo_title_, LV_ALIGN_CENTER, -120, 0);

    // Gold divider
    int divider_y = header_h + 20;
    lv_obj_t* div1 = lv_obj_create(gramo_page_);
    lv_obj_set_size(div1, w - 40, 3);
    lv_obj_set_style_bg_color(div1, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(div1, 0, 0);
    lv_obj_align(div1, LV_ALIGN_TOP_MID, 0, divider_y);

    // === MIDDLE: Large cards ===
    int card_top = divider_y + 10;
    int card_margin = 24;
    int card_w = (w - card_margin * 3) / 2;
    int card_h = (h - card_top - 200) / 2;
    int card_radius = 10;

    // --- YEAR card ---
    lv_obj_t* year_card = lv_obj_create(gramo_page_);
    lv_obj_set_size(year_card, card_w, card_h);
    lv_obj_set_style_bg_color(year_card, GRAMO_CARD_BG, 0);
    lv_obj_set_style_border_color(year_card, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(year_card, 3, 0);
    lv_obj_set_style_radius(year_card, card_radius, 0);
    lv_obj_set_style_pad_all(year_card, 14, 0);
    lv_obj_align(year_card, LV_ALIGN_TOP_LEFT, card_margin, card_top);

    // Year card grain
    for (int gy = 8; gy < card_h; gy += 14) {
        lv_obj_t* yg = lv_obj_create(year_card);
        lv_obj_set_size(yg, card_w - 8, 2);
        lv_obj_set_style_bg_color(yg, gy % 28 == 0 ? GRAMO_GRAIN1 : GRAMO_GRAIN2, 0);
        lv_obj_set_style_border_width(yg, 0, 0);
        lv_obj_set_style_radius(yg, 0, 0);
        lv_obj_align(yg, LV_ALIGN_TOP_MID, 0, gy);
    }

    // Year shadow (3D effect, scaled up)

    gramo_year_label_ = lv_label_create(year_card);
    lv_label_set_text(gramo_year_label_, "2025");
    lv_obj_set_style_text_color(gramo_year_label_, GRAMO_TEXT, 0);
    lv_obj_set_style_text_font(gramo_year_label_, text_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_year_label_, 768, 0);
    lv_obj_set_style_transform_scale_y(gramo_year_label_, 768, 0);
    lv_obj_align(gramo_year_label_, LV_ALIGN_CENTER, -30, 0);

    lv_obj_t* year_tag = lv_label_create(year_card);
    lv_label_set_text(year_tag, "YEAR");
    lv_obj_set_style_text_color(year_tag, GRAMO_HINT, 0);
    lv_obj_align(year_tag, LV_ALIGN_BOTTOM_MID, 0, -80);

    // --- VOLUME card ---
    lv_obj_t* vol_card = lv_obj_create(gramo_page_);
    lv_obj_set_size(vol_card, card_w, card_h);
    lv_obj_set_style_bg_color(vol_card, GRAMO_CARD_BG, 0);
    lv_obj_set_style_border_color(vol_card, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(vol_card, 3, 0);
    lv_obj_set_style_radius(vol_card, card_radius, 0);
    lv_obj_set_style_pad_all(vol_card, 14, 0);
    lv_obj_align(vol_card, LV_ALIGN_TOP_RIGHT, -card_margin, card_top);

    for (int gy = 8; gy < card_h; gy += 14) {
        lv_obj_t* vg = lv_obj_create(vol_card);
        lv_obj_set_size(vg, card_w - 8, 2);
        lv_obj_set_style_bg_color(vg, gy % 28 == 0 ? GRAMO_GRAIN1 : GRAMO_GRAIN2, 0);
        lv_obj_set_style_border_width(vg, 0, 0);
        lv_obj_set_style_radius(vg, 0, 0);
        lv_obj_align(vg, LV_ALIGN_TOP_MID, 0, gy);
    }


    gramo_vol_label_ = lv_label_create(vol_card);
    lv_label_set_text(gramo_vol_label_, "70");
    lv_obj_set_style_text_color(gramo_vol_label_, GRAMO_TEXT, 0);
    lv_obj_set_style_text_font(gramo_vol_label_, text_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_vol_label_, 768, 0);
    lv_obj_set_style_transform_scale_y(gramo_vol_label_, 768, 0);
    lv_obj_align(gramo_vol_label_, LV_ALIGN_CENTER, -30, 0);

    lv_obj_t* vol_tag = lv_label_create(vol_card);
    lv_label_set_text(vol_tag, "VOLUME");
    lv_obj_set_style_text_color(vol_tag, GRAMO_HINT, 0);
    lv_obj_align(vol_tag, LV_ALIGN_BOTTOM_MID, 0, -80);

    // === BOTTOM: Date + Time panel ===
    // Keep values near LV_SCALE_NONE(256): bot panel is short; 512(=2x) gets clipped.
    int bot_panel_h = 140;
    lv_obj_t* bot_panel = lv_obj_create(gramo_page_);
    lv_obj_set_size(bot_panel, w - 32, bot_panel_h);
    lv_obj_set_style_bg_color(bot_panel, GRAMO_CARD_BG, 0);
    lv_obj_set_style_border_color(bot_panel, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(bot_panel, 3, 0);
    lv_obj_set_style_radius(bot_panel, card_radius, 0);
    lv_obj_set_style_pad_all(bot_panel, 12, 0);
    lv_obj_clear_flag(bot_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bot_panel, LV_ALIGN_BOTTOM_MID, 0, -16);

    // Bottom panel grain
    for (int gy = 6; gy < bot_panel_h; gy += 12) {
        lv_obj_t* bg = lv_obj_create(bot_panel);
        lv_obj_set_size(bg, w - 48, 2);
        lv_obj_set_style_bg_color(bg, gy % 24 == 0 ? GRAMO_GRAIN1 : GRAMO_GRAIN2, 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_radius(bg, 0, 0);
        lv_obj_align(bg, LV_ALIGN_TOP_MID, 0, gy);
    }

    // Date section (left half): tag on top, value below
    lv_obj_t* date_tag = lv_label_create(bot_panel);
    lv_label_set_text(date_tag, "DATE");
    lv_obj_set_style_text_color(date_tag, GRAMO_HINT, 0);
    lv_obj_align(date_tag, LV_ALIGN_TOP_LEFT, 16, 10);

    gramo_date_label_ = lv_label_create(bot_panel);
    lv_label_set_text(gramo_date_label_, "--/--");
    lv_obj_set_style_text_color(gramo_date_label_, GRAMO_TEXT, 0);
    lv_obj_set_style_text_font(gramo_date_label_, text_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_date_label_, 320, 0);
    lv_obj_set_style_transform_scale_y(gramo_date_label_, 320, 0);
    lv_obj_align(gramo_date_label_, LV_ALIGN_TOP_LEFT, 16, 48);

    // Time section (right half)
    lv_obj_t* time_tag = lv_label_create(bot_panel);
    lv_label_set_text(time_tag, "TIME");
    lv_obj_set_style_text_color(time_tag, GRAMO_HINT, 0);
    lv_obj_align(time_tag, LV_ALIGN_TOP_RIGHT, -16, 10);

    gramo_time_label_ = lv_label_create(bot_panel);
    lv_label_set_text(gramo_time_label_, "--:--");
    lv_obj_set_style_text_color(gramo_time_label_, GRAMO_TEXT, 0);
    lv_obj_set_style_text_font(gramo_time_label_, text_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_time_label_, 320, 0);
    lv_obj_set_style_transform_scale_y(gramo_time_label_, 320, 0);
    lv_obj_align(gramo_time_label_, LV_ALIGN_TOP_RIGHT, -40, 48);

    // Corner ornaments
    int cs = 10;
    for (int r = 0; r < 4; r++) {
        int cx = (r % 2 == 0) ? 6 : w - 16;
        int cy = (r < 2) ? 6 : h - 16;
        lv_obj_t* sq = lv_obj_create(gramo_page_);
        lv_obj_set_size(sq, cs, cs);
        lv_obj_set_style_bg_color(sq, GRAMO_GOLD, 0);
        lv_obj_set_style_border_width(sq, 0, 0);
        lv_obj_set_style_radius(sq, 2, 0);
        lv_obj_set_style_pad_all(sq, 0, 0);
        lv_obj_align(sq, LV_ALIGN_TOP_LEFT, cx, cy);
    }

    // Ornamental dots along dividers
    int dn = 7;
    int ds = (w - 80) / (dn - 1);
    for (int k = 0; k < 2; k++) {
        int dy = (k == 0) ? header_h + 28 : h - bot_panel_h - 38;
        for (int i = 0; i < dn; i++) {
            lv_obj_t* d = lv_obj_create(gramo_page_);
            lv_obj_set_size(d, 5, 5);
            lv_obj_set_style_bg_color(d, GRAMO_GOLD, 0);
            lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_all(d, 0, 0);
            lv_obj_align(d, LV_ALIGN_TOP_MID, ds * i - (dn - 1) * ds / 2, dy);
        }
    }

    // Gold outer frame
    lv_obj_t* frame = lv_obj_create(gramo_page_);
    lv_obj_set_size(frame, w - 8, h - 8);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(frame, GRAMO_GOLD, 0);
    lv_obj_set_style_border_width(frame, 3, 0);
    lv_obj_set_style_radius(frame, 4, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_center(frame);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_CLICKABLE);

    // === Chat overlay (hidden by default, shown during AI interaction) ===
    gramo_overlay_ = lv_obj_create(gramo_page_);
    lv_obj_set_size(gramo_overlay_, w, h);
    lv_obj_set_style_bg_opa(gramo_overlay_, LV_OPA_0, 0);
    lv_obj_set_style_border_width(gramo_overlay_, 0, 0);
    lv_obj_set_style_pad_all(gramo_overlay_, 0, 0);
    lv_obj_add_flag(gramo_overlay_, LV_OBJ_FLAG_HIDDEN);

    gramo_overlay_emoji_ = lv_img_create(gramo_overlay_);
    lv_obj_align(gramo_overlay_emoji_, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(gramo_overlay_emoji_, LV_OBJ_FLAG_HIDDEN);

    gramo_overlay_status_ = lv_label_create(gramo_overlay_);
    lv_label_set_text(gramo_overlay_status_, "");
    lv_obj_set_style_text_color(gramo_overlay_status_, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(gramo_overlay_status_, icon_font, 0);
    lv_obj_align(gramo_overlay_status_, LV_ALIGN_CENTER, 0, 60);
    lv_label_set_long_mode(gramo_overlay_status_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(gramo_overlay_status_, w - 40);

    gramo_overlay_msg_ = lv_label_create(gramo_overlay_);
    lv_label_set_text(gramo_overlay_msg_, "");
    lv_obj_set_style_text_color(gramo_overlay_msg_, GRAMO_TEXT, 0);
    lv_obj_set_style_text_font(gramo_overlay_msg_, text_font, 0);
    lv_obj_align(gramo_overlay_msg_, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_long_mode(gramo_overlay_msg_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(gramo_overlay_msg_, w - 40);

    ESP_LOGI("LcdDisplay", "Gramophone UI created (senior-friendly wood style)");
}

void LcdDisplay::ShowGramophoneUI(bool show) {
    DisplayLockGuard lock(this);
    if (gramo_page_ == nullptr) {
        SetupGramophoneUI();
    }
    if (show) {
        lv_obj_remove_flag(gramo_page_, LV_OBJ_FLAG_HIDDEN);
        if (container_)    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_)      lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_)   lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_)   lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_)    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_)  lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (encoder_panel_) lv_obj_add_flag(encoder_panel_, LV_OBJ_FLAG_HIDDEN);
        if (playback_label_) lv_obj_add_flag(playback_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(gramo_page_, LV_OBJ_FLAG_HIDDEN);
        if (container_)    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_)      lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_)   lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_)   lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_)    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_)  lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (encoder_panel_) lv_obj_remove_flag(encoder_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ShowGramophoneOverlay(bool show) {
    DisplayLockGuard lock(this);
    gramo_chat_active_ = show;
    if (show) {
        if (emoji_box_)    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_)  lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_) lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_label_) lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_)   lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_)      lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_)   lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (encoder_panel_) lv_obj_add_flag(encoder_panel_, LV_OBJ_FLAG_HIDDEN);
        if (playback_label_) lv_obj_add_flag(playback_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (emoji_box_)    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_)  lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_label_) lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_)   lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_)      lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_)   lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (encoder_panel_) lv_obj_remove_flag(encoder_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetupChatUI() {
    DisplayLockGuard lock(this);
    if (gramo_chat_page_ != nullptr) return;
    auto screen = lv_screen_active();
    int w = width_, h = height_;
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    gramo_chat_page_ = lv_obj_create(screen);
    lv_obj_set_size(gramo_chat_page_, w, h);
    lv_obj_set_style_radius(gramo_chat_page_, 0, 0);
    lv_obj_set_style_border_width(gramo_chat_page_, 0, 0);
    lv_obj_set_style_pad_all(gramo_chat_page_, 0, 0);
    lv_obj_set_style_bg_color(gramo_chat_page_, GRAMO_BG, 0);
    lv_obj_add_flag(gramo_chat_page_, LV_OBJ_FLAG_HIDDEN);

    // Wood grain
    for (int y = 0; y < h; y += 20) {
        lv_obj_t* g = lv_obj_create(gramo_chat_page_);
        lv_obj_set_size(g, w, 7);
        lv_obj_set_style_bg_color(g, (y / 20) % 3 == 0 ? GRAMO_GRAIN1 : (y / 20) % 3 == 1 ? GRAMO_GRAIN2 : GRAMO_GRAIN3, 0);
        lv_obj_set_style_border_width(g, 0, 0);
        lv_obj_set_style_radius(g, 0, 0);
        lv_obj_set_style_pad_all(g, 0, 0);
        lv_obj_align(g, LV_ALIGN_TOP_LEFT, 0, y);
    }

    // Title
    lv_obj_t* tb = lv_obj_create(gramo_chat_page_);
    lv_obj_set_size(tb, w - 20, 70);
    lv_obj_set_style_bg_color(tb, GRAMO_CARD_BG, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0x5C3A1E), 0);
    lv_obj_set_style_border_width(tb, 2, 0);
    lv_obj_set_style_radius(tb, 8, 0);
    lv_obj_align(tb, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t* ct = lv_label_create(tb);
    lv_label_set_text(ct, "AI TIME MACHINE");
    lv_obj_set_style_text_color(ct, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(ct, text_font, 0);
    lv_obj_center(ct);

    // AI emoji
    gramo_chat_emoji_ = lv_label_create(gramo_chat_page_);
    lv_label_set_text(gramo_chat_emoji_, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_set_style_text_color(gramo_chat_emoji_, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(gramo_chat_emoji_, icon_font, 0);
    lv_obj_set_style_transform_scale_x(gramo_chat_emoji_, 512, 0);
    lv_obj_set_style_transform_scale_y(gramo_chat_emoji_, 512, 0);
    lv_obj_align(gramo_chat_emoji_, LV_ALIGN_CENTER, -15, 210);

    // Status
    gramo_chat_status_ = lv_label_create(gramo_chat_page_);
    lv_label_set_text(gramo_chat_status_, "");
    lv_obj_set_style_text_color(gramo_chat_status_, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(gramo_chat_status_, text_font, 0);
    lv_obj_align(gramo_chat_status_, LV_ALIGN_CENTER, 0, 60);

    // Chat message
    gramo_chat_msg_ = lv_label_create(gramo_chat_page_);
    lv_label_set_text(gramo_chat_msg_, "");
    lv_obj_set_style_text_color(gramo_chat_msg_, GRAMO_GOLD, 0);
    lv_obj_set_style_text_font(gramo_chat_msg_, text_font, 0);
    lv_label_set_long_mode(gramo_chat_msg_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(gramo_chat_msg_, w - 40);
    lv_obj_align(gramo_chat_msg_, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Gold frame
    lv_obj_t* fr = lv_obj_create(gramo_chat_page_);
    lv_obj_set_size(fr, w - 8, h - 8);
    lv_obj_set_style_bg_opa(fr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(fr, lv_color_hex(0x5C3A1E), 0);
    lv_obj_set_style_border_width(fr, 3, 0);
    lv_obj_set_style_radius(fr, 4, 0);
    lv_obj_set_style_pad_all(fr, 0, 0);
    lv_obj_center(fr);
    lv_obj_add_flag(fr, LV_OBJ_FLAG_CLICKABLE);

    // Corner ornaments
    for (int r = 0; r < 4; r++) {
        int cx = (r % 2 == 0) ? 6 : w - 16;
        int cy = (r < 2) ? 6 : h - 16;
        lv_obj_t* sq = lv_obj_create(gramo_chat_page_);
        lv_obj_set_size(sq, 10, 10);
        lv_obj_set_style_bg_color(sq, lv_color_hex(0x5C3A1E), 0);
        lv_obj_set_style_border_width(sq, 0, 0);
        lv_obj_set_style_radius(sq, 2, 0);
        lv_obj_set_style_pad_all(sq, 0, 0);
        lv_obj_align(sq, LV_ALIGN_TOP_LEFT, cx, cy);
    }
    ESP_LOGI("LcdDisplay", "Chat UI created");
}

void LcdDisplay::ShowChatUI(bool show) {
    DisplayLockGuard lock(this);
    if (gramo_chat_page_ == nullptr) SetupChatUI();
    gramo_chat_active_ = show;
    if (show) {
        lv_obj_remove_flag(gramo_chat_page_, LV_OBJ_FLAG_HIDDEN);
        if (gramo_page_) lv_obj_add_flag(gramo_page_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_) { lv_obj_move_foreground(emoji_label_); lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN); }
        if (emoji_box_) { lv_obj_move_foreground(emoji_box_); lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN); }
        if (status_label_) { lv_obj_move_foreground(status_label_); lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN); }
        if (chat_message_label_) { lv_obj_move_foreground(chat_message_label_); lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN); }
        if (bottom_bar_) { lv_obj_move_foreground(bottom_bar_); lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN); }
    } else {
        lv_obj_add_flag(gramo_chat_page_, LV_OBJ_FLAG_HIDDEN);
        if (gramo_page_) lv_obj_remove_flag(gramo_page_, LV_OBJ_FLAG_HIDDEN);
        // Hide overlay elements to prevent overlap with gramophone UI
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_label_) lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ShowVmrReadConfirm(const char* read_label, const char* unread_label) {
    {
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        if (screen == nullptr) {
            // Fall through; SetStatus below still updates status text.
        } else {
            // Leave top status + VMR banner visible above the confirm panel
            lv_coord_t top_h = 0;
            if (status_bar_ != nullptr) {
                top_h = lv_obj_get_height(status_bar_);
            }
            if (vmr_banner_ != nullptr && !lv_obj_has_flag(vmr_banner_, LV_OBJ_FLAG_HIDDEN)) {
                top_h += lv_obj_get_height(vmr_banner_);
            }
            if (top_h < 40) {
                top_h = 40;
            }

            if (vmr_confirm_ == nullptr) {
                vmr_confirm_ = lv_obj_create(screen);
                lv_obj_set_style_bg_color(vmr_confirm_, lv_color_hex(0x101820), 0);
                lv_obj_set_style_bg_opa(vmr_confirm_, LV_OPA_90, 0);
                lv_obj_set_style_border_width(vmr_confirm_, 0, 0);
                lv_obj_set_style_pad_all(vmr_confirm_, 12, 0);
                lv_obj_set_style_radius(vmr_confirm_, 0, 0);
                lv_obj_remove_flag(vmr_confirm_, LV_OBJ_FLAG_SCROLLABLE);

                auto prompt = lv_label_create(vmr_confirm_);
                lv_label_set_text(prompt, Lang::Strings::VMR_CONFIRM_PROMPT);
                lv_obj_set_style_text_color(prompt, lv_color_white(), 0);
                lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 8);
                lv_obj_set_width(prompt, LV_HOR_RES - 40);
                lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);

                vmr_confirm_read_btn_ = lv_button_create(vmr_confirm_);
                lv_obj_set_size(vmr_confirm_read_btn_, LV_HOR_RES / 2 - 20, LV_VER_RES / 4);
                lv_obj_align(vmr_confirm_read_btn_, LV_ALIGN_LEFT_MID, 10, 10);
                lv_obj_set_style_bg_color(vmr_confirm_read_btn_, lv_color_hex(0x2E7D32), 0);
                auto read_lbl = lv_label_create(vmr_confirm_read_btn_);
                lv_label_set_text(read_lbl, read_label ? read_label : "Read");
                lv_obj_set_style_text_color(read_lbl, lv_color_white(), 0);
                lv_obj_center(read_lbl);

                vmr_confirm_unread_btn_ = lv_button_create(vmr_confirm_);
                lv_obj_set_size(vmr_confirm_unread_btn_, LV_HOR_RES / 2 - 20, LV_VER_RES / 4);
                lv_obj_align(vmr_confirm_unread_btn_, LV_ALIGN_RIGHT_MID, -10, 10);
                lv_obj_set_style_bg_color(vmr_confirm_unread_btn_, lv_color_hex(0xC62828), 0);
                auto unread_lbl = lv_label_create(vmr_confirm_unread_btn_);
                lv_label_set_text(unread_lbl, unread_label ? unread_label : "Unread");
                lv_obj_set_style_text_color(unread_lbl, lv_color_white(), 0);
                lv_obj_center(unread_lbl);
            } else {
                if (vmr_confirm_read_btn_) {
                    auto lbl = lv_obj_get_child(vmr_confirm_read_btn_, 0);
                    if (lbl) lv_label_set_text(lbl, read_label ? read_label : "Read");
                }
                if (vmr_confirm_unread_btn_) {
                    auto lbl = lv_obj_get_child(vmr_confirm_unread_btn_, 0);
                    if (lbl) lv_label_set_text(lbl, unread_label ? unread_label : "Unread");
                }
            }

            // (Re)bind click handlers every show — Hide clears them to avoid UAF/races.
            if (vmr_confirm_read_btn_) {
                lv_obj_remove_event_cb(vmr_confirm_read_btn_, nullptr);
                lv_obj_remove_flag(vmr_confirm_read_btn_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(vmr_confirm_read_btn_, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(vmr_confirm_read_btn_, [](lv_event_t* e) {
                    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
                    if (self && self->on_vmr_read_confirm) {
                        self->on_vmr_read_confirm(true);
                    }
                }, LV_EVENT_CLICKED, this);
            }
            if (vmr_confirm_unread_btn_) {
                lv_obj_remove_event_cb(vmr_confirm_unread_btn_, nullptr);
                lv_obj_remove_flag(vmr_confirm_unread_btn_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(vmr_confirm_unread_btn_, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(vmr_confirm_unread_btn_, [](lv_event_t* e) {
                    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
                    if (self && self->on_vmr_read_confirm) {
                        self->on_vmr_read_confirm(false);
                    }
                }, LV_EVENT_CLICKED, this);
            }

            lv_obj_set_size(vmr_confirm_, LV_HOR_RES, LV_VER_RES - top_h);
            lv_obj_align(vmr_confirm_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_remove_flag(vmr_confirm_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(vmr_confirm_);
            if (top_bar_) lv_obj_move_foreground(top_bar_);
            if (status_bar_) lv_obj_move_foreground(status_bar_);
            if (vmr_banner_ && !lv_obj_has_flag(vmr_banner_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_move_foreground(vmr_banner_);
            }
        }
    }
    // SetStatus takes its own DisplayLock — never nest under DisplayLockGuard
    // (non-recursive lvgl_port_lock → main-task self-deadlock → WDT reboot).
    SetStatus(Lang::Strings::VMR_CONFIRM_PROMPT);
}

void LcdDisplay::HideVmrReadConfirm() {
    DisplayLockGuard lock(this);
    // Drop callback under LVGL lock so no concurrent click can invoke it.
    on_vmr_read_confirm = nullptr;
    if (vmr_confirm_ == nullptr) {
        return;
    }
    if (vmr_confirm_read_btn_ != nullptr) {
        lv_obj_remove_event_cb(vmr_confirm_read_btn_, nullptr);
        lv_obj_add_flag(vmr_confirm_read_btn_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(vmr_confirm_read_btn_, LV_OBJ_FLAG_CLICKABLE);
    }
    if (vmr_confirm_unread_btn_ != nullptr) {
        lv_obj_remove_event_cb(vmr_confirm_unread_btn_, nullptr);
        lv_obj_add_flag(vmr_confirm_unread_btn_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(vmr_confirm_unread_btn_, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_flag(vmr_confirm_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::ShowVmrBanner(const char* text) {
    DisplayLockGuard lock(this);
    if (vmr_banner_ == nullptr || vmr_banner_label_ == nullptr) {
        Display::ShowVmrBanner(text);
        return;
    }
    lv_label_set_text(vmr_banner_label_, text ? text : "");
    if (status_bar_ != nullptr) {
        lv_obj_align_to(vmr_banner_, status_bar_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    }
    lv_obj_remove_flag(vmr_banner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(vmr_banner_);
}

void LcdDisplay::HideVmrBanner() {
    DisplayLockGuard lock(this);
    if (vmr_banner_ != nullptr) {
        lv_obj_add_flag(vmr_banner_, LV_OBJ_FLAG_HIDDEN);
    }
}
