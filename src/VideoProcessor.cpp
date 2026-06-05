#include "VideoProcessor.h"

#include "DisplayRenderer.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitRuntimeError = 2;
constexpr const char* WindowName = "JONImageProcessor";

cv::Mat createDummyMask(int width, int height, std::size_t frameIndex)
{
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    const int radius = std::max(8, std::min(width, height) / 5);
    const int usableWidth = std::max(1, width - 2 * radius);
    const int x = radius + static_cast<int>((frameIndex * 3) % usableWidth);
    const int y = height / 2;

    cv::circle(mask, cv::Point(x, y), radius, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    return mask;
}

cv::Mat applyMaskOverlay(const cv::Mat& frame, const cv::Mat& mask)
{
    cv::Mat colorOverlay(frame.size(), frame.type(), cv::Scalar(40, 40, 220));
    cv::Mat blended;
    cv::addWeighted(frame, 0.65, colorOverlay, 0.35, 0.0, blended);

    cv::Mat result = frame.clone();
    blended.copyTo(result, mask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    cv::drawContours(result, contours, -1, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

    return result;
}

bool shouldStopFromKey(int key)
{
    const int normalized = key & 0xff;
    return normalized == 27 || normalized == 'q' || normalized == 'Q';
}

} // namespace

VideoProcessor::VideoProcessor(ProcessorConfig config)
    : config_(std::move(config))
{
}

int VideoProcessor::run()
{
    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    cv::VideoCapture capture;

    if (!config_.inputPath.empty()) {
        if (config_.verbose) {
            std::cout << "Opening input file: " << config_.inputPath << '\n';
        }
        capture.open(config_.inputPath);
    } else {
        if (config_.verbose) {
            std::cout << "Opening camera device: " << config_.devicePath << '\n';
        }
        capture.open(config_.devicePath, cv::CAP_ANY);
    }

    if (!capture.isOpened()) {
        std::cerr << "Error: Could not open video source: "
                  << (!config_.inputPath.empty() ? config_.inputPath : config_.devicePath) << '\n';
        return ExitRuntimeError;
    }

    const cv::Size outputSize(config_.width, config_.height);
    const cv::Size maskSize(config_.maskWidth, config_.maskHeight);
    const bool writeFile = config_.outputMode == OutputMode::File;

    cv::VideoWriter writer;
    if (writeFile) {
        double fps = capture.get(cv::CAP_PROP_FPS);
        if (fps <= 1.0 || fps > 240.0) {
            fps = 30.0;
        }

        const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
        writer.open(config_.outputFile, fourcc, fps, outputSize, true);
        if (!writer.isOpened()) {
            std::cerr << "Error: Could not open output file: "
                      << config_.outputFile << '\n';
            return ExitRuntimeError;
        }

        if (config_.verbose) {
            std::cout << "Writing MP4 output: " << config_.outputFile << " @ " << fps << " fps\n";
        }
    } else {
        cv::namedWindow(WindowName, cv::WINDOW_NORMAL | cv::WINDOW_FREERATIO);
        cv::resizeWindow(WindowName, outputSize.width, outputSize.height);
        if (config_.fullscreen) {
            cv::setWindowProperty(WindowName, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
            cv::waitKey(1);
        }
    }

    std::size_t frameIndex = 0;
    cv::Mat frame;

    while (capture.read(frame)) {
        if (frame.empty()) {
            continue;
        }

        cv::Mat resized;
        cv::resize(frame, resized, outputSize, 0.0, 0.0, cv::INTER_LINEAR);

        cv::Mat maskWorkCopy;
        cv::resize(resized, maskWorkCopy, maskSize, 0.0, 0.0, cv::INTER_AREA);

        cv::Mat smallMask = createDummyMask(maskWorkCopy.cols, maskWorkCopy.rows, frameIndex);
        cv::Mat outputMask;
        cv::resize(smallMask, outputMask, outputSize, 0.0, 0.0, cv::INTER_NEAREST);

        cv::Mat outputFrame = applyMaskOverlay(resized, outputMask);

        if (writeFile) {
            writer.write(outputFrame);
        } else {
            const cv::Size displaySize = getWindowDisplaySize(WindowName, outputSize);
            const cv::Mat displayFrame = renderForDisplay(outputFrame, displaySize, config_.displayMode);
            cv::imshow(WindowName, displayFrame);
            if (shouldStopFromKey(cv::waitKey(1))) {
                break;
            }
        }

        ++frameIndex;
    }

    if (frameIndex == 0) {
        std::cerr << "Error: No frames could be read.\n";
        return ExitRuntimeError;
    }

    if (config_.verbose) {
        std::cout << "Processed frames: " << frameIndex << '\n';
    }

    return ExitOk;
}
