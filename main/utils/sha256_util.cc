#include "sha256_util.h"

#include <mbedtls/sha256.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#define TAG "Sha256"

Sha256Hasher::Sha256Hasher() {}

Sha256Hasher::~Sha256Hasher() {}

void Sha256Hasher::Start() {
    mbedtls_sha256_init(&ctx_);
    mbedtls_sha256_starts(&ctx_, 0);  // 0 = SHA-256 (not SHA-224)
}

void Sha256Hasher::Update(const void* data, size_t len) {
    mbedtls_sha256_update(&ctx_, static_cast<const unsigned char*>(data), len);
}

std::string Sha256Hasher::Finish() {
    unsigned char hash[32];
    mbedtls_sha256_finish(&ctx_, hash);
    mbedtls_sha256_free(&ctx_);

    // Convert to lowercase hex string (64 chars)
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

std::string Sha256Hasher::Sha256File(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open file: %s", filepath.c_str());
        return "";
    }

    Sha256Hasher hasher;
    hasher.Start();

    unsigned char buf[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), fp)) > 0) {
        hasher.Update(buf, bytes_read);
    }
    fclose(fp);

    return hasher.Finish();
}
