#ifndef SHA256_UTIL_H_
#define SHA256_UTIL_H_

#include <string>
#include <mbedtls/sha256.h>

/**
 * @brief SHA-256 hash utility using mbedTLS.
 *
 * Supports incremental (streaming) hashing for large files — call
 * Sha256Start(), then Sha256Update() as data arrives, finally
 * Sha256Finish() to get the hex digest.
 *
 * For convenience, Sha256File() computes the hash of an entire file
 * in one call.
 */
class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();

    /// Start a new SHA-256 context
    void Start();

    /// Feed data into the hash (callable multiple times)
    void Update(const void* data, size_t len);

    /// Finalize and return the hex digest (lowercase, 64 chars)
    std::string Finish();

    // ---- Convenience ----

    /// Compute SHA-256 hex digest of an entire file on disk.
    /// Returns empty string on error.
    static std::string Sha256File(const std::string& filepath);

private:
    mbedtls_sha256_context ctx_;
};

#endif // SHA256_UTIL_H_
