#include <cmath>
#include <iostream>
#include <stdexcept>
#include "core.h"

static int checks = 0;
static void require(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        const auto wide = calibrator::geometry(1920, 1080);
        require(std::abs(wide.widthMeters - 4.1056019) < 0.00001,
                "60-degree OEM camera at 2m must preserve angular width");
        require(std::abs(wide.heightMeters - 2.3094011) < 0.00001,
                "OEM camera height must match world-space canvas");
        require(wide.distanceMeters == 2.0, "calibration targets must stay at OEM depth");
        const auto square = calibrator::geometry(1000, 1000);
        require(std::abs(square.widthMeters - square.heightMeters) < 0.00001,
                "aspect changes must not distort targets");
        bool rejected = false;
        try { calibrator::geometry(0, 1080); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid capture dimensions must be rejected");
        rejected = false;
        try { calibrator::geometry(1920, -1); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "negative capture dimensions must be rejected");
        const auto crop = calibrator::cropBounds(1280, 720, 8, 31, 1264, 681);
        require(std::abs(crop.uMin - 8.0f / 1280) < 0.00001f, "client crop left");
        require(std::abs(crop.vMax - 712.0f / 720) < 0.00001f, "client crop bottom");
        rejected = false;
        try { calibrator::cropBounds(100, 100, 0, 0, 101, 100); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "out-of-frame crop must fail instead of shifting targets");
        calibrator::ButtonEdges edges;
        require(!edges.update(true, false), "a held trigger must not start calibration on launch");
        require(!edges.update(false, false), "release does not trigger input");
        require(edges.update(true, false), "a new trigger press starts calibration");
        require(!edges.update(true, false), "holding trigger must not repeat");
        require(!edges.update(false, true), "dashboard consumes controller input");
        require(!edges.update(true, true), "dashboard press is suppressed");
        require(!edges.update(true, false), "closing dashboard with held trigger must not calibrate");
        require(!edges.update(false, false), "release after dashboard");
        require(edges.update(true, false), "new press after dashboard works");
        require(calibrator::parseMode({}) == calibrator::Mode::Normal, "double click opens utility");
        require(calibrator::parseMode({L"--diagnose"}) == calibrator::Mode::Diagnose, "diagnose mode");
        require(calibrator::parseMode({L"--smoke-test"}) == calibrator::Mode::Smoke, "smoke mode");
        rejected = false;
        try { calibrator::parseMode({L"--unknown"}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unknown flags must fail");
        rejected = false;
        try { calibrator::parseMode({L"--diagnose", L"--smoke-test"}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "conflicting modes must fail");
        require(calibrator::sameExecutablePath(L"C:\\Program Files\\Pimax/EyeTrackingGuide/EyeTrackingGuide.exe",
                                               L"c:\\program files\\pimax\\EyeTrackingGuide\\EyeTrackingGuide.exe"),
                "Win32-normalized paths must bind the installed OEM process");
        require(!calibrator::sameExecutablePath(L"C:\\Pimax\\EyeTrackingGuide.exe", L"D:\\Other\\EyeTrackingGuide.exe"),
                "matching window title or executable name must not select another installation");
        require(!calibrator::sameExecutablePath(L"", L""), "empty executable paths must never match");
        require(!calibrator::canForwardInput(0, 0, false, false), "never collect calibration without a submitted frame");
        require(calibrator::canForwardInput(1, 100, false, false), "recent visible frame enables explicit input");
        require(!calibrator::canForwardInput(30, 1501, false, false), "stale targets must suppress input");
        require(!calibrator::canForwardInput(30, 100, true, false), "minimized source must suppress input");
        require(!calibrator::canForwardInput(30, 100, false, true), "dashboard must suppress input");
        std::cout << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
