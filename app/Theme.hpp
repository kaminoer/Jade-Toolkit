// Theme.hpp — application-wide jade / phthalo-green theme.
//
// New for the native port (the Python GUI ran on stock Fusion): a dark
// green-tinted palette with phthalo-green accents. Buttons and inputs are
// square — border-radius is 0 everywhere by design.
#pragma once

class QApplication;

namespace theme {

// Core palette (also used by custom-painted widgets that can't take QSS).
inline constexpr const char* WINDOW_BG    = "#0F1713";  // near-black green
inline constexpr const char* BASE_BG      = "#131E18";  // views / inputs
inline constexpr const char* ALT_BASE_BG  = "#182620";  // alternating rows
inline constexpr const char* PANEL_BG     = "#1B2B23";  // buttons / headers
inline constexpr const char* BORDER       = "#2A4033";
inline constexpr const char* ACCENT       = "#1E8F6A";  // phthalo green
inline constexpr const char* ACCENT_HOVER = "#26AD82";
inline constexpr const char* ACCENT_DOWN  = "#157A58";
inline constexpr const char* TEXT         = "#D9E6DF";
inline constexpr const char* DIM_TEXT     = "#8FA89C";
inline constexpr const char* TOOLTIP_BG   = "#0C231A";

// Install Fusion style + palette + the QSS sheet on the application.
void apply(QApplication& app);

}  // namespace theme
