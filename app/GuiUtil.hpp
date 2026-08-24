// GuiUtil.hpp — tiny shared helpers for the GUI port.
#pragma once

#include <QString>
#include <string>

// f"0x{key:08X}" — uppercase hex key for display.
inline QString hex_key(quint32 key) {
    return QStringLiteral("0x")
           + QString::number(key, 16).rightJustified(8, QLatin1Char('0'))
                 .toUpper();
}

// f"0x{key:08x}" — lowercase hex key for op-dict serialization.
inline std::string hex_key_lower(quint32 key) {
    return ("0x"
            + QString::number(key, 16).rightJustified(8, QLatin1Char('0')))
        .toStdString();
}

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
