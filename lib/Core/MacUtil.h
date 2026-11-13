/**
 * @file MacUtil.h
 * @brief Helpers to normalise BLE MAC addresses and derive printer ids.
 *
 * Canonical MAC format inside the bridge: lower-case, colon separated
 * ("aa:bb:cc:dd:ee:ff"). The printer id used in IPP URIs and NVS keys is the
 * same address without colons ("aabbccddeeff").
 */
#pragma once

#include <cctype>
#include <string>

namespace macutil
{

    /**
     * @brief Normalise a MAC address or printer id into canonical form.
     *
     * Accepts "AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff" and "aabbccddeeff".
     * @return canonical MAC or an empty string if the input is invalid.
     */
    inline std::string normalize(const std::string &in)
    {
        std::string hex;
        hex.reserve(12);
        for (char c : in)
        {
            if (c == ':' || c == '-')
                continue;
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return {};
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (hex.size() != 12)
            return {};
        std::string out;
        out.reserve(17);
        for (size_t i = 0; i < 12; i += 2)
        {
            if (i)
                out.push_back(':');
            out.append(hex, i, 2);
        }
        return out;
    }

    /** Printer id derived from a canonical MAC ("aabbccddeeff"). */
    inline std::string toId(const std::string &mac)
    {
        std::string id;
        id.reserve(12);
        for (char c : mac)
            if (c != ':')
                id.push_back(c);
        return id;
    }

    /** Last two bytes of the MAC in upper case, used for default names ("EEFF"). */
    inline std::string shortSuffix(const std::string &mac)
    {
        std::string id = toId(mac);
        std::string s = id.size() >= 4 ? id.substr(id.size() - 4) : id;
        for (auto &c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

} // namespace macutil
