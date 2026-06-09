#include "OpenCvCaptureBackends.h"

#include "Logger.h"

#include <opencv2/videoio.hpp>

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
