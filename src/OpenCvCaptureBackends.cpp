#include "OpenCvCaptureBackends.h"

#include "Logger.h"

#include <opencv2/videoio.hpp>

#include <cmath>
#include <sstream>

namespace {

int cameraFormatFourcc(CameraFormat format)
{
    switch (format) {
    case CameraFormat::MJPG:
        return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    case CameraFormat::YUYV:
        return cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V');
    }

    return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
}

std::string fourccToString(double fourccValue)
{
    const auto fourcc = static_cast<int>(std::llround(fourccValue));
    std::string text;
    text.push_back(static_cast<char>(fourcc & 0xff));
    text.push_back(static_cast<char>((fourcc >> 8) & 0xff));
    text.push_back(static_cast<char>((fourcc >> 16) & 0xff));
    text.push_back(static_cast<char>((fourcc >> 24) & 0xff));

    for (char& character : text) {
        if (character < 32 || character > 126) {
            character = '?';
        }
    }

    return text;
}

bool almostEqual(double left, double right, double tolerance)
{
    return std::fabs(left - right) <= tolerance;
}

void logRequestedCameraConfig(const ProcessorConfig& config)
{
    LOG_VERBOSE("Requested camera device: " << config.devicePath);
    LOG_VERBOSE("Requested camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Requested camera size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Requested camera FPS: " << config.cameraFps);
}

void configureCameraCapture(cv::VideoCapture& capture, const ProcessorConfig& config)
{
    LOG_VERBOSE("Camera capture buffer size requested: 1");
    capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
    capture.set(cv::CAP_PROP_FOURCC, cameraFormatFourcc(config.cameraFormat));
    capture.set(cv::CAP_PROP_FRAME_WIDTH, config.width);
    capture.set(cv::CAP_PROP_FRAME_HEIGHT, config.height);
    capture.set(cv::CAP_PROP_FPS, config.cameraFps);

    const std::string activeFormat = fourccToString(capture.get(cv::CAP_PROP_FOURCC));
    const double activeWidth = capture.get(cv::CAP_PROP_FRAME_WIDTH);
    const double activeHeight = capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    const double activeFps = capture.get(cv::CAP_PROP_FPS);

    LOG_VERBOSE("Active camera format: " << activeFormat);
    LOG_VERBOSE("Active camera size: " << static_cast<int>(std::llround(activeWidth))
        << "x" << static_cast<int>(std::llround(activeHeight)));
    LOG_VERBOSE("Active camera FPS: " << activeFps);

    const std::string requestedFormat = cameraFormatToString(config.cameraFormat);
    if (activeFormat != requestedFormat) {
        LOG_WARNING("Camera did not accept requested format. Requested "
            << requestedFormat << ", active " << activeFormat);
    }

    if (!almostEqual(activeWidth, config.width, 0.5) || !almostEqual(activeHeight, config.height, 0.5)) {
        LOG_WARNING("Camera did not accept requested size. Requested "
            << config.width << "x" << config.height << ", active "
            << static_cast<int>(std::llround(activeWidth)) << "x" << static_cast<int>(std::llround(activeHeight)));
    }

    if (!almostEqual(activeFps, config.cameraFps, 0.5)) {
        LOG_WARNING("Camera did not accept requested FPS. Requested "
            << config.cameraFps << ", active " << activeFps);
    }
}

} // namespace

bool OpenCvFileCaptureBackend::open(const ProcessorConfig& config)
{
    LOG_INFO("Opening input video: " << config.inputPath);
    capture_.open(config.inputPath);
    if (!capture_.isOpened()) {
        LOG_ERROR("Cannot open input file: " << config.inputPath);
        return false;
    }

    return true;
}

bool OpenCvFileCaptureBackend::read(cv::Mat& frame)
{
    return capture_.read(frame);
}

double OpenCvFileCaptureBackend::fps() const
{
    return capture_.get(cv::CAP_PROP_FPS);
}

void OpenCvFileCaptureBackend::interrupt()
{
}

void OpenCvFileCaptureBackend::close()
{
    capture_.release();
}

std::string OpenCvFileCaptureBackend::name() const
{
    return "opencv";
}

bool OpenCvFileCaptureBackend::isCamera() const
{
    return false;
}

bool OpenCvCameraCaptureBackend::open(const ProcessorConfig& config)
{
    LOG_INFO("Opening OpenCV camera device: " << config.devicePath);
    logRequestedCameraConfig(config);
    capture_.open(config.devicePath, cv::CAP_ANY);
    if (!capture_.isOpened()) {
        LOG_ERROR("Cannot open camera device: " << config.devicePath);
        return false;
    }

    configureCameraCapture(capture_, config);
    return true;
}

bool OpenCvCameraCaptureBackend::read(cv::Mat& frame)
{
    return capture_.read(frame);
}

double OpenCvCameraCaptureBackend::fps() const
{
    return capture_.get(cv::CAP_PROP_FPS);
}

void OpenCvCameraCaptureBackend::interrupt()
{
}

void OpenCvCameraCaptureBackend::close()
{
    capture_.release();
}

std::string OpenCvCameraCaptureBackend::name() const
{
    return "opencv";
}

bool OpenCvCameraCaptureBackend::isCamera() const
{
    return true;
}
