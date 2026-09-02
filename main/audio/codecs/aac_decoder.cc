#include "aac_decoder.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ============================================================
// libhelix-aac API — uses the standard RealNetworks open-source
// AAC decoder (HaacDecoder / HAACDecoder API)
//
// Source: https://github.com/nickfox/helix-aac
// Place the extracted source under:
//   main/audio/codecs/helix/
//
// Required files from helix-aac:
//   aacdec.c, aacdec.h, aactabs.c, aactabs.h, bitstream.c,
//   bitstream.h, bufdesc.c, bufdesc.h, channel.c, channel.h,
//   huff.c, huff.h, hufftab.h, ic_predict.c, ic_predict.h,
//   lt_predict.c, lt_predict.h, pnselement.c, pnselement.h,
//   pulse.c, pulse.h, sbr.c, sbr.h, sbrfwd.c, sbrfwd.h,
//   sbrtabs.c, sbrtabs.h, tns.c, tns.h
//
// API:
//   void* HaacDecoderOpen(void);
//   int  HaacDecoderInit(void* h, int aac_frame_len);
//   int  HaacDecoderDecode(void* h,
//          unsigned char* in_data, int in_len,
//          short* out_samples, int max_samples);
//   void HaacDecoderClose(void* h);
//   int  HaacDecoderGetSampleRate(void* h);
//   int  HaacDecoderGetNumChannels(void* h);
// ============================================================

// ---- Re-define these when the actual helix headers are present ----
extern "C" {
    // These extern declarations will be replaced by the actual
    // #include "helix/aacdec.h" when the source is in place.
    void* HaacDecoderOpen(void);
    int  HaacDecoderInit(void* h, int aac_frame_len);
    int  HaacDecoderDecode(void* h,
            unsigned char* in_data, int in_len,
            short* out_samples, int max_samples);
    void HaacDecoderClose(void* h);
    int  HaacDecoderGetSampleRate(void* h);
    int  HaacDecoderGetNumChannels(void* h);
}

#define TAG "AacDecoder"

// ADTS fixed header is 7 bytes
#define ADTS_HEADER_SIZE  7

// Maximum AAC frame size (worst case: ~6KB for 128kbps HE-AAC)
#define AAC_MAX_FRAME_SIZE  8192
#define AAC_INPUT_BUF_SIZE  16384
#define PCM_OUTPUT_SIZE     4096   // max samples per decode call

AacDecoder::AacDecoder() {
}

AacDecoder::~AacDecoder() {
    Close();
}

bool AacDecoder::Open(const std::string& filepath) {
    Close();

    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", filepath.c_str());
        return false;
    }
    file_ = f;

    // Create helix decoder instance
    haac_ = HaacDecoderOpen();
    if (!haac_) {
        ESP_LOGE(TAG, "HaacDecoderOpen failed");
        fclose(f);
        file_ = nullptr;
        return false;
    }

    // Read first ADTS frame to initialize
    int frame_size = ReadAdtsHeader();
    if (frame_size <= 0) {
        ESP_LOGE(TAG, "Invalid ADTS header");
        Close();
        return false;
    }

    // Initialize with frame size (common ADTS frame lengths like 1024)
    // The actual frame length depends on the profile (AAC-LC uses 1024)
    HaacDecoderInit(haac_, 1024);

    // Read metadata
    sample_rate_ = HaacDecoderGetSampleRate(haac_);
    channels_    = HaacDecoderGetNumChannels(haac_);
    if (sample_rate_ <= 0) sample_rate_ = 44100;
    if (channels_ <= 0)    channels_ = 2;

    // Estimate duration
    EstimateDuration();

    position_sec_ = 0.0;
    rewind(f);
    // Reset decoder for fresh start
    HaacDecoderClose(haac_);
    haac_ = HaacDecoderOpen();
    HaacDecoderInit(haac_, 1024);

    // Re-read the first ADTS header to sync the read pointer
    ReadAdtsHeader();

    ESP_LOGI(TAG, "Opened: %s  %dHz %dch %.1fs",
             filepath.c_str(), sample_rate_, channels_, duration_sec_);
    return true;
}

void AacDecoder::Close() {
    if (haac_) {
        HaacDecoderClose(haac_);
        haac_ = nullptr;
    }
    if (file_) {
        fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    sample_rate_ = 0;
    channels_ = 0;
    duration_sec_ = 0.0;
    position_sec_ = 0.0;
}

int AacDecoder::Decode(int16_t* output, int max_samples) {
    if (!haac_ || !file_) return 0;

    FILE* f = static_cast<FILE*>(file_);
    unsigned char in_buf[AAC_MAX_FRAME_SIZE];
    int total_samples = 0;

    while (total_samples < max_samples) {
        // Read ADTS frame
        int frame_size = ReadAdtsHeader();
        if (frame_size <= 0 || frame_size > AAC_MAX_FRAME_SIZE) {
            break;  // EOF or corrupt
        }

        size_t remain = frame_size - ADTS_HEADER_SIZE;
        size_t read = fread(in_buf, 1, remain, f);
        if (read < remain) {
            ESP_LOGW(TAG, "Short read: %d/%d bytes", (int)read, (int)remain);
            break;
        }

        // Decode this frame
        short* out = output + total_samples;
        int avail = max_samples - total_samples;
        int decoded = HaacDecoderDecode(haac_, in_buf, (int)read, out, avail);
        if (decoded < 0) {
            ESP_LOGW(TAG, "Decode error: %d", decoded);
            continue;  // skip bad frame
        }
        total_samples += decoded;

        // Update position estimate (1024 samples per frame for AAC-LC)
        double samples_per_frame = 1024.0;
        position_sec_ += samples_per_frame / sample_rate_;
    }

    return total_samples;
}

double AacDecoder::Seek(double offset_seconds) {
    if (!file_ || !haac_) return 0.0;

    // For AAC with known bitrate: approximate byte offset
    // file_byte_offset ≈ offset_seconds * bitrate_bps / 8
    // We estimate bitrate from file_size / duration
    long current_pos = ftell(static_cast<FILE*>(file_));

    if (duration_sec_ > 0 && offset_seconds >= 0) {
        // Estimate byte position
        fseek(static_cast<FILE*>(file_), 0, SEEK_END);
        long total_size = ftell(static_cast<FILE*>(file_));
        double bitrate = total_size * 8.0 / duration_sec_;  // bps
        long target_byte = (long)(offset_seconds * bitrate / 8.0);
        if (target_byte >= total_size) target_byte = total_size - 1024;
        if (target_byte < 0) target_byte = 0;

        // Seek and re-sync to next ADTS header
        fseek(static_cast<FILE*>(file_), target_byte, SEEK_SET);

        // Forward-scan to find the next ADTS sync word (0xFFF)
        unsigned char c;
        int state = 0;
        while (fread(&c, 1, 1, static_cast<FILE*>(file_)) == 1) {
            if (c == 0xFF) {
                state = 1;
            } else if (state == 1 && (c & 0xF0) == 0xF0) {
                // Found sync, step back 2 bytes so ReadAdtsHeader can re-read it
                fseek(static_cast<FILE*>(file_), -2, SEEK_CUR);
                break;
            } else {
                state = 0;
            }
        }
    }

    // Reset decoder state
    HaacDecoderClose(haac_);
    haac_ = HaacDecoderOpen();
    HaacDecoderInit(haac_, 1024);

    position_sec_ = offset_seconds;
    ESP_LOGI(TAG, "Seek to %.1fs (byte offset: %ld)", offset_seconds, ftell(static_cast<FILE*>(file_)));
    return position_sec_;
}

// ---- Internal ----

int AacDecoder::ReadAdtsHeader() {
    unsigned char hdr[ADTS_HEADER_SIZE];
    FILE* f = static_cast<FILE*>(file_);
    size_t read = fread(hdr, 1, ADTS_HEADER_SIZE, f);
    if (read < ADTS_HEADER_SIZE) return -1;

    // Verify sync word: 0xFFF
    if ((hdr[0] & 0xFF) != 0xFF || (hdr[1] & 0xF0) != 0xF0) {
        ESP_LOGW(TAG, "ADTS sync not found: %02X %02X", hdr[0], hdr[1]);
        return -1;
    }

    // Frame length (13 bits): bits 30-43 of header
    int frame_length = ((hdr[3] & 0x03) << 11) | (hdr[4] << 3) | ((hdr[5] >> 5) & 0x07);
    if (frame_length <= ADTS_HEADER_SIZE) return -1;

    return frame_length;
}

void AacDecoder::EstimateDuration() {
    FILE* f = static_cast<FILE*>(file_);
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, cur, SEEK_SET);

    // Estimate: at 128kbps, ~16KB/s; at 44100Hz stereo 16bit, ~176KB/s PCM
    // Rough estimate from file size and sample_rate
    if (sample_rate_ > 0 && channels_ > 0) {
        // AAC-LC 128kbps: compressed bytes / (bitrate/8) = approx duration
        // Use conservative: assume ~128kbps for estimate
        double bitrate = 128000.0;  // 128 kbps
        duration_sec_ = (double)total * 8.0 / bitrate;
    }
}
