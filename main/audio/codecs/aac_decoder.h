#ifndef AAC_DECODER_H_
#define AAC_DECODER_H_

#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief AAC audio file decoder (AAC-LC, ADTS container).
 *
 * Uses libhelix-aac for the actual decode. Supports seek by time offset.
 *
 * Usage:
 *   AacDecoder dec;
 *   dec.Open("/sdcard/program.aac");
 *   // ...
 *   int16_t buf[2048];
 *   int frames = dec.Decode(buf, 1024);  // 1024 samples per channel
 *   // feed buf to AudioCodec::OutputData()
 *   dec.Seek(5.0);  // jump to 5 seconds
 *   dec.Close();
 */
class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();

    /// Open an AAC file (ADTS format). Returns false on failure.
    bool Open(const std::string& filepath);

    /// Close the current file and clean up.
    void Close();

    /// Decode up to max_samples frames into output (16-bit PCM, mono if source is mono).
    /// Returns number of samples actually decoded, 0 = EOF, <0 = error.
    int Decode(int16_t* output, int max_samples);

    /// Seek to a time offset in seconds. Returns the actual offset reached.
    double Seek(double offset_seconds);

    /// Get the sample rate (Hz) of the decoded audio.
    int GetSampleRate() const { return sample_rate_; }

    /// Get the number of channels (1 = mono, 2 = stereo).
    int GetChannels() const { return channels_; }

    /// Get total duration in seconds (0 if unknown).
    double GetDuration() const { return duration_sec_; }

    /// Get current playback position in seconds.
    double GetPosition() const { return position_sec_; }

    /// True if file is currently open.
    bool IsOpen() const { return file_ != nullptr; }

private:
    int sample_rate_ = 0;
    int channels_ = 0;
    double duration_sec_ = 0.0;
    double position_sec_ = 0.0;
    void* file_ = nullptr;   // FILE* handle (opaque to avoid C header in .h)

    // Helix decoder state (opaque pointer — defined in the .cc file)
    void* haac_ = nullptr;

    // Internal: read and parse ADTS header, return frame size in bytes
    int ReadAdtsHeader();
    // Internal: estimate duration from file size and bitrate
    void EstimateDuration();
};

#endif // AAC_DECODER_H_
