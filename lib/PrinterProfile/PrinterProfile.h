/**
 * @file PrinterProfile.h
 * @brief Protocol profiles describing how to recognise and talk to a printer.
 *
 * Discovery is strictly based on GATT service UUIDs: a BLE device is
 * considered a Cat Printer if it advertises (or, after an optional probe,
 * exposes) one of the UUIDs listed in a profile. Advertising names such as
 * "MX05", "GB01" or "YT01" are never used, because they are inconsistent
 * across vendors and firmware versions.
 *
 * Adding a new printer variant:
 *  1. Add the UUIDs and protocol family to kProfiles in PrinterProfile.cpp.
 *  2. If the packet format differs, implement a new catgfx::ProtocolEncoder.
 */
#pragma once

#include <NimBLEUUID.h>

#include <string>
#include <vector>

#include "CatProtocol.h"

struct PrinterProfile
{
    std::string profileName; ///< Unique profile identifier, persisted in NVS
    NimBLEUUID serviceUUID;  ///< Primary GATT service carrying the print characteristic
    NimBLEUUID writeUUID;    ///< Characteristic receiving print packets

    // ---- Extensions beyond the minimal profile ------------------------------
    NimBLEUUID notifyUUID;                   ///< Optional notification characteristic (status / flow control)
    std::vector<NimBLEUUID> advertisedUUIDs; ///< UUIDs that identify this profile in advertisements
    catgfx::ProtocolFamily protocol = catgfx::ProtocolFamily::CatGfxLegacy;
    uint16_t printWidthPx = 384; ///< Print head width in dots
    uint16_t dpi = 203;          ///< Print head resolution
};

namespace PrinterProfiles
{
    /** All built-in profiles, in matching priority order. */
    const std::vector<PrinterProfile> &all();

    /** Profile by name, or nullptr. */
    const PrinterProfile *findByName(const std::string &name);

    /** Profile matching an advertised/discovered service UUID, or nullptr. */
    const PrinterProfile *matchService(const NimBLEUUID &uuid);

    /** Fallback profile used when a persisted profile name is unknown. */
    const PrinterProfile &defaultProfile();
} // namespace PrinterProfiles
