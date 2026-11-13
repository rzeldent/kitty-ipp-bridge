#include "PrinterProfile.h"

namespace
{
    std::vector<PrinterProfile> buildProfiles()
    {
        std::vector<PrinterProfile> profiles;

        // Classic Cat Printers (GB0x/GT01/MX0x/YT01 families).
        // GATT service 0xAE30, write without response on 0xAE01, notifications
        // (status + flow control) on 0xAE02. Many units advertise 0xAF30
        // instead of 0xAE30, both identify this profile.
        PrinterProfile legacy;
        legacy.profileName = "catgfx-ae30";
        legacy.serviceUUID = NimBLEUUID(static_cast<uint16_t>(0xAE30));
        legacy.writeUUID = NimBLEUUID(static_cast<uint16_t>(0xAE01));
        legacy.notifyUUID = NimBLEUUID(static_cast<uint16_t>(0xAE02));
        legacy.advertisedUUIDs = {NimBLEUUID(static_cast<uint16_t>(0xAE30)), NimBLEUUID(static_cast<uint16_t>(0xAF30))};
        legacy.protocol = catgfx::ProtocolFamily::CatGfxLegacy;
        legacy.printWidthPx = 384;
        legacy.dpi = 203;
        profiles.push_back(legacy);

        // TODO(catgfx): add further variants here, e.g. printers exposing the
        // print characteristic below a 0xFF00/0x18F0 service, or next
        // generation firmware using ProtocolFamily::CatGfxV2.
        return profiles;
    }
} // namespace

namespace PrinterProfiles
{
    const std::vector<PrinterProfile> &all()
    {
        static const std::vector<PrinterProfile> profiles = buildProfiles();
        return profiles;
    }

    const PrinterProfile *findByName(const std::string &name)
    {
        for (const auto &p : all())
            if (p.profileName == name)
                return &p;
        return nullptr;
    }

    const PrinterProfile *matchService(const NimBLEUUID &uuid)
    {
        for (const auto &p : all())
        {
            if (p.serviceUUID == uuid)
                return &p;
            for (const auto &adv : p.advertisedUUIDs)
                if (adv == uuid)
                    return &p;
        }
        return nullptr;
    }

    const PrinterProfile &defaultProfile() { return all().front(); }
} // namespace PrinterProfiles
