#pragma once
#include <cmath>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace calibrator {
inline bool canForwardInput(unsigned frames, long long ageMilliseconds, bool minimized, bool dashboard) {
    return frames > 0 && ageMilliseconds >= 0 && ageMilliseconds <= 1500 && !minimized && !dashboard;
}

inline bool sameExecutablePath(const std::wstring& expected, const std::wstring& actual) {
    if (expected.empty() || expected.size() != actual.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto left = expected[i] == L'/' ? L'\\' : std::towlower(expected[i]);
        const auto right = actual[i] == L'/' ? L'\\' : std::towlower(actual[i]);
        if (left != right) return false;
    }
    return true;
}

struct Geometry {
    double widthMeters;
    double heightMeters;
    double distanceMeters;
};

// Verified against the installed EyeTrackingGuide level0: 60-degree vertical
// camera FOV, world-space canvas at z=2, scale .002 and size 890 by 570.
inline Geometry geometry(int width, int height) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("Invalid capture dimensions.");
    constexpr double depth = 2.0;
    const double physicalHeight = 2.0 * depth * std::tan(3.14159265358979323846 / 6.0);
    return {physicalHeight * width / height, physicalHeight, depth};
}

struct CropBounds { float uMin, vMin, uMax, vMax; };
inline CropBounds cropBounds(int frameWidth, int frameHeight, int x, int y, int width, int height) {
    if (frameWidth <= 0 || frameHeight <= 0 || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        width > frameWidth || height > frameHeight || x > frameWidth - width || y > frameHeight - height)
        throw std::invalid_argument("Calibrator client area is outside the capture frame.");
    return {static_cast<float>(x) / frameWidth, static_cast<float>(y) / frameHeight,
            static_cast<float>(x + width) / frameWidth, static_cast<float>(y + height) / frameHeight};
}

class ButtonEdges {
    bool initialized_ = false;
    bool previous_ = false;
public:
    bool update(bool pressed, bool dashboard) {
        const bool rising = initialized_ && pressed && !previous_ && !dashboard;
        previous_ = pressed;
        initialized_ = true;
        return rising;
    }
};

enum class Mode { Normal, Diagnose, Smoke };
inline Mode parseMode(const std::vector<std::wstring>& args) {
    if (args.empty()) return Mode::Normal;
    if (args.size() == 1 && args[0] == L"--diagnose") return Mode::Diagnose;
    if (args.size() == 1 && args[0] == L"--smoke-test") return Mode::Smoke;
    throw std::invalid_argument("Usage: SteamVREyeCalibrator.exe [--diagnose | --smoke-test]");
}
}
