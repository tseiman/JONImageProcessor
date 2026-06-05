#include "DisplayEnvironment.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace {

#if defined(__linux__)
ScreenInfo detectPrimaryScreenLinux()
{
    FILE* pipe = popen("xrandr --current 2>/dev/null", "r");
    if (pipe == nullptr) {
        return ScreenInfo {};
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        const std::size_t marker = line.find('*');
        if (marker == std::string::npos) {
            continue;
        }

        std::size_t start = marker;
        while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t') {
            --start;
        }

        const std::string mode = line.substr(start, marker - start);
        const std::size_t separator = mode.find('x');
        if (separator == std::string::npos) {
            continue;
        }

        const int width = std::atoi(mode.substr(0, separator).c_str());
        const int height = std::atoi(mode.substr(separator + 1).c_str());
        pclose(pipe);

        if (width > 0 && height > 0) {
            return ScreenInfo {cv::Size(width, height), true};
        }
        return ScreenInfo {};
    }

    pclose(pipe);
    return ScreenInfo {};
}
#endif

} // namespace

ScreenInfo detectPrimaryScreen()
{
#ifdef __APPLE__
    const CGDirectDisplayID displayId = CGMainDisplayID();
    const std::size_t width = CGDisplayPixelsWide(displayId);
    const std::size_t height = CGDisplayPixelsHigh(displayId);
    if (width > 0 && height > 0) {
        return ScreenInfo {cv::Size(static_cast<int>(width), static_cast<int>(height)), true};
    }
    return ScreenInfo {};
#elif defined(__linux__)
    return detectPrimaryScreenLinux();
#else
    return ScreenInfo {};
#endif
}
