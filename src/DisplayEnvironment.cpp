#include "DisplayEnvironment.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#if defined(JON_ENABLE_DRM_DISPLAY)
#include <fcntl.h>
#include <unistd.h>
#include <xf86drmMode.h>
#endif

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

#if defined(__linux__) && defined(JON_ENABLE_DRM_DISPLAY)
int detectConnectedDrmDisplayCountLinux()
{
    for (int index = 0; index < 8; ++index) {
        const std::string path = "/dev/dri/card" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        int connected = 0;
        drmModeRes* resources = drmModeGetResources(fd);
        if (resources) {
            for (int connectorIndex = 0; connectorIndex < resources->count_connectors; ++connectorIndex) {
                drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[connectorIndex]);
                if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
                    ++connected;
                }
                if (connector) {
                    drmModeFreeConnector(connector);
                }
            }
            drmModeFreeResources(resources);
        }

        close(fd);
        if (connected > 0) {
            return connected;
        }
    }

    return 0;
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

int detectConnectedDrmDisplayCount()
{
#if defined(__linux__) && defined(JON_ENABLE_DRM_DISPLAY)
    return detectConnectedDrmDisplayCountLinux();
#else
    return 0;
#endif
}
