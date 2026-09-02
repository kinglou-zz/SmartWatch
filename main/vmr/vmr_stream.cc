#include "vmr_stream.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

#define TAG "VmrStream"

// ============================================================
// VmrBytePipe
// ============================================================

VmrBytePipe::VmrBytePipe(size_t capacity) : capacity_(capacity) {
    buf_.reserve(capacity_);
    mu_ = xSemaphoreCreateMutex();
    cv_ = xSemaphoreCreateBinary();
}

VmrBytePipe::~VmrBytePipe() {
    Abort();
    if (cv_) {
        vSemaphoreDelete(cv_);
        cv_ = nullptr;
    }
    if (mu_) {
        vSemaphoreDelete(mu_);
        mu_ = nullptr;
    }
}

void VmrBytePipe::NotifyAll() {
    if (cv_) {
        xSemaphoreGive(cv_);
    }
}

size_t VmrBytePipe::Size() const {
    if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return 0;
    size_t n = buf_.size();
    xSemaphoreGive(mu_);
    return n;
}

bool VmrBytePipe::Push(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return !IsClosed();
    size_t off = 0;
    while (off < len) {
        if (IsClosed()) return false;
        if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return false;

        while (buf_.size() >= capacity_ && !abort_.load() && !error_.load()) {
            xSemaphoreGive(mu_);
            // Wait for consumer to free space
            xSemaphoreTake(cv_, pdMS_TO_TICKS(50));
            if (IsClosed()) return false;
            if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return false;
        }
        if (abort_.load() || error_.load()) {
            xSemaphoreGive(mu_);
            return false;
        }

        size_t space = capacity_ - buf_.size();
        size_t n = std::min(space, len - off);
        buf_.insert(buf_.end(), data + off, data + off + n);
        total_pushed_ += n;
        off += n;
        xSemaphoreGive(mu_);
        NotifyAll();
    }
    return true;
}

bool VmrBytePipe::Wait(size_t min_bytes, int timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    TickType_t wait_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    while (true) {
        if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return false;
        size_t n = buf_.size();
        bool closed = eof_.load() || error_.load() || abort_.load();
        bool ok = (min_bytes == 0) ? (n > 0) : (n >= min_bytes);
        if (ok || (closed && n > 0 && min_bytes > 0 && n >= min_bytes)) {
            xSemaphoreGive(mu_);
            return true;
        }
        if (closed) {
            // EOF with partial data: succeed only if we already have min, else false
            bool have = (min_bytes == 0) ? (n > 0) : (n >= min_bytes);
            xSemaphoreGive(mu_);
            return have;
        }
        xSemaphoreGive(mu_);

        if (timeout_ms >= 0) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= wait_ticks) return false;
            TickType_t remain = wait_ticks - elapsed;
            xSemaphoreTake(cv_, remain);
        } else {
            xSemaphoreTake(cv_, portMAX_DELAY);
        }
    }
}

size_t VmrBytePipe::CopyOut(uint8_t* dst, size_t n) const {
    if (dst == nullptr || n == 0) return 0;
    if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return 0;
    size_t m = std::min(n, buf_.size());
    if (m > 0) {
        memcpy(dst, buf_.data(), m);
    }
    xSemaphoreGive(mu_);
    return m;
}

void VmrBytePipe::Consume(size_t n) {
    if (n == 0) return;
    if (xSemaphoreTake(mu_, portMAX_DELAY) != pdTRUE) return;
    if (n >= buf_.size()) {
        buf_.clear();
    } else {
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)n);
    }
    // Keep capacity reserved so Data() pointer stays stable across Push
    if (buf_.capacity() < capacity_) {
        buf_.reserve(capacity_);
    }
    xSemaphoreGive(mu_);
    NotifyAll();
}

void VmrBytePipe::MarkEof() {
    eof_.store(true);
    NotifyAll();
}

void VmrBytePipe::MarkError() {
    error_.store(true);
    NotifyAll();
}

void VmrBytePipe::Abort() {
    abort_.store(true);
    NotifyAll();
}

// ============================================================
// WAV probe / parse
// ============================================================

bool VmrParseWavHeader(const uint8_t* data, size_t len, VmrWavFormat& out) {
    if (data == nullptr || len < 12) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    VmrWavFormat hdr;
    size_t pos = 12;
    bool has_fmt = false;
    bool has_data = false;

    while (pos + 8 <= len) {
        uint32_t chunk_size = data[pos + 4] | (data[pos + 5] << 8) |
                              (data[pos + 6] << 16) | (data[pos + 7] << 24);
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            const uint8_t* f = data + pos + 8;
            size_t avail = (pos + 8 + chunk_size <= len) ? chunk_size : (len - pos - 8);
            if (avail < 16) return false;  // need more bytes
            hdr.audio_format = f[0] | (f[1] << 8);
            hdr.num_channels = f[2] | (f[3] << 8);
            hdr.sample_rate = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
            hdr.byte_rate = f[8] | (f[9] << 8) | (f[10] << 16) | (f[11] << 24);
            hdr.block_align = f[12] | (f[13] << 8);
            hdr.bits_per_sample = f[14] | (f[15] << 8);
            has_fmt = true;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            hdr.data_size = chunk_size;
            hdr.data_offset = pos + 8;
            has_data = true;
            break;
        } else {
            // Unknown chunk — need full chunk to skip
            if (pos + 8 + chunk_size > len) return false;
        }
        if (chunk_size > len) return false;
        pos += 8 + chunk_size + (chunk_size & 1);
        if (!has_data && pos > len) return false;
    }

    if (!has_fmt || !has_data) return false;
    if (hdr.num_channels <= 0 || hdr.bits_per_sample <= 0 || hdr.sample_rate <= 0) {
        return false;
    }
    out = hdr;
    return true;
}

bool VmrProbeWavFileComplete(const char* path) {
    if (path == nullptr) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 44) {
        return false;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) return false;

    // Read prefix large enough for typical headers + junk chunks
    const size_t kMaxProbe = 8192;
    size_t to_read = std::min((size_t)st.st_size, kMaxProbe);
    std::vector<uint8_t> prefix(to_read);
    size_t n = fread(prefix.data(), 1, to_read, fp);
    fclose(fp);
    if (n < 44) return false;

    VmrWavFormat hdr;
    if (!VmrParseWavHeader(prefix.data(), n, hdr)) {
        // Header may extend past probe window on exotic files
        return false;
    }
    uint64_t need = (uint64_t)hdr.data_offset + (uint64_t)hdr.data_size;
    if (need < 64) return false;
    if ((uint64_t)st.st_size < need) {
        ESP_LOGW(TAG, "WAV incomplete %s: size=%ld need=%llu",
                 path, (long)st.st_size, (unsigned long long)need);
        return false;
    }
    return true;
}

// ============================================================
// Stream WAV player
// ============================================================

static inline int16_t DecodeFrame(const uint8_t* f, int bps, int ch) {
    if (bps == 16 && ch == 1) {
        return (int16_t)(f[0] | (f[1] << 8));
    }
    if (bps == 16 && ch == 2) {
        int16_t l = (int16_t)(f[0] | (f[1] << 8));
        int16_t r = (int16_t)(f[2] | (f[3] << 8));
        return (int16_t)(((int32_t)l + r) / 2);
    }
    if (bps == 8) {
        return ((int16_t)f[0] - 128) << 8;
    }
    return 0;
}

bool VmrPlayWavFromPipe(VmrBytePipe& pipe,
                        int codec_rate,
                        int prebuffer_ms,
                        int rebuffer_ms,
                        int underrun_giveup_ms,
                        const std::function<bool()>& should_stop,
                        const std::function<void()>& on_started,
                        const std::function<void(std::vector<int16_t>&)>& output_pcm) {
    if (codec_rate <= 0) codec_rate = 24000;
    if (prebuffer_ms < 100) prebuffer_ms = 100;
    if (rebuffer_ms < 50) rebuffer_ms = 50;
    if (underrun_giveup_ms < 1000) underrun_giveup_ms = 1000;

    // ---- Wait for WAV header ----
    VmrWavFormat hdr;
    bool header_ok = false;
    int64_t header_wait_ms = 0;
    const int kHeaderStepMs = 100;
    std::vector<uint8_t> probe;
    probe.reserve(2048);
    while (!should_stop() && !pipe.HasError() && !pipe.IsAborted()) {
        if (!pipe.Wait(44, kHeaderStepMs)) {
            header_wait_ms += kHeaderStepMs;
            if (pipe.IsEof() && pipe.Size() < 44) {
                ESP_LOGE(TAG, "EOF before WAV header");
                return false;
            }
            if (header_wait_ms > 15000) {
                ESP_LOGE(TAG, "Timeout waiting WAV header");
                return false;
            }
            continue;
        }
        size_t n = std::min(pipe.Size(), (size_t)8192);
        if (n > probe.size()) {
            probe.resize(n);
        }
        size_t got = pipe.CopyOut(probe.data(), n);
        if (VmrParseWavHeader(probe.data(), got, hdr)) {
            header_ok = true;
            break;
        }
        if (got >= 8192) {
            ESP_LOGE(TAG, "WAV header not found in first %u bytes", (unsigned)got);
            return false;
        }
        if (pipe.IsEof()) {
            ESP_LOGE(TAG, "EOF with unparsable WAV header (%u bytes)", (unsigned)got);
            return false;
        }
        size_t want = n + 1;
        if (!pipe.Wait(want, kHeaderStepMs)) {
            header_wait_ms += kHeaderStepMs;
            if (header_wait_ms > 15000) {
                ESP_LOGE(TAG, "Timeout extending WAV header");
                return false;
            }
        }
    }
    if (!header_ok || should_stop()) return false;

    if (hdr.bits_per_sample != 8 && hdr.bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported bits_per_sample=%d", hdr.bits_per_sample);
        return false;
    }
    if (hdr.num_channels < 1 || hdr.num_channels > 2) {
        ESP_LOGE(TAG, "Unsupported channels=%d", hdr.num_channels);
        return false;
    }

    const size_t bps = (size_t)hdr.bits_per_sample / 8;
    const size_t frame_bytes = bps * (size_t)hdr.num_channels;
    if (frame_bytes == 0) return false;

    int byte_rate = hdr.byte_rate > 0
                        ? hdr.byte_rate
                        : (hdr.sample_rate * hdr.num_channels * hdr.bits_per_sample / 8);
    if (byte_rate <= 0) {
        byte_rate = hdr.sample_rate > 0 ? hdr.sample_rate * (int)frame_bytes : 32000;
    }
    size_t prebuffer_bytes = (size_t)((int64_t)byte_rate * prebuffer_ms / 1000);
    size_t rebuffer_bytes = (size_t)((int64_t)byte_rate * rebuffer_ms / 1000);
    if (prebuffer_bytes < frame_bytes * 64) prebuffer_bytes = frame_bytes * 64;
    if (rebuffer_bytes < frame_bytes * 32) rebuffer_bytes = frame_bytes * 32;

    // CRITICAL: prebuffer must fit in the ring, otherwise producer Push() blocks
    // forever while consumer Wait() never reaches the target (deadlock → "一直下载").
    const size_t ring_cap = pipe.Capacity();
    const size_t max_pre = ring_cap > 4096 ? (ring_cap * 3 / 4) : ring_cap / 2;
    if (prebuffer_bytes > max_pre) {
        ESP_LOGW(TAG, "Clamp prebuffer %u → %u (ring=%u, rate=%d)",
                 (unsigned)prebuffer_bytes, (unsigned)max_pre,
                 (unsigned)ring_cap, byte_rate);
        prebuffer_bytes = max_pre;
    }
    // Allow thicker rebuffer (up to 2/3 of usable ring) so one underrun recovers
    // with enough headroom instead of stuttering every few hundred ms.
    if (rebuffer_bytes > max_pre * 2 / 3) {
        rebuffer_bytes = max_pre * 2 / 3;
    }
    if (rebuffer_bytes < frame_bytes * 32) {
        rebuffer_bytes = frame_bytes * 32;
    }

    // Ensure header bytes present, then drop them so pipe holds PCM only
    if (!pipe.Wait(hdr.data_offset, 5000)) {
        ESP_LOGE(TAG, "Missing bytes through data_offset=%u", (unsigned)hdr.data_offset);
        return false;
    }
    pipe.Consume(hdr.data_offset);

    ESP_LOGI(TAG, "WAV stream: %dHz %dch %dbit data_size=%u prebuffer=%u bytes",
             (int)hdr.sample_rate, hdr.num_channels, hdr.bits_per_sample,
             (unsigned)hdr.data_size, (unsigned)prebuffer_bytes);

    // ---- Prefetch: adaptive catch-up ----
    // Start when buffered play-time covers remaining download (+margin), or when
    // download already ≥ realtime with min prebuffer. Avoids short-burst underruns
    // when cloud HTTPS << PCM byte_rate.
    const int64_t t0_us = esp_timer_get_time();
    int64_t last_rate_us = t0_us;
    size_t last_rate_have = pipe.Size();
    double dl_rate = 0.0;  // bytes/sec
    bool slow_oversized_abort = false;

    while (!should_stop() && !pipe.HasError() && !pipe.IsAborted()) {
        const size_t have = pipe.Size();
        const int64_t now_us = esp_timer_get_time();
        const int64_t pref_wait = (now_us - t0_us) / 1000;

        if (pipe.IsEof()) break;

        // Update download rate every ≥400ms of wall time (works even if Wait succeeds).
        if ((now_us - last_rate_us) >= 400000) {
            const int64_t dt_ms = (now_us - last_rate_us) / 1000;
            if (dt_ms > 0) {
                if (have >= last_rate_have) {
                    const double inst =
                        (double)(have - last_rate_have) * 1000.0 / (double)dt_ms;
                    dl_rate = (dl_rate <= 1.0) ? inst : (dl_rate * 0.65 + inst * 0.35);
                } else {
                    dl_rate *= 0.5;  // consumed? shouldn't happen before play
                }
            }
            last_rate_us = now_us;
            last_rate_have = have;
        }

        // Ring nearly full: must start so producer can finish.
        if (ring_cap > 0 && have + frame_bytes >= ring_cap) {
            ESP_LOGW(TAG, "Start play on full ring have=%u cap=%u",
                     (unsigned)have, (unsigned)ring_cap);
            break;
        }

        // Fast path: min prebuffer + download keeping up with realtime.
        if (have >= prebuffer_bytes && dl_rate >= (double)byte_rate * 0.90) {
            ESP_LOGI(TAG, "Prefetch realtime-ready have=%u dl=%.0fB/s need=%dB/s",
                     (unsigned)have, dl_rate, byte_rate);
            break;
        }

        // File bigger than ring + slow download → cannot stream smoothly.
        if (pref_wait >= 2000 && hdr.data_size > max_pre &&
            dl_rate > 1.0 && dl_rate < (double)byte_rate * 0.85) {
            ESP_LOGW(TAG,
                     "Stream unsuitable: data=%u ring_pre=%u dl=%.0fB/s < play=%dB/s",
                     (unsigned)hdr.data_size, (unsigned)max_pre, dl_rate, byte_rate);
            slow_oversized_abort = true;
            break;
        }

        // Catch-up: buffered audio duration >= ETA for remaining bytes + margin.
        if (hdr.data_size > 0 && dl_rate > 256.0 && have >= (size_t)(byte_rate / 2)) {
            const size_t remain =
                (have < hdr.data_size) ? (hdr.data_size - have) : 0;
            const double eta_ms =
                (remain > 0) ? ((double)remain * 1000.0 / dl_rate) : 0.0;
            const double buf_ms = (double)have * 1000.0 / (double)byte_rate;
            constexpr double kMarginMs = 800.0;
            if (remain == 0 || buf_ms >= eta_ms + kMarginMs) {
                ESP_LOGI(TAG,
                         "Prefetch catch-up start have=%u buf≈%.0fms remain=%u eta≈%.0fms dl=%.0fB/s",
                         (unsigned)have, buf_ms, (unsigned)remain, eta_ms, dl_rate);
                break;
            }
        }

        if (pref_wait >= 90000) {
            if (have >= (size_t)byte_rate) {
                ESP_LOGW(TAG, "Prefetch deadline start have=%u dl=%.0fB/s",
                         (unsigned)have, dl_rate);
                break;
            }
            ESP_LOGE(TAG, "Prefetch timeout have=%u need=%u dl=%.0fB/s",
                     (unsigned)have, (unsigned)prebuffer_bytes, dl_rate);
            return false;
        }

        const size_t want = (have < prebuffer_bytes) ? (have + 1) : (have + 1);
        pipe.Wait(want, 100);
    }
    if (slow_oversized_abort) {
        return false;  // caller falls back to download_then_play
    }
    if (should_stop() || pipe.HasError() || pipe.IsAborted()) return false;
    if (pipe.Size() < frame_bytes) {
        ESP_LOGE(TAG, "No PCM after prefetch");
        return false;
    }

    {
        const int have_ms = byte_rate > 0
                                ? (int)((pipe.Size() * 1000ull) / (size_t)byte_rate)
                                : 0;
        ESP_LOGI(TAG, "prefetch ok bytes=%u ms≈%d (target %dms, dl≈%.0fB/s)",
                 (unsigned)pipe.Size(), have_ms, prebuffer_ms, dl_rate);
    }
    if (on_started) {
        on_started();
    }

    const int CHUNK = 512;
    std::vector<int16_t> out;
    out.reserve(CHUNK);

    const bool need_resample = (hdr.sample_rate != codec_rate && hdr.sample_rate > 0);
    const double step = need_resample
                            ? ((double)hdr.sample_rate / (double)codec_rate)
                            : 1.0;
    double src_pos = 0.0;  // fractional index into src_mono

    std::vector<int16_t> src_mono;
    src_mono.reserve(2048);
    uint32_t pcm_bytes_seen = 0;
    uint32_t underrun_count = 0;
    int64_t starve_ms = 0;
    bool started_output = false;

    auto compact_src = [&]() {
        size_t drop = (size_t)src_pos;
        if (drop == 0) return;
        if (drop >= src_mono.size()) {
            src_pos -= (double)src_mono.size();
            src_mono.clear();
            return;
        }
        src_mono.erase(src_mono.begin(), src_mono.begin() + (std::ptrdiff_t)drop);
        src_pos -= (double)drop;
    };

    auto refill_src_mono = [&](size_t want_frames) -> bool {
        while (src_mono.size() < want_frames) {
            if (should_stop() || pipe.IsAborted() || pipe.HasError()) return false;

            size_t avail = pipe.Size();
            size_t frames_avail = avail / frame_bytes;

            if (hdr.data_size > 0) {
                uint32_t remain = (pcm_bytes_seen < hdr.data_size)
                                     ? (hdr.data_size - pcm_bytes_seen)
                                     : 0;
                size_t max_frames = remain / frame_bytes;
                if (max_frames == 0) {
                    return !src_mono.empty();
                }
                frames_avail = std::min(frames_avail, max_frames);
            }

            if (frames_avail == 0) {
                if (pipe.IsEof()) {
                    return !src_mono.empty();
                }
                if (started_output) {
                    underrun_count++;
                    starve_ms = 0;
                    // Wait for a solid refill — early resume with ~0.5s causes
                    // "short bursts" when download << realtime PCM.
                    ESP_LOGW(TAG, "underrun, rebuffering to %u bytes...",
                             (unsigned)rebuffer_bytes);
                    while (!should_stop() && !pipe.IsEof() && !pipe.HasError() &&
                           !pipe.IsAborted()) {
                        if (pipe.Size() >= rebuffer_bytes) break;
                        if (!pipe.Wait(rebuffer_bytes, 100)) {
                            starve_ms += 100;
                            if (starve_ms >= underrun_giveup_ms) {
                                if (pcm_bytes_seen >= 16000) {
                                    ESP_LOGW(TAG, "underrun giveup after play (%u pcm) — end",
                                             (unsigned)pcm_bytes_seen);
                                    return !src_mono.empty();
                                }
                                ESP_LOGE(TAG, "underrun giveup after %d ms (count=%u)",
                                         underrun_giveup_ms, (unsigned)underrun_count);
                                return false;
                            }
                        } else {
                            break;
                        }
                    }
                    continue;
                }
                if (!pipe.Wait(frame_bytes, 100)) {
                    if (pipe.IsEof()) return !src_mono.empty();
                    starve_ms += 100;
                    if (starve_ms >= underrun_giveup_ms) return false;
                }
                continue;
            }

            size_t take_frames =
                std::min(frames_avail, want_frames - src_mono.size());
            take_frames = std::min(take_frames, (size_t)256);
            size_t take_bytes = take_frames * frame_bytes;
            uint8_t raw_stack[1024];
            uint8_t* raw = raw_stack;
            std::vector<uint8_t> raw_heap;
            if (take_bytes > sizeof(raw_stack)) {
                raw_heap.resize(take_bytes);
                raw = raw_heap.data();
            }
            if (pipe.CopyOut(raw, take_bytes) < take_bytes) {
                continue;
            }
            for (size_t i = 0; i < take_frames; i++) {
                src_mono.push_back(DecodeFrame(raw + i * frame_bytes,
                                               hdr.bits_per_sample, hdr.num_channels));
            }
            pipe.Consume(take_bytes);
            pcm_bytes_seen += (uint32_t)take_bytes;
            starve_ms = 0;
        }
        return true;
    };

    while (!should_stop() && !pipe.HasError() && !pipe.IsAborted()) {
        if (hdr.data_size > 0 && pcm_bytes_seen >= hdr.data_size &&
            src_mono.empty() && pipe.Size() < frame_bytes) {
            break;
        }
        if (pipe.IsEof() && pipe.Size() < frame_bytes && src_mono.empty()) {
            break;
        }

        out.clear();
        if (!need_resample) {
            if (!refill_src_mono(1)) {
                if (pipe.IsEof() || should_stop() || started_output) break;
                return false;
            }
            size_t n = std::min((size_t)CHUNK, src_mono.size());
            out.assign(src_mono.begin(), src_mono.begin() + (std::ptrdiff_t)n);
            src_mono.erase(src_mono.begin(), src_mono.begin() + (std::ptrdiff_t)n);
        } else {
            size_t need_src = (size_t)src_pos + (size_t)(step * CHUNK) + 2;
            if (!refill_src_mono(need_src)) {
                if (!(pipe.IsEof() || should_stop() || started_output)) return false;
                if (src_mono.size() < (size_t)src_pos + 2) break;
            }
            out.resize(CHUNK);
            size_t produced = 0;
            for (; produced < (size_t)CHUNK; produced++) {
                size_t i0 = (size_t)src_pos;
                if (i0 + 1 >= src_mono.size()) break;
                double frac = src_pos - (double)i0;
                int16_t s0 = src_mono[i0];
                int16_t s1 = src_mono[i0 + 1];
                out[produced] = (int16_t)(s0 * (1.0 - frac) + s1 * frac);
                src_pos += step;
            }
            out.resize(produced);
            compact_src();
            if (produced == 0) break;
        }

        if (!out.empty()) {
            started_output = true;
            output_pcm(out);
        }
    }

    ESP_LOGI(TAG, "stream play done pcm_bytes=%u underrun=%u stopped=%d",
             (unsigned)pcm_bytes_seen, (unsigned)underrun_count, (int)should_stop());
    // Reject near-empty "success" (UI flash with no audible audio)
    const uint32_t kMinPcmBytes = 4000;  // ~0.1s @ 16kHz mono 16-bit
    if (!should_stop() && !pipe.HasError() && !pipe.IsAborted() &&
        pcm_bytes_seen < kMinPcmBytes) {
        ESP_LOGW(TAG, "stream play too short (%u bytes), treat as failure",
                 (unsigned)pcm_bytes_seen);
        return false;
    }
    return !should_stop() && !pipe.HasError() && !pipe.IsAborted() && started_output;
}
