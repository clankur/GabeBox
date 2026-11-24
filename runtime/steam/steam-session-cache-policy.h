#pragma once

#include <cstdint>
#include <cstring>

namespace steam_session_cache {

// Accessing the network, writing the report or loading application metadata may
// fail after a valid DPAPI record has been read. A restore fault at that point
// says nothing about the encrypted credential.
constexpr bool invalidRecordRead(bool recordLoaded, std::uint32_t error) {
    if (recordLoaded) return false;
    switch (error) {
        case 0x80070057u: // E_INVALIDARG from record validation, including expiry
        case 0x80090005u: // NTE_BAD_DATA from DPAPI
        case 0x8007000du: // HRESULT_FROM_WIN32(ERROR_INVALID_DATA)
        case 0x80070002u: // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            return true;
        default:
            return false;
    }
}

// Valve's isteamuser.h defines SteamServerConnectFailure_t as callback 102
// with a 32-bit EResult followed by m_bStillRetrying.
inline bool credentialRejected(std::int32_t user, std::int32_t eventUser, std::int32_t message,
        const void *data, std::int32_t length) {
    if (user != eventUser || message != 102 || !data || length < 5) return false;
    const auto payload = static_cast<const std::uint8_t *>(data);
    std::int32_t result = 0;
    std::memcpy(&result, payload, sizeof(result));
    if (payload[sizeof(result)] != 0) return false;
    switch (result) {
        case 5:  // InvalidPassword
        case 18: // AccountNotFound
        case 19: // InvalidSteamID
        case 26: // Revoked
        case 27: // Expired
        case 43: // AccountDisabled
            return true;
        default:
            return false;
    }
}

}
