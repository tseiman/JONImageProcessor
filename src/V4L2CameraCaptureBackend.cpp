#include "V4L2CameraCaptureBackend.h"

#include "Logger.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef __linux__
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>
#endif

namespace {

#ifdef __linux__

constexpr unsigned int BufferCount = 4;
constexpr int PollTimeoutMs = 100;

int xioctl(int fd, unsigned long request, void* argument)
{
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::string errnoMessage()
{
    return std::strerror(errno);
}

unsigned int cameraFormatToV4L2(CameraFormat format)
{
    switch (format) {
    case CameraFormat::MJPG:
        return V4L2_PIX_FMT_MJPEG;
    case CameraFormat::YUYV:
        return V4L2_PIX_FMT_YUYV;
    }

    return V4L2_PIX_FMT_MJPEG;
}

std::string v4l2PixelFormatToString(unsigned int format)
{
    std::string text;
    text.push_back(static_cast<char>(format & 0xff));
    text.push_back(static_cast<char>((format >> 8) & 0xff));
    text.push_back(static_cast<char>((format >> 16) & 0xff));
    text.push_back(static_cast<char>((format >> 24) & 0xff));
    for (char& character : text) {
        if (character < 32 || character > 126) {
            character = '?';
        }
    }
    return text;
}

double fpsFromTimeperframe(const v4l2_fract& timeperframe)
{
    if (timeperframe.numerator == 0 || timeperframe.denominator == 0) {
        return 0.0;
    }

    return static_cast<double>(timeperframe.denominator) / static_cast<double>(timeperframe.numerator);
}

#endif

} // namespace

V4L2CameraCaptureBackend::~V4L2CameraCaptureBackend()
{
    close();
}

bool V4L2CameraCaptureBackend::open(const ProcessorConfig& config)
{
#ifdef __linux__
    LOG_INFO("Opening V4L2 camera device: " << config.devicePath);
    LOG_VERBOSE("Requested camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Requested camera size: " << config.width << "x" << config.height);

    fd_ = ::open(config.devicePath.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        LOG_ERROR("Failed to open V4L2 device: " << config.devicePath << ": " << errnoMessage());
        return false;
    }

    stopRequested_.store(false);
    if (!initializeDevice(config) || !initializeMmap() || !startStreaming()) {
        close();
        return false;
    }

    logActiveFormat(config);
    LOG_VERBOSE("V4L2 mmap buffers: " << buffers_.size());
    return true;
#else
    (void)config;
    LOG_ERROR("V4L2 capture backend is only available on Linux");
    return false;
#endif
}

bool V4L2CameraCaptureBackend::read(cv::Mat& frame)
{
#ifdef __linux__
    if (fd_ < 0 || !streaming_) {
        return false;
    }

    while (!stopRequested_.load()) {
        pollfd descriptor {};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        const int pollResult = poll(&descriptor, 1, PollTimeoutMs);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("V4L2 poll failed: " << errnoMessage());
            return false;
        }
        if (pollResult == 0) {
            continue;
        }

        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            LOG_ERROR("VIDIOC_DQBUF failed: " << errnoMessage());
            return false;
        }

        bool decoded = false;
        if (buffer.index < buffers_.size()) {
            decoded = decodeFrame(buffers_[buffer.index].start, buffer.bytesused, frame);
        }

        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("VIDIOC_QBUF failed: " << errnoMessage());
            return false;
        }

        if (decoded) {
            return true;
        }
        LOG_WARNING("Skipping invalid V4L2 frame");
    }

    return false;
#else
    (void)frame;
    return false;
#endif
}

double V4L2CameraCaptureBackend::fps() const
{
    return fps_;
}

void V4L2CameraCaptureBackend::interrupt()
{
    stopRequested_.store(true);
}

void V4L2CameraCaptureBackend::close()
{
#ifdef __linux__
    stopRequested_.store(true);
    if (fd_ >= 0 && streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
            LOG_WARNING("VIDIOC_STREAMOFF failed: " << errnoMessage());
        }
        streaming_ = false;
    }

    cleanupBuffers();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

std::string V4L2CameraCaptureBackend::name() const
{
    return "v4l2";
}

bool V4L2CameraCaptureBackend::isCamera() const
{
    return true;
}

bool V4L2CameraCaptureBackend::initializeDevice(const ProcessorConfig& config)
{
#ifdef __linux__
    v4l2_capability capability {};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        LOG_ERROR("VIDIOC_QUERYCAP failed: " << errnoMessage());
        return false;
    }

    const unsigned int capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
        ? capability.device_caps
        : capability.capabilities;

    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        LOG_ERROR("V4L2 device does not support video capture");
        return false;
    }

    if ((capabilities & V4L2_CAP_STREAMING) == 0) {
        LOG_ERROR("V4L2 device does not support streaming I/O");
        return false;
    }

    v4l2_format format {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<unsigned int>(config.width);
    format.fmt.pix.height = static_cast<unsigned int>(config.height);
    format.fmt.pix.pixelformat = cameraFormatToV4L2(config.cameraFormat);
    format.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
        LOG_ERROR("VIDIOC_S_FMT failed: " << errnoMessage());
        return false;
    }

    width_ = static_cast<int>(format.fmt.pix.width);
    height_ = static_cast<int>(format.fmt.pix.height);
    pixelFormat_ = format.fmt.pix.pixelformat;

    if (pixelFormat_ != V4L2_PIX_FMT_MJPEG && pixelFormat_ != V4L2_PIX_FMT_YUYV) {
        LOG_ERROR("Unsupported V4L2 pixel format: " << v4l2PixelFormatToString(pixelFormat_));
        return false;
    }

    v4l2_streamparm streamParm {};
    streamParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_PARM, &streamParm) == 0) {
        fps_ = fpsFromTimeperframe(streamParm.parm.capture.timeperframe);
    }

    return true;
#else
    (void)config;
    return false;
#endif
}

bool V4L2CameraCaptureBackend::initializeMmap()
{
#ifdef __linux__
    v4l2_requestbuffers request {};
    request.count = BufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0) {
        LOG_ERROR("VIDIOC_REQBUFS failed: " << errnoMessage());
        return false;
    }

    if (request.count < 2) {
        LOG_ERROR("V4L2 device provided too few mmap buffers");
        return false;
    }

    buffers_.resize(request.count);
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            LOG_ERROR("VIDIOC_QUERYBUF failed: " << errnoMessage());
            return false;
        }

        buffers_[index].length = buffer.length;
        buffers_[index].start = mmap(
            nullptr,
            buffer.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            static_cast<off_t>(buffer.m.offset));

        if (buffers_[index].start == MAP_FAILED) {
            buffers_[index].start = nullptr;
            LOG_ERROR("mmap failed for V4L2 buffer: " << errnoMessage());
            return false;
        }
    }

    return true;
#else
    return false;
#endif
}

bool V4L2CameraCaptureBackend::startStreaming()
{
#ifdef __linux__
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            LOG_ERROR("VIDIOC_QBUF failed: " << errnoMessage());
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("VIDIOC_STREAMON failed: " << errnoMessage());
        return false;
    }

    streaming_ = true;
    return true;
#else
    return false;
#endif
}

bool V4L2CameraCaptureBackend::decodeFrame(const void* data, std::size_t bytesUsed, cv::Mat& frame)
{
#ifdef __linux__
    if (pixelFormat_ == V4L2_PIX_FMT_MJPEG) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::vector<unsigned char> encoded(bytes, bytes + bytesUsed);
        frame = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (frame.empty()) {
            LOG_ERROR("cv::imdecode failed for MJPG frame");
            return false;
        }
        return true;
    }

    if (pixelFormat_ == V4L2_PIX_FMT_YUYV) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        cv::Mat yuyv(height_, width_, CV_8UC2, const_cast<unsigned char*>(bytes));
        cv::cvtColor(yuyv, frame, cv::COLOR_YUV2BGR_YUYV);
        return !frame.empty();
    }

    LOG_ERROR("Unsupported V4L2 pixel format: " << v4l2PixelFormatToString(pixelFormat_));
    return false;
#else
    (void)data;
    (void)bytesUsed;
    (void)frame;
    return false;
#endif
}

void V4L2CameraCaptureBackend::cleanupBuffers()
{
#ifdef __linux__
    for (Buffer& buffer : buffers_) {
        if (buffer.start != nullptr && buffer.length > 0) {
            if (munmap(buffer.start, buffer.length) < 0) {
                LOG_WARNING("munmap failed for V4L2 buffer: " << errnoMessage());
            }
        }
        buffer.start = nullptr;
        buffer.length = 0;
    }
    buffers_.clear();
#endif
}

void V4L2CameraCaptureBackend::logActiveFormat(const ProcessorConfig& config) const
{
#ifdef __linux__
    LOG_VERBOSE("Active V4L2 format: " << v4l2PixelFormatToString(pixelFormat_));
    LOG_VERBOSE("Active V4L2 size: " << width_ << "x" << height_);
    LOG_VERBOSE("Active V4L2 FPS: " << fps_);

    const std::string requestedFormat = cameraFormatToString(config.cameraFormat);
    const std::string activeFormat = v4l2PixelFormatToString(pixelFormat_);
    if (activeFormat != requestedFormat) {
        LOG_WARNING("Camera did not accept requested format. Requested "
            << requestedFormat << ", active " << activeFormat);
    }

    if (width_ != config.width || height_ != config.height) {
        LOG_WARNING("Camera did not accept requested size. Requested "
            << config.width << "x" << config.height << ", active "
            << width_ << "x" << height_);
    }

#else
    (void)config;
#endif
}
