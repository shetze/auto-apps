#pragma once
// Shared startup validation for the controller's demo-only SOME/IP client
// identification. A service ID alone does not set a vsomeip client ID.
// Client IDs are useful consistency checks, NOT secure authentication.
#include <vsomeip/vsomeip.hpp>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>

namespace vsomeip_identity {
inline bool init(const std::shared_ptr<vsomeip::application> &app,
                 vsomeip::client_t expected) {
    if (!app || !app->init()) {
        std::cerr << "VSOMEIP identity: application init failed\n";
        return false;
    }
    const auto actual = app->get_client();
    std::cerr << "VSOMEIP identity: " << app->get_name() << " client=0x"
              << std::hex << std::setw(4) << std::setfill('0') << actual
              << " expected=0x" << std::setw(4) << expected << std::dec;
    const char *configuration = std::getenv("VSOMEIP_CONFIGURATION");
    std::cerr << " config=" << (configuration ? configuration : "(default)") << '\n';
    if (actual != expected) {
        std::cerr << "VSOMEIP identity ERROR: unexpected client ID. Check "
                     "VSOMEIP_CONFIGURATION and application name; "
                     "refusing to start to avoid DENIED/TIMEOUT tuning requests.\n";
        return false;
    }
    return true;
}
inline void denied(const char *service, vsomeip::client_t actual,
                   vsomeip::client_t expected) {
    std::cerr << service << ": controller request rejected: SOME/IP client=0x"
              << std::hex << std::setw(4) << std::setfill('0') << actual
              << " expected=0x" << std::setw(4) << expected
              << std::dec << " (client ID is not authentication)\n";
}
} // namespace vsomeip_identity
