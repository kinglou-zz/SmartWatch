#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <vector>
#include <nvs_flash.h>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    bool SetString(const std::string& key, const std::string& value);
    bool GetBlob(const std::string& key, std::vector<uint8_t>& out);
    bool SetBlob(const std::string& key, const void* data, size_t length);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    bool SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    bool SetBool(const std::string& key, bool value);
    bool EraseKey(const std::string& key);
    bool EraseAll();
    bool Commit();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
