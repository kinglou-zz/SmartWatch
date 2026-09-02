#ifndef PCM_PLAYER_H_
#define PCM_PLAYER_H_

#include <string>
#include <vector>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioCodec;

class PcmPlayer {
public:
    PcmPlayer();
    ~PcmPlayer();

    void SetFileList(const std::vector<std::string>& files);
    void SetCodec(AudioCodec* codec) { codec_ = codec; }

    // Navigation
    int GetTrackCount() const { return (int)files_.size(); }
    int GetCurrentIndex() const { return current_index_; }
    std::string GetCurrentFile() const;
    void NextTrack();
    void PrevTrack();
    // Select track whose basename starts with year (e.g. 1980 → "1980-7-15.wav")
    bool SelectByYear(int year);
    bool CurrentMatchesYear(int year) const;

    // Playback control
    void Play();
    void Stop();
    bool IsPlaying() const { return playing_; }

    // Callback when track changes (for UI update)
    std::function<void(const std::string& file, bool playing)> on_status_change;

private:
    void PlayTask();
    static void PlayTaskEntry(void* arg);

    std::vector<std::string> files_;
    int current_index_ = -1;
    AudioCodec* codec_ = nullptr;
    bool playing_ = false;
    bool stop_requested_ = false;
    TaskHandle_t play_task_ = nullptr;
};

#endif // PCM_PLAYER_H_
