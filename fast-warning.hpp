#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace fast_warning {
// Always 58 columns, symmetric, English-only and frame-independent.
inline std::vector<std::string> artwork() {
    const std::size_t width = 58;
    auto center = [](const std::string &s, std::size_t w) {
        const auto inner = s.size() > w ? s.substr(0, w) : s;
        const auto left = (w - inner.size()) / 2;
        return "|" + std::string(left, ' ') + inner +
               std::string(w - left - inner.size(), ' ') + "|";
    };
    const std::string edge = "+" + std::string(width - 2, '-') + "+";
    std::vector<std::string> out{
        edge,
        center("! ! !    HIGH SPEED    ! ! !", width - 2),
        center("", width - 2),
        center("   __o                           __o", width - 2),
        center(" _/\\___     >>>>>>>>>>>      ___/\\_", width - 2),
        center("|_ O _ O|                   |_ O _ O|", width - 2),
        center("", width - 2),
        center("VIDEO DISABLED", width - 2),
        center("EYES ON THE ROAD", width - 2),
        center("Follow speed limits and traffic rules.", width - 2),
        center("", width - 2),
        center("X / V  Exit video        Q  Quit", width - 2),
        edge,
    };
    return out;
}
} // namespace fast_warning
