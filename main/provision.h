#ifndef PROVISION_H_
#define PROVISION_H_

#include <string>

/**
 * @brief Device provisioning utility for staging/testing.
 *
 * Implements "方案 B 预烧 token": writes the device JWT, WebSocket URL,
 * and device_uid directly to NVS so the device can authenticate with the
 * staging server without going through the full BLE provisioning flow.
 *
 * Usage:
 *   1. Set the staging values below (or via serial command at runtime)
 *   2. Call Provision::ApplyDefaults() once at startup if NVS is empty
 *   3. Or call Provision::SaveXxx() individually to update at runtime
 */
class Provision {
public:
    // ============================================================
    // Staging defaults — edit these for your environment
    // ============================================================
    // Primary staging URL (port 8000 — WebSocket 直连, bypasses nginx).
    // This is the working path; nginx :80 does not proxy WS yet.
    static constexpr const char* STAGING_WS_URL =
        "ws://***:8000/ws/terminal";

    // Fallback URL (port 80 — requires nginx WS proxy support).
    // Switch to this once nginx is configured to forward WebSocket.
    static constexpr const char* STAGING_WS_URL_FALLBACK =
        "ws://***:80/ws/terminal";

    // Long-lived staging JWT — fill in locally, do not commit secrets.
    static constexpr const char* STAGING_DEVICE_JWT =
        "***";

    // Device UID — sent as Device-Id header, must match JWT "duid" claim.
    static constexpr const char* STAGING_DEVICE_UID =
        "***";

    // VMR (Voice Message Recording) HTTP API base URL.
    // Port 80 works for HTTP even when WS is not proxied.
    static constexpr const char* STAGING_VMR_SERVER =
        "http://***";

    // Device ID used in VMR API paths (matches JWT "did" claim).
    static constexpr const char* STAGING_VMR_DEVICE_ID = "***";

    // Default protocol version (3 = compact binary header)
    static constexpr int STAGING_PROTOCOL_VERSION = 3;

    // ============================================================
    // Save to NVS (namespace: "websocket")
    // ============================================================

    /// Write the WebSocket server URL
    static void SaveWsUrl(const std::string& url);

    /// Write the device JWT (access_token)
    static void SaveToken(const std::string& token);

    /// Write the device UID (used as Device-Id header, must match JWT duid)
    static void SaveDeviceId(const std::string& device_id);

    /// Write the protocol version
    static void SaveProtocolVersion(int version);

    /// Apply all staging defaults if NVS is not yet provisioned.
    /// Safe to call at every boot — only writes if values are missing.
    static void ApplyDefaultsIfNeeded();

    /// Force-write all staging defaults (overwrites existing values)
    static void ApplyDefaults();

    /// Check whether staging provisioning is active.
    /// Returns true if NVS "websocket"/"url" matches the staging URL.
    static bool IsStagingProvisioned();
};

#endif // PROVISION_H_
