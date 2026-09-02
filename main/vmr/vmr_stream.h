#ifndef VMR_STREAM_H_
#define VMR_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * Sliding byte pipe for VMR 边下边播 / 边读边播.
 * Producer Push() blocks when full; consumer Wait() blocks until data or EOF.
 */
class VmrBytePipe {
public:
    explicit VmrBytePipe(size_t capacity);
    ~VmrBytePipe();

    VmrBytePipe(const VmrBytePipe&) = delete;
    VmrBytePipe& operator=(const VmrBytePipe&) = delete;

    size_t Capacity() const { return capacity_; }
    size_t Size() const;
    size_t TotalPushed() const { return total_pushed_; }

    /// Push bytes; blocks while full unless aborted. Returns false on abort/error.
    bool Push(const uint8_t* data, size_t len);

    /// Wait until Size() >= min_bytes, or EOF/error/abort/timeout.
    /// @return true if Size() >= min_bytes (or any data when min_bytes==0 and Size()>0)
    bool Wait(size_t min_bytes, int timeout_ms);

    const uint8_t* Data() const { return buf_.data(); }
    /// Copy up to n bytes from the front (locked). Returns bytes copied.
    size_t CopyOut(uint8_t* dst, size_t n) const;
    void Consume(size_t n);

    void MarkEof();
    void MarkError();
    void Abort();

    bool IsEof() const { return eof_.load(); }
    bool HasError() const { return error_.load(); }
    bool IsAborted() const { return abort_.load(); }
    bool IsClosed() const { return eof_.load() || error_.load() || abort_.load(); }

private:
    void NotifyAll();

    std::vector<uint8_t> buf_;
    size_t capacity_ = 0;
    size_t total_pushed_ = 0;
    mutable SemaphoreHandle_t mu_ = nullptr;
    SemaphoreHandle_t cv_ = nullptr;  // binary; given on push/consume/close
    std::atomic<bool> eof_{false};
    std::atomic<bool> error_{false};
    std::atomic<bool> abort_{false};
};

struct VmrWavFormat {
    int16_t audio_format = 0;
    int16_t num_channels = 0;
    int32_t sample_rate = 0;
    int32_t byte_rate = 0;
    int16_t block_align = 0;
    int16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    size_t data_offset = 0;
};

/// Probe whether a local WAV file is complete (header + file size), without loading PCM.
bool VmrProbeWavFileComplete(const char* path);

/// Parse WAV fmt/data from a prefix buffer. Returns true when header is fully known.
bool VmrParseWavHeader(const uint8_t* data, size_t len, VmrWavFormat& out);

/**
 * Stream-decode WAV PCM from a byte pipe and play via output callback.
 * Reports on_started once prebuffer is met and first audio is about to play.
 *
 * @param prebuffer_ms  open-play threshold
 * @param rebuffer_ms   underrun recovery threshold
 * @param underrun_giveup_ms  fail if starved this long
 * @param should_stop   polled each chunk
 * @param output_pcm    receives mono int16 at codec_rate
 */
bool VmrPlayWavFromPipe(VmrBytePipe& pipe,
                        int codec_rate,
                        int prebuffer_ms,
                        int rebuffer_ms,
                        int underrun_giveup_ms,
                        const std::function<bool()>& should_stop,
                        const std::function<void()>& on_started,
                        const std::function<void(std::vector<int16_t>&)>& output_pcm);

#endif  // VMR_STREAM_H_
