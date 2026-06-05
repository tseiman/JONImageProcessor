#pragma once

#include <string>
#include <string_view>

#ifndef JON_IMAGE_PROCESSOR_GIT_VERSION
#define JON_IMAGE_PROCESSOR_GIT_VERSION "unknown"
#endif

#ifndef JON_IMAGE_PROCESSOR_RELEASE_VERSION
#define JON_IMAGE_PROCESSOR_RELEASE_VERSION ""
#endif

#ifndef JON_IMAGE_PROCESSOR_BUILD_HOST
#define JON_IMAGE_PROCESSOR_BUILD_HOST "unknown"
#endif

inline bool jonImageProcessorHasReleaseVersion()
{
    return !std::string_view(JON_IMAGE_PROCESSOR_RELEASE_VERSION).empty();
}

inline std::string jonImageProcessorVersionText()
{
    std::string text = "JONImageProcessor ";
    if (jonImageProcessorHasReleaseVersion()) {
        text += JON_IMAGE_PROCESSOR_RELEASE_VERSION;
        text += ' ';
    }
    text += "git=";
    text += JON_IMAGE_PROCESSOR_GIT_VERSION;
    return text;
}

inline std::string_view jonImageProcessorReleaseVersionOrUnreleased()
{
    if (jonImageProcessorHasReleaseVersion()) {
        return JON_IMAGE_PROCESSOR_RELEASE_VERSION;
    }

    return "unreleased";
}
