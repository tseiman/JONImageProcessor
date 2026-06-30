#include "VideoProcessor.h"

#include "BenchmarkRecorder.h"
#include "CaptureBackendFactory.h"
#include "DisplayBackendFactory.h"
#include "DisplayEnvironment.h"
#include "HtmlMediaRenderer.h"
#include "ICaptureBackend.h"
#include "Logger.h"
#include "LowLatencyFrameCapture.h"
#include "MaskBackends.h"
#include "ShutdownSignal.h"
#include "Version.h"
#include "ipc/IPCServer.h"
#include "ipc/RuntimeState.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#if defined(JON_ENABLE_GSTREAMER_APP)
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#endif

#if defined(JON_ENABLE_NVBUFSURFACE)
#include <nvbufsurface.h>
#endif

#if defined(JON_ENABLE_FREETYPE_TEXT)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitRuntimeError = 2;
constexpr auto CameraReconnectInterval = std::chrono::seconds(5);
constexpr auto CameraReconnectSettleTime = std::chrono::seconds(3);
constexpr auto DisplayReconnectInterval = std::chrono::seconds(3);

std::time_t fileMtime(const std::string& path);

struct BackgroundEffectBuffers {
    cv::Mat downscaledFrame;
    cv::Mat downscaledBlurredFrame;
    cv::Mat blurredFrame;
    cv::Mat scaledBackgroundImage;
    cv::Mat result;
};

bool isAbsolutePath(const std::string& path)
{
    return !path.empty() && path.front() == '/';
}

std::string mediaPath(const std::string& folder, const std::string& path)
{
    if (path.empty() || isAbsolutePath(path) || folder.empty() || folder == ".") {
        return path;
    }
    if (folder.back() == '/') return folder + path;
    return folder + "/" + path;
}

bool looksLikeHtmlFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file) return false;
    std::string prefix(512, '\0');
    file.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(file.gcount()));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto first = prefix.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    prefix.erase(0, first);
    return prefix.rfind("<!doctype html", 0) == 0
        || prefix.rfind("<html", 0) == 0
        || prefix.find("<head") != std::string::npos
        || prefix.find("<body") != std::string::npos;
}

struct MediaFile {
    cv::Mat image;
    cv::Mat lastVideoFrame;
    cv::VideoCapture video;
    std::unique_ptr<HtmlMediaRenderer> html;
    std::string path;
    std::time_t mtime = 0;
    cv::Size htmlSize;
    double videoFps = 0.0;
    double videoFrameCount = 0.0;
    std::thread videoThread;
    std::mutex videoMutex;
    std::atomic<bool> stopVideoThread {false};
    bool loop = false;
    bool isVideo = false;
    bool isHtml = false;
    bool warnedFrameFailure = false;

    ~MediaFile()
    {
        release();
        if (html) {
            // WPEBackend-fdo has shown crashes on Jetson when its Wayland/EGL
            // proxies are torn down during runtime switches. Let the OS reclaim
            // the renderer at process exit instead of destructing it here.
            (void)html.release();
        }
    }

    void stopVideoDecoder()
    {
        stopVideoThread = true;
        if (videoThread.joinable()) {
            videoThread.join();
        }
        video.release();
        stopVideoThread = false;
    }

    void release()
    {
        stopVideoDecoder();
        if (html) html->reset();
        image.release();
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            lastVideoFrame.release();
        }
        path.clear();
        mtime = 0;
        htmlSize = {};
        videoFps = 0.0;
        videoFrameCount = 0.0;
        isVideo = false;
        isHtml = false;
        warnedFrameFailure = false;
        loop = false;
    }

    void replaceWithLoadedHtml(const std::string& nextPath, std::time_t nextMtime, bool nextLoop, const cv::Size& renderSize)
    {
        stopVideoDecoder();
        image.release();
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            lastVideoFrame.release();
        }
        path = nextPath;
        mtime = nextMtime;
        loop = nextLoop;
        htmlSize = renderSize;
        videoFps = 0.0;
        videoFrameCount = 0.0;
        isVideo = false;
        isHtml = true;
        warnedFrameFailure = false;
    }

    void startVideoDecoder()
    {
        const double fps = videoFps > 0.0 ? videoFps : 25.0;
        const auto frameDelay = std::chrono::duration<double>(1.0 / fps);
        videoThread = std::thread([this, frameDelay]() {
            auto nextFrameAt = std::chrono::steady_clock::now();
            while (!stopVideoThread) {
                cv::Mat decoded;
                if (!video.read(decoded) || decoded.empty()) {
                    if (loop) {
                        video.set(cv::CAP_PROP_POS_FRAMES, 0.0);
                        nextFrameAt = std::chrono::steady_clock::now();
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(videoMutex);
                    lastVideoFrame = decoded;
                }
                nextFrameAt += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameDelay);
                std::this_thread::sleep_until(nextFrameAt);
                if (std::chrono::steady_clock::now() > nextFrameAt + std::chrono::seconds(1)) {
                    nextFrameAt = std::chrono::steady_clock::now();
                }
            }
        });
    }

    bool ensureLoaded(const std::string& nextPath, bool nextLoop, const cv::Size& renderSize, const std::string& label)
    {
        const std::time_t nextMtime = fileMtime(nextPath);
        if (path == nextPath && mtime == nextMtime && (!isHtml || htmlSize == renderSize)) {
            loop = nextLoop;
            return !image.empty() || video.isOpened() || !lastVideoFrame.empty() || isHtml;
        }
        const bool hadMedia = !path.empty();

        if (looksLikeHtmlFile(nextPath)) {
            if (!HtmlMediaRenderer::supported()) {
                LOG_ERROR("HTML media is not supported in this build: " << nextPath);
                return hadMedia;
            }
            if (!html) {
                html = std::make_unique<HtmlMediaRenderer>();
            }
            std::string error;
            if (!html->load(nextPath, renderSize, error)) {
                LOG_ERROR("Cannot load " << label << " HTML media: " << error);
                return hadMedia;
            }
            replaceWithLoadedHtml(nextPath, nextMtime, nextLoop, renderSize);
            LOG_INFO("Loaded " << label << " HTML media: " << nextPath);
            return true;
        }

        cv::Mat nextImage = cv::imread(nextPath, cv::IMREAD_COLOR);
        if (!nextImage.empty()) {
            release();
            path = nextPath;
            mtime = nextMtime;
            loop = nextLoop;
            image = std::move(nextImage);
            LOG_INFO("Loaded " << label << " image media: " << nextPath);
            return true;
        }

        cv::VideoCapture nextVideo(nextPath);
        if (!nextVideo.isOpened()) {
            LOG_ERROR("Cannot load " << label << " media: " << nextPath);
            return hadMedia;
        }
        release();
        path = nextPath;
        mtime = nextMtime;
        loop = nextLoop;
        video = std::move(nextVideo);
        videoFps = video.get(cv::CAP_PROP_FPS);
        videoFrameCount = video.get(cv::CAP_PROP_FRAME_COUNT);
        LOG_INFO("Loaded " << label << " video media: " << nextPath
            << " fps=" << videoFps << " frames=" << videoFrameCount);
        isVideo = true;
        startVideoDecoder();
        return true;
    }

    bool read(cv::Mat& frame)
    {
        if (isHtml && html) {
            std::string error;
            return html->render(frame, error);
        }
        if (!image.empty()) {
            frame = image;
            return true;
        }
        std::lock_guard<std::mutex> lock(videoMutex);
        if (lastVideoFrame.empty()) {
            return false;
        }
        frame = lastVideoFrame;
        return true;
    }
};

struct PauseCameraSource {
    struct State {
        std::mutex mutex;
        std::thread worker;
        std::string desiredPipeline;
        std::string activePipeline;
        cv::Mat lastFrame;
        bool hasFrame = false;
        bool started = false;
        bool stop = false;
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    std::string warnedOpenFailurePipeline;
    std::string warnedReadFailurePipeline;

#if defined(JON_ENABLE_GSTREAMER_APP)
    struct GstObjectDeleter {
        void operator()(GstElement* element) const { if (element) gst_object_unref(element); }
        void operator()(GstSample* sample) const { if (sample) gst_sample_unref(sample); }
        void operator()(GstCaps* caps) const { if (caps) gst_caps_unref(caps); }
        void operator()(GstBus* bus) const { if (bus) gst_object_unref(bus); }
        void operator()(GstMessage* message) const { if (message) gst_message_unref(message); }
    };

    using GstElementPtr = std::unique_ptr<GstElement, GstObjectDeleter>;
    using GstSamplePtr = std::unique_ptr<GstSample, GstObjectDeleter>;
    using GstBusPtr = std::unique_ptr<GstBus, GstObjectDeleter>;
    using GstMessagePtr = std::unique_ptr<GstMessage, GstObjectDeleter>;

    static void ensureGStreamerInitialized()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            gst_init(nullptr, nullptr);
        });
    }

    static GstElement* findAppSink(GstElement* pipeline)
    {
        GstElement* namedSink = gst_bin_get_by_name(GST_BIN(pipeline), "airplay_sink");
        if (namedSink && GST_IS_APP_SINK(namedSink)) {
            return namedSink;
        }
        if (namedSink) {
            gst_object_unref(namedSink);
        }

        GstIterator* iterator = gst_bin_iterate_sinks(GST_BIN(pipeline));
        GValue value = G_VALUE_INIT;
        while (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
            auto* element = GST_ELEMENT(g_value_get_object(&value));
            if (element && GST_IS_APP_SINK(element)) {
                gst_object_ref(element);
                g_value_unset(&value);
                gst_iterator_free(iterator);
                return element;
            }
            g_value_unset(&value);
        }
        gst_iterator_free(iterator);
        return nullptr;
    }

    static void logFrameContentDiagnostics(const cv::Mat& frame, const std::string& sampleFormat, std::size_t stride, std::size_t bufferSize)
    {
        if (frame.empty()) {
            return;
        }

        static std::mutex diagnosticsMutex;
        static std::uint64_t frameCount = 0;
        static std::chrono::steady_clock::time_point lastLogTime;
        static std::chrono::steady_clock::time_point lastBlackWarningTime;
        static bool dumpedBlackFrame = false;
        static bool dumpedOkFrame = false;

        const auto now = std::chrono::steady_clock::now();
        bool shouldInspect = false;
        {
            std::lock_guard<std::mutex> lock(diagnosticsMutex);
            ++frameCount;
            if (frameCount % 30 == 0 || lastLogTime.time_since_epoch().count() == 0
                || now - lastLogTime >= std::chrono::seconds(1)) {
                shouldInspect = true;
                lastLogTime = now;
            }
        }
        if (!shouldInspect) {
            return;
        }

        const cv::Scalar meanValue = cv::mean(frame);
        cv::Mat gray;
        double minValue = 0.0;
        double maxValue = 0.0;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::minMaxLoc(gray, &minValue, &maxValue);
        cv::Mat brightMask;
        cv::threshold(gray, brightMask, 16.0, 255.0, cv::THRESH_BINARY);
        const double brightRatio = static_cast<double>(cv::countNonZero(brightMask)) / static_cast<double>(gray.rows * gray.cols);

        const bool isBlack = meanValue[0] < 8.0 && meanValue[1] < 8.0 && meanValue[2] < 8.0 && brightRatio < 0.03;
        bool dumpBlack = false;
        bool dumpOk = false;
        bool warnBlack = false;
        {
            std::lock_guard<std::mutex> lock(diagnosticsMutex);
            if (isBlack) {
                warnBlack = lastBlackWarningTime.time_since_epoch().count() == 0
                    || now - lastBlackWarningTime >= std::chrono::seconds(1);
                if (warnBlack) {
                    lastBlackWarningTime = now;
                }
                dumpBlack = !dumpedBlackFrame;
                dumpedBlackFrame = true;
            } else {
                dumpOk = !dumpedOkFrame;
                dumpedOkFrame = true;
            }
        }

        LOG_INFO("Secondary camera frame content: size=" << frame.cols << "x" << frame.rows
                 << " format=" << sampleFormat
                 << " stride=" << stride
                 << " buffer=" << bufferSize
                 << " mean_bgr=" << meanValue[0] << "," << meanValue[1] << "," << meanValue[2]
                 << " gray_minmax=" << minValue << "," << maxValue
                 << " bright_ratio=" << brightRatio);
        if (warnBlack) {
            LOG_WARNING("Secondary camera frame content is black although caps are valid");
        }
        if (dumpBlack) {
            cv::imwrite("/tmp/jon-airplay-black-debug.jpg", frame);
        }
        if (dumpOk) {
            cv::imwrite("/tmp/jon-airplay-ok-debug.jpg", frame);
        }
    }

    static bool isNearlyBlackFrame(const cv::Mat& frame)
    {
        if (frame.empty()) {
            return true;
        }
        const cv::Scalar meanValue = cv::mean(frame);
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::Mat brightMask;
        cv::threshold(gray, brightMask, 16.0, 255.0, cv::THRESH_BINARY);
        const double brightRatio = static_cast<double>(cv::countNonZero(brightMask)) / static_cast<double>(gray.rows * gray.cols);
        return meanValue[0] < 8.0 && meanValue[1] < 8.0 && meanValue[2] < 8.0 && brightRatio < 0.03;
    }

#if defined(JON_ENABLE_NVBUFSURFACE)
    static bool tryNvBufSurfaceToBgrMat(GstSample* sample, cv::Mat& frame, std::string& error)
    {
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (!caps || !buffer || gst_caps_get_size(caps) < 1) {
            return false;
        }

        GstStructure* structure = gst_caps_get_structure(caps, 0);
        const char* memoryType = gst_structure_get_string(structure, "nvbuf-memory-type");
        if (!memoryType) {
            return false;
        }

        const char* format = gst_structure_get_string(structure, "format");
        const std::string sampleFormat(format ? format : "");
        const bool isPackedRgb = sampleFormat == "BGR" || sampleFormat == "RGB"
            || sampleFormat == "BGRx" || sampleFormat == "BGRA" || sampleFormat == "RGBA";
        if (!isPackedRgb) {
            std::ostringstream stream;
            stream << "NvBufSurface: unsupported NVMM appsink format: " << sampleFormat;
            error = stream.str();
            return true;
        }

        GstMapInfo map {};
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            error = "NvBufSurface: cannot map GstBuffer";
            return true;
        }

        auto unmapGstAndReturn = [&](bool handled) {
            gst_buffer_unmap(buffer, &map);
            return handled;
        };

        if (map.size < sizeof(NvBufSurface)) {
            std::ostringstream stream;
            stream << "NvBufSurface: GstBuffer too small for NvBufSurface struct: "
                   << map.size << "/" << sizeof(NvBufSurface);
            error = stream.str();
            return unmapGstAndReturn(true);
        }

        auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
        if (!surface || surface->numFilled == 0 || surface->surfaceList == nullptr) {
            error = "NvBufSurface: surface is null or empty";
            return unmapGstAndReturn(true);
        }

        if (NvBufSurfaceMap(surface, 0, 0, NVBUF_MAP_READ) != 0) {
            error = "NvBufSurface: NvBufSurfaceMap() failed";
            return unmapGstAndReturn(true);
        }

        auto unmapSurfaceAndGstAndReturn = [&](bool handled) {
            NvBufSurfaceUnMap(surface, 0, 0);
            gst_buffer_unmap(buffer, &map);
            return handled;
        };

        if (NvBufSurfaceSyncForCpu(surface, 0, 0) != 0) {
            error = "NvBufSurface: NvBufSurfaceSyncForCpu() failed";
            return unmapSurfaceAndGstAndReturn(true);
        }

        NvBufSurfaceParams& params = surface->surfaceList[0];
        const int width = static_cast<int>(params.width);
        const int height = static_cast<int>(params.height);
        const std::size_t stride = static_cast<std::size_t>(params.planeParams.pitch[0] > 0 ? params.planeParams.pitch[0] : params.pitch);
        auto* data = static_cast<std::uint8_t*>(params.mappedAddr.addr[0]);
        if (!data || width <= 0 || height <= 0 || stride == 0) {
            error = "NvBufSurface: invalid mapped address or dimensions";
            return unmapSurfaceAndGstAndReturn(true);
        }

        static std::once_flag nvPathLogFlag;
        std::call_once(nvPathLogFlag, [&]() {
            LOG_INFO("Secondary camera using NvBufSurface CPU mapping"
                << " memory=" << memoryType
                << " format=" << sampleFormat
                << " size=" << width << "x" << height
                << " stride=" << stride);
        });

        const int channels = (sampleFormat == "BGR" || sampleFormat == "RGB") ? 3 : 4;
        cv::Mat wrapped(height, width, channels == 3 ? CV_8UC3 : CV_8UC4, data, stride);
        if (sampleFormat == "BGR") {
            frame = wrapped.clone();
        } else if (sampleFormat == "RGB") {
            cv::cvtColor(wrapped, frame, cv::COLOR_RGB2BGR);
        } else if (sampleFormat == "RGBA") {
            cv::cvtColor(wrapped, frame, cv::COLOR_RGBA2BGR);
        } else {
            frame.create(height, width, CV_8UC3);
            cv::cvtColor(wrapped, frame, cv::COLOR_BGRA2BGR);
        }

        logFrameContentDiagnostics(frame, sampleFormat, stride, static_cast<std::size_t>(params.dataSize));
        return unmapSurfaceAndGstAndReturn(true);
    }
#endif

    static bool sampleToBgrMat(GstSample* sample, cv::Mat& frame, std::string& error)
    {
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (!caps || !buffer || gst_caps_get_size(caps) < 1) {
            error = "appsink sample has no caps or buffer";
            return false;
        }

        GstStructure* structure = gst_caps_get_structure(caps, 0);
        int width = 0;
        int height = 0;
        const char* format = gst_structure_get_string(structure, "format");
        if (!format || !gst_structure_get_int(structure, "width", &width) || !gst_structure_get_int(structure, "height", &height)
            || width <= 0 || height <= 0) {
            error = "appsink sample has invalid caps";
            return false;
        }
        const std::string sampleFormat(format);

#if defined(JON_ENABLE_NVBUFSURFACE)
        if (tryNvBufSurfaceToBgrMat(sample, frame, error)) {
            return error.empty() && !frame.empty();
        }
#endif

        GstMapInfo map {};
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            error = "cannot map appsink buffer";
            return false;
        }
        const std::size_t bufferSize = map.size;

        GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
        const int channels = (sampleFormat == "BGR" || sampleFormat == "RGB") ? 3
            : ((sampleFormat == "BGRx" || sampleFormat == "BGRA" || sampleFormat == "RGBA") ? 4 : 0);
        std::size_t stride = channels > 0 ? static_cast<std::size_t>(width) * static_cast<std::size_t>(channels) : static_cast<std::size_t>(width);
        if (meta) {
            if (meta->stride[0] > 0) {
                stride = static_cast<std::size_t>(meta->stride[0]);
            }
        } else if (height > 0 && bufferSize % static_cast<std::size_t>(height) == 0) {
            const std::size_t inferredStride = bufferSize / static_cast<std::size_t>(height);
            if (inferredStride >= stride) {
                stride = inferredStride;
            }
        }

        gchar* capsTextRaw = gst_caps_to_string(caps);
        const std::string capsText = capsTextRaw ? capsTextRaw : "";
        if (capsTextRaw) {
            g_free(capsTextRaw);
        }
        std::ostringstream signatureStream;
        signatureStream << sampleFormat << " " << width << "x" << height << " stride=" << stride
                        << " buffer=" << bufferSize << " caps=" << capsText;
        static std::mutex lastCapsMutex;
        static std::string lastCapsSignature;
        const std::string capsSignature = signatureStream.str();
        {
            std::lock_guard<std::mutex> lock(lastCapsMutex);
            if (capsSignature != lastCapsSignature) {
                LOG_INFO("Secondary camera appsink caps: " << capsSignature);
                lastCapsSignature = capsSignature;
            }
        }

        auto planeOffset = [meta](int plane) -> std::size_t {
            return meta && plane < static_cast<int>(meta->n_planes) ? static_cast<std::size_t>(meta->offset[plane]) : 0U;
        };
        auto planeStride = [meta](int plane, std::size_t fallback) -> std::size_t {
            return meta && plane < static_cast<int>(meta->n_planes) && meta->stride[plane] > 0
                ? static_cast<std::size_t>(meta->stride[plane])
                : fallback;
        };
        auto failBufferTooSmall = [&](std::size_t expected) {
            std::ostringstream stream;
            stream << "appsink buffer too small: " << bufferSize << "/" << expected << " caps=" << capsText;
            error = stream.str();
        };

        if (sampleFormat == "NV12") {
            if (width % 2 != 0 || height % 2 != 0) {
                std::ostringstream stream;
                stream << "NV12 appsink sample has odd size: " << width << "x" << height << " caps=" << capsText;
                error = stream.str();
                gst_buffer_unmap(buffer, &map);
                return false;
            }
            const std::size_t yStride = planeStride(0, static_cast<std::size_t>(width));
            const std::size_t uvStride = planeStride(1, static_cast<std::size_t>(width));
            const std::size_t yOffset = planeOffset(0);
            const std::size_t uvOffset = meta ? planeOffset(1) : yStride * static_cast<std::size_t>(height);
            const std::size_t expected = uvOffset + uvStride * static_cast<std::size_t>(height / 2);
            if (bufferSize < expected) {
                failBufferTooSmall(expected);
                gst_buffer_unmap(buffer, &map);
                return false;
            }
            cv::Mat yPlane(height, width, CV_8UC1, map.data + yOffset, yStride);
            cv::Mat uvPlane(height / 2, width / 2, CV_8UC2, map.data + uvOffset, uvStride);
            cv::cvtColorTwoPlane(yPlane, uvPlane, frame, cv::COLOR_YUV2BGR_NV12);
            gst_buffer_unmap(buffer, &map);
            logFrameContentDiagnostics(frame, sampleFormat, yStride, bufferSize);
            return true;
        }

        if (sampleFormat == "I420") {
            if (width % 2 != 0 || height % 2 != 0) {
                std::ostringstream stream;
                stream << "I420 appsink sample has odd size: " << width << "x" << height << " caps=" << capsText;
                error = stream.str();
                gst_buffer_unmap(buffer, &map);
                return false;
            }
            const std::size_t yStride = planeStride(0, static_cast<std::size_t>(width));
            const std::size_t uStride = planeStride(1, static_cast<std::size_t>(width / 2));
            const std::size_t vStride = planeStride(2, static_cast<std::size_t>(width / 2));
            const std::size_t chromaWidth = static_cast<std::size_t>(width / 2);
            const std::size_t chromaHeight = static_cast<std::size_t>(height / 2);
            const std::size_t yOffset = planeOffset(0);
            const std::size_t uOffset = meta ? planeOffset(1) : yStride * static_cast<std::size_t>(height);
            const std::size_t vOffset = meta ? planeOffset(2) : uOffset + uStride * chromaHeight;
            const std::size_t expected = vOffset + vStride * chromaHeight;
            if (bufferSize < expected) {
                failBufferTooSmall(expected);
                gst_buffer_unmap(buffer, &map);
                return false;
            }
            std::vector<unsigned char> tightData(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) + 2U * chromaWidth * chromaHeight);
            unsigned char* tight = tightData.data();
            for (int row = 0; row < height; ++row) {
                std::memcpy(tight + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    map.data + yOffset + static_cast<std::size_t>(row) * yStride,
                    static_cast<std::size_t>(width));
            }
            unsigned char* uDest = tight + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            unsigned char* vDest = uDest + chromaWidth * chromaHeight;
            for (std::size_t row = 0; row < chromaHeight; ++row) {
                std::memcpy(uDest + row * chromaWidth, map.data + uOffset + row * uStride, chromaWidth);
            }
            for (std::size_t row = 0; row < chromaHeight; ++row) {
                std::memcpy(vDest + row * chromaWidth, map.data + vOffset + row * vStride, chromaWidth);
            }
            cv::Mat yuv(height + height / 2, width, CV_8UC1, tightData.data());
            cv::cvtColor(yuv, frame, cv::COLOR_YUV2BGR_I420);
            gst_buffer_unmap(buffer, &map);
            logFrameContentDiagnostics(frame, sampleFormat, yStride, bufferSize);
            return true;
        }

        if (channels == 0) {
            gst_buffer_unmap(buffer, &map);
            error = "appsink sample format is not BGR/RGB/BGRx/BGRA/RGBA/NV12/I420: ";
            error += sampleFormat;
            error += " caps=";
            error += capsText;
            return false;
        }

        const std::size_t expected = stride * static_cast<std::size_t>(height - 1)
            + static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
        if (bufferSize < expected) {
            failBufferTooSmall(expected);
            gst_buffer_unmap(buffer, &map);
            return false;
        }

        if (sampleFormat == "BGR") {
            cv::Mat wrapped(height, width, CV_8UC3, map.data, stride);
            frame = wrapped.clone();
        } else if (sampleFormat == "RGB") {
            cv::Mat wrapped(height, width, CV_8UC3, map.data, stride);
            frame.create(height, width, CV_8UC3);
            cv::cvtColor(wrapped, frame, cv::COLOR_RGB2BGR);
        } else if (sampleFormat == "RGBA") {
            cv::Mat wrapped(height, width, CV_8UC4, map.data, stride);
            cv::cvtColor(wrapped, frame, cv::COLOR_RGBA2BGR);
        } else {
            cv::Mat wrapped(height, width, CV_8UC4, map.data, stride);
            cv::cvtColor(wrapped, frame, cv::COLOR_BGRA2BGR);
        }
        gst_buffer_unmap(buffer, &map);
        logFrameContentDiagnostics(frame, sampleFormat, stride, bufferSize);
        return true;
    }

    static void logGStreamerMessage(GstMessage* message, const std::string& pipeline)
    {
        GError* error = nullptr;
        gchar* debug = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(message, &error, &debug);
            LOG_ERROR("Secondary camera GStreamer error: " << (error ? error->message : "unknown") << " pipeline=" << pipeline);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
            gst_message_parse_warning(message, &error, &debug);
            LOG_WARNING("Secondary camera GStreamer warning: " << (error ? error->message : "unknown") << " pipeline=" << pipeline);
        }
        if (debug) g_free(debug);
        if (error) g_error_free(error);
    }
#endif

    void release()
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->desiredPipeline.empty()) {
            state->desiredPipeline.clear();
            state->activePipeline.clear();
            state->lastFrame.release();
            state->hasFrame = false;
        }
    }

    void ensureWorkerStarted()
    {
        if (state->started) {
            return;
        }
        state->started = true;
        auto workerState = state;
        workerState->worker = std::thread([workerState]() {
#if defined(JON_ENABLE_GSTREAMER_APP)
            ensureGStreamerInitialized();
            GstElementPtr pipeline;
            GstElementPtr appSink;
            GstBusPtr bus;
            std::string localPipeline;
            std::string lastOpenFailure;
            std::string lastReadFailure;
            std::uint64_t frames = 0;
            auto statsStart = std::chrono::steady_clock::now();
            bool inBlackStreak = false;
            std::chrono::steady_clock::time_point firstBlackFrameAt;
            std::chrono::steady_clock::time_point lastSkippedBlackLog;
            constexpr auto blackStreakRestartThreshold = std::chrono::seconds(3);
#else
            std::string lastOpenFailure;
#endif

            while (!workerState->stop) {
                std::string wantedPipeline;
                {
                    std::lock_guard<std::mutex> lock(workerState->mutex);
                    wantedPipeline = workerState->desiredPipeline;
                }

                if (wantedPipeline.empty()) {
#if defined(JON_ENABLE_GSTREAMER_APP)
                    if (pipeline) {
                        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                    }
                    appSink.reset();
                    bus.reset();
                    pipeline.reset();
                    localPipeline.clear();
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

#if defined(JON_ENABLE_GSTREAMER_APP)
                if (!pipeline || !appSink || localPipeline != wantedPipeline) {
                    if (pipeline) {
                        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                    }
                    appSink.reset();
                    bus.reset();
                    pipeline.reset();
                    localPipeline.clear();
                    if (lastOpenFailure != wantedPipeline) {
                        LOG_INFO("Opening pause camera GStreamer pipeline");
                    }

                    GError* parseError = nullptr;
                    GstElement* rawPipeline = gst_parse_launch(wantedPipeline.c_str(), &parseError);
                    if (!rawPipeline) {
                        if (lastOpenFailure != wantedPipeline) {
                            LOG_ERROR("Cannot parse secondary camera GStreamer pipeline: "
                                << (parseError ? parseError->message : "unknown"));
                            lastOpenFailure = wantedPipeline;
                        }
                        if (parseError) g_error_free(parseError);
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }
                    if (parseError) {
                        LOG_WARNING("Secondary camera GStreamer parse warning: " << parseError->message);
                        g_error_free(parseError);
                    }

                    pipeline.reset(rawPipeline);
                    appSink.reset(findAppSink(pipeline.get()));
                    if (!appSink) {
                        if (lastOpenFailure != wantedPipeline) {
                            LOG_ERROR("Secondary camera pipeline does not contain an appsink");
                            lastOpenFailure = wantedPipeline;
                        }
                        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                        pipeline.reset();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }
                    gst_app_sink_set_emit_signals(GST_APP_SINK(appSink.get()), FALSE);
                    gst_app_sink_set_max_buffers(GST_APP_SINK(appSink.get()), 1);
                    gst_app_sink_set_drop(GST_APP_SINK(appSink.get()), TRUE);

                    bus.reset(gst_element_get_bus(pipeline.get()));
                    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
                    if (stateResult == GST_STATE_CHANGE_FAILURE) {
                        if (lastOpenFailure != wantedPipeline) {
                            LOG_ERROR("Cannot start secondary camera GStreamer pipeline");
                            lastOpenFailure = wantedPipeline;
                        }
                        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                        appSink.reset();
                        bus.reset();
                        pipeline.reset();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }

                    localPipeline = wantedPipeline;
                    lastOpenFailure.clear();
                    lastReadFailure.clear();
                    inBlackStreak = false;
                    firstBlackFrameAt = {};
                    lastSkippedBlackLog = {};
                    frames = 0;
                    statsStart = std::chrono::steady_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(workerState->mutex);
                        if (workerState->desiredPipeline == wantedPipeline) {
                            workerState->activePipeline = wantedPipeline;
                        }
                    }
                    LOG_INFO("Pause camera opened using GStreamer appsink pipeline");
                }

                if (bus) {
                    while (GstMessage* rawMessage = gst_bus_pop_filtered(bus.get(), static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS))) {
                        GstMessagePtr message(rawMessage);
                        if (GST_MESSAGE_TYPE(rawMessage) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(rawMessage) == GST_MESSAGE_WARNING) {
                            logGStreamerMessage(rawMessage, localPipeline);
                        }
                        if (GST_MESSAGE_TYPE(rawMessage) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(rawMessage) == GST_MESSAGE_EOS) {
                            gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                            appSink.reset();
                            bus.reset();
                            pipeline.reset();
                            localPipeline.clear();
                            {
                                std::lock_guard<std::mutex> lock(workerState->mutex);
                                workerState->activePipeline.clear();
                            }
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            break;
                        }
                    }
                    if (!pipeline || !appSink) {
                        continue;
                    }
                }

                GstSamplePtr sample(gst_app_sink_try_pull_sample(GST_APP_SINK(appSink.get()), 100 * GST_MSECOND));
                if (!sample) {
                    continue;
                }

                cv::Mat frame;
                std::string frameError;
                if (!sampleToBgrMat(sample.get(), frame, frameError) || frame.empty()) {
                    if (lastReadFailure != localPipeline) {
                        LOG_WARNING("Cannot read secondary camera appsink frame: " << frameError);
                        lastReadFailure = localPipeline;
                    }
                    continue;
                }

                if (isNearlyBlackFrame(frame)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!inBlackStreak) {
                        inBlackStreak = true;
                        firstBlackFrameAt = now;
                        LOG_WARNING("Secondary camera black frame detected; keeping last valid frame");
                    }
                    if (lastSkippedBlackLog.time_since_epoch().count() == 0
                        || now - lastSkippedBlackLog >= std::chrono::seconds(5)) {
                        LOG_WARNING("Ignoring black secondary camera frame; keeping last valid frame");
                        lastSkippedBlackLog = now;
                    }
                    if (now - firstBlackFrameAt >= blackStreakRestartThreshold) {
                        LOG_WARNING("Secondary camera black stream sustained >"
                            << blackStreakRestartThreshold.count()
                            << "s; restarting GStreamer pipeline");
                        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
                        appSink.reset();
                        bus.reset();
                        pipeline.reset();
                        localPipeline.clear();
                        inBlackStreak = false;
                        firstBlackFrameAt = {};
                        lastSkippedBlackLog = {};
                        frames = 0;
                        statsStart = std::chrono::steady_clock::now();
                        {
                            std::lock_guard<std::mutex> lock(workerState->mutex);
                            workerState->activePipeline.clear();
                            workerState->lastFrame.release();
                            workerState->hasFrame = false;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                    continue;
                }

                inBlackStreak = false;
                firstBlackFrameAt = {};
                lastSkippedBlackLog = {};
                lastReadFailure.clear();
                frames++;
                if (frames <= 5 || frames % 900 == 0) {
                    LOG_VERBOSE("Pause camera appsink frame received: " << frames);
                }
                {
                    std::lock_guard<std::mutex> lock(workerState->mutex);
                    if (workerState->desiredPipeline == localPipeline) {
                        workerState->lastFrame = frame;
                        workerState->hasFrame = true;
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                const double elapsedSeconds = std::chrono::duration<double>(now - statsStart).count();
                if (elapsedSeconds >= 5.0) {
                    LOG_VERBOSE("Pause camera appsink stream: fps=" << (static_cast<double>(frames) / elapsedSeconds)
                        << " frames=" << frames);
                    frames = 0;
                    statsStart = now;
                }
#else
                if (lastOpenFailure != wantedPipeline) {
                    LOG_ERROR("Secondary camera requires a build with GStreamer appsink support");
                    lastOpenFailure = wantedPipeline;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
#endif
            }
#if defined(JON_ENABLE_GSTREAMER_APP)
            if (pipeline) {
                gst_element_set_state(pipeline.get(), GST_STATE_NULL);
            }
#endif
        });
        workerState->worker.detach();
    }

    bool ensureOpened(const std::string& nextPipeline)
    {
        if (nextPipeline.empty()) {
            release();
            if (warnedOpenFailurePipeline != "<empty>") {
                LOG_ERROR("Secondary camera pipeline is empty");
                warnedOpenFailurePipeline = "<empty>";
            }
            return false;
        }
        ensureWorkerStarted();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->desiredPipeline != nextPipeline) {
                state->desiredPipeline = nextPipeline;
                state->activePipeline.clear();
                state->lastFrame.release();
                state->hasFrame = false;
            }
        }
        return true;
    }

    bool read(cv::Mat& frame, const cv::Size& fallbackSize)
    {
        (void)fallbackSize;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->hasFrame || state->lastFrame.empty()) {
            return false;
        }
        frame = state->lastFrame;
        return true;
    }
};

struct StatusFrameBuffers {
    MediaFile pauseMedia;
    PauseCameraSource pauseCamera;
    cv::Mat scaledPauseImage;
#if defined(JON_ENABLE_FREETYPE_TEXT)
    FT_Library ftLibrary = nullptr;
    FT_Face ftFace = nullptr;
    std::string ftFontPath;

    ~StatusFrameBuffers()
    {
        if (ftFace) FT_Done_Face(ftFace);
        if (ftLibrary) FT_Done_FreeType(ftLibrary);
    }
#endif
};

std::time_t fileMtime(const std::string& path)
{
    struct stat statBuffer {};
    if (::stat(path.c_str(), &statBuffer) != 0) {
        return 0;
    }
    return statBuffer.st_mtime;
}

int pauseImageFontFace(const std::string& font)
{
    if (font == "plain") return cv::FONT_HERSHEY_PLAIN;
    if (font == "duplex") return cv::FONT_HERSHEY_DUPLEX;
    if (font == "complex") return cv::FONT_HERSHEY_COMPLEX;
    if (font == "triplex") return cv::FONT_HERSHEY_TRIPLEX;
    if (font == "complex-small") return cv::FONT_HERSHEY_COMPLEX_SMALL;
    if (font == "script-simplex") return cv::FONT_HERSHEY_SCRIPT_SIMPLEX;
    if (font == "script-complex") return cv::FONT_HERSHEY_SCRIPT_COMPLEX;
    return cv::FONT_HERSHEY_SIMPLEX;
}

bool isBuiltInPauseFont(const std::string& font)
{
    return font == "plain" || font == "simplex" || font == "duplex"
        || font == "complex" || font == "triplex" || font == "complex-small"
        || font == "script-simplex" || font == "script-complex";
}

int alignedTextX(int anchorX, int textWidth, PauseFontAlign align)
{
    if (align == PauseFontAlign::Center) return anchorX - textWidth / 2;
    if (align == PauseFontAlign::Right) return anchorX - textWidth;
    return anchorX;
}

void renderHersheyLine(
    cv::Mat& overlay,
    const std::string& text,
    const cv::Point& anchor,
    int fontFace,
    double scale,
    int thickness,
    const cv::Scalar& color,
    PauseFontAlign align)
{
    int baseline = 0;
    const cv::Size textSize = cv::getTextSize(text, fontFace, scale, thickness, &baseline);
    cv::putText(overlay, text, cv::Point(alignedTextX(anchor.x, textSize.width, align), anchor.y),
        fontFace, scale, color, thickness, cv::LINE_AA);
}

#if defined(JON_ENABLE_FREETYPE_TEXT)
bool ensureFreeTypeFace(StatusFrameBuffers& buffers, const std::string& fontPath)
{
    if (buffers.ftFace && buffers.ftFontPath == fontPath) return true;
    if (buffers.ftFace) {
        FT_Done_Face(buffers.ftFace);
        buffers.ftFace = nullptr;
    }
    if (!buffers.ftLibrary && FT_Init_FreeType(&buffers.ftLibrary) != 0) {
        LOG_WARNING("Cannot initialize FreeType text renderer");
        return false;
    }
    if (FT_New_Face(buffers.ftLibrary, fontPath.c_str(), 0, &buffers.ftFace) != 0) {
        LOG_WARNING("Cannot load pause TTF font: " << fontPath);
        return false;
    }
    buffers.ftFontPath = fontPath;
    LOG_INFO("Loaded pause TTF font: " << fontPath);
    return true;
}

int measureFreeTypeLine(FT_Face face, const std::string& text, int pixelSize)
{
    if (!face || FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0) return 0;
    int width = 0;
    for (unsigned char c : text) {
        if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) == 0) {
            width += static_cast<int>(face->glyph->advance.x >> 6);
        }
    }
    return width;
}

void blendGlyph(cv::Mat& frame, FT_Bitmap& bitmap, int left, int top, const RgbaColor& color)
{
    const double textAlpha = std::clamp(color.a, 0, 255) / 255.0;
    for (int row = 0; row < static_cast<int>(bitmap.rows); ++row) {
        const int y = top + row;
        if (y < 0 || y >= frame.rows) continue;
        const unsigned char* src = bitmap.buffer + row * bitmap.pitch;
        for (int col = 0; col < static_cast<int>(bitmap.width); ++col) {
            const int x = left + col;
            if (x < 0 || x >= frame.cols) continue;
            const double alpha = (static_cast<double>(src[col]) / 255.0) * textAlpha;
            cv::Vec3b& dst = frame.at<cv::Vec3b>(y, x);
            dst[0] = static_cast<unsigned char>(dst[0] * (1.0 - alpha) + std::clamp(color.b, 0, 255) * alpha);
            dst[1] = static_cast<unsigned char>(dst[1] * (1.0 - alpha) + std::clamp(color.g, 0, 255) * alpha);
            dst[2] = static_cast<unsigned char>(dst[2] * (1.0 - alpha) + std::clamp(color.r, 0, 255) * alpha);
        }
    }
}

void renderFreeTypeLine(
    cv::Mat& frame,
    FT_Face face,
    const std::string& text,
    const cv::Point& anchor,
    int pixelSize,
    const RgbaColor& color,
    PauseFontAlign align)
{
    if (!face || FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0) return;
    int penX = alignedTextX(anchor.x, measureFreeTypeLine(face, text, pixelSize), align);
    for (unsigned char c : text) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot glyph = face->glyph;
        blendGlyph(frame, glyph->bitmap, penX + glyph->bitmap_left, anchor.y - glyph->bitmap_top, color);
        penX += static_cast<int>(glyph->advance.x >> 6);
    }
}
#endif

void renderPauseStatusText(cv::Mat& frame, const std::string& status, const ProcessorConfig& config, StatusFrameBuffers& buffers, const cv::Point& textPosition)
{
    const double textSize = std::clamp(config.pauseImageTextSize, 0.1, 10.0);
    const int lineOffset = static_cast<int>(std::round(52.0 * textSize / 1.6));

    if (!isBuiltInPauseFont(config.pauseImageFont)) {
#if defined(JON_ENABLE_FREETYPE_TEXT)
        const std::string fontPath = mediaPath(config.pauseImageFontDirectory, config.pauseImageFont + ".ttf");
        if (ensureFreeTypeFace(buffers, fontPath)) {
            const int pixelSize = std::max(6, static_cast<int>(std::round(38.0 * textSize / 1.6)));
            renderFreeTypeLine(frame, buffers.ftFace, status, textPosition, pixelSize, config.pauseImageTextColor, config.pauseImageFontAlign);
            renderFreeTypeLine(frame, buffers.ftFace, "JONImageProcessor", cv::Point(textPosition.x, textPosition.y + lineOffset),
                std::max(6, static_cast<int>(std::round(pixelSize * 0.5))), config.pauseImageTextColor, config.pauseImageFontAlign);
            return;
        }
#else
        static std::string lastWarnedFont;
        if (lastWarnedFont != config.pauseImageFont) {
            LOG_WARNING("Pause TTF font requested but this build has no FreeType support: " << config.pauseImageFont);
            lastWarnedFont = config.pauseImageFont;
        }
#endif
    }

    cv::Mat textOverlay = frame.clone();
    const cv::Scalar textColor(
        std::clamp(config.pauseImageTextColor.b, 0, 255),
        std::clamp(config.pauseImageTextColor.g, 0, 255),
        std::clamp(config.pauseImageTextColor.r, 0, 255));
    const int fontFace = pauseImageFontFace(config.pauseImageFont);
    renderHersheyLine(textOverlay, status, textPosition, fontFace, textSize,
        std::max(1, static_cast<int>(std::round(4.0 * textSize / 1.6))), textColor, config.pauseImageFontAlign);
    renderHersheyLine(textOverlay, "JONImageProcessor", cv::Point(textPosition.x, textPosition.y + lineOffset), fontFace, textSize * 0.5,
        std::max(1, static_cast<int>(std::round(2.0 * textSize / 1.6))), textColor, config.pauseImageFontAlign);
    const double alpha = std::clamp(config.pauseImageTextColor.a, 0, 255) / 255.0;
    cv::addWeighted(textOverlay, alpha, frame, 1.0 - alpha, 0.0, frame);
}

cv::Mat letterboxPauseCameraFrame(const cv::Mat& frame, const cv::Size& targetSize)
{
    if (frame.empty() || targetSize.width <= 0 || targetSize.height <= 0) {
        return frame.clone();
    }
    if (frame.size() == targetSize) {
        return frame.clone();
    }

    const double scale = std::min(
        static_cast<double>(targetSize.width) / static_cast<double>(frame.cols),
        static_cast<double>(targetSize.height) / static_cast<double>(frame.rows));
    const int scaledWidth = std::max(1, static_cast<int>(std::round(static_cast<double>(frame.cols) * scale)));
    const int scaledHeight = std::max(1, static_cast<int>(std::round(static_cast<double>(frame.rows) * scale)));

    cv::Mat scaled;
    cv::resize(frame, scaled, cv::Size(scaledWidth, scaledHeight), 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat output(targetSize, frame.type(), cv::Scalar(0, 0, 0));
    const int offsetX = (targetSize.width - scaledWidth) / 2;
    const int offsetY = (targetSize.height - scaledHeight) / 2;
    scaled.copyTo(output(cv::Rect(offsetX, offsetY, scaledWidth, scaledHeight)));
    return output;
}

cv::Mat makeStatusFrame(
    const cv::Size& size,
    const std::string& status,
    const ProcessorConfig* config = nullptr,
    StatusFrameBuffers* buffers = nullptr)
{
    auto logGeneratedStatusReason = [](const std::string& reason) {
        static std::mutex mutex;
        static std::map<std::string, std::chrono::steady_clock::time_point> lastLogTimes;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        auto it = lastLogTimes.find(reason);
        if (it == lastLogTimes.end() || now - it->second >= std::chrono::seconds(30)) {
            LOG_INFO("Using generated camera status image: " << reason);
            lastLogTimes[reason] = now;
        }
    };

    if (config != nullptr && buffers != nullptr && config->pauseImageEnabled && config->pauseSource == PauseSource::Camera) {
        buffers->pauseMedia.release();
        cv::Mat pauseFrame;
        const cv::Size captureSize(config->width, config->height);
        if (buffers->pauseCamera.ensureOpened(config->secondaryCameraPipeline)
            && buffers->pauseCamera.read(pauseFrame, captureSize) && !pauseFrame.empty()) {
            cv::Mat frame;
            if (pauseFrame.size() == size) {
                static std::mutex preserveNoticeMutex;
                static bool preserveNoticeLogged = false;
                if (config->pausePreserveAspectRatio) {
                    std::lock_guard<std::mutex> lock(preserveNoticeMutex);
                    if (!preserveNoticeLogged) {
                        LOG_INFO("Pause preserveAspectRatio has no visible effect because secondary camera frames already match output size: "
                            << pauseFrame.cols << "x" << pauseFrame.rows);
                        preserveNoticeLogged = true;
                    }
                }
                frame = pauseFrame.clone();
            } else {
                // AirPlay/UxPlay may deliver device-shaped frames. Always letterbox here so the appsink reader stays a pure pixel-format converter.
                frame = letterboxPauseCameraFrame(pauseFrame, size);
            }
            if (config->pauseImageShowStatusText) {
                const int marginX = size.width / 8;
                const cv::Point textPosition(
                    config->pauseImageTextPosition.x >= 0 ? config->pauseImageTextPosition.x : marginX + 32,
                    config->pauseImageTextPosition.y >= 0 ? config->pauseImageTextPosition.y : size.height / 2 - 10);
                renderPauseStatusText(frame, status, *config, *buffers, textPosition);
            }
            return frame;
        }
        buffers->scaledPauseImage.release();
        logGeneratedStatusReason("pause camera unavailable");
        return makeStatusFrame(size, "Camera 2. DISCONNECTED");
    } else if (config != nullptr && buffers != nullptr && config->pauseImageEnabled && !config->pauseImagePath.empty()) {
        buffers->pauseCamera.release();
        const std::string resolvedPath = mediaPath(config->pauseImageFolder, config->pauseImagePath);
        if (!buffers->pauseMedia.ensureLoaded(resolvedPath, config->pauseLoopIfVideo, size, "pause")) {
            buffers->scaledPauseImage.release();
        } else {
            cv::Mat pauseImage;
            if (!buffers->pauseMedia.read(pauseImage) || pauseImage.empty()) {
                const bool shouldWarn = !buffers->pauseMedia.isHtml || !buffers->pauseMedia.warnedFrameFailure;
                if (shouldWarn) {
                    LOG_WARNING("Cannot read pause media frame: " << resolvedPath);
                }
                buffers->pauseMedia.warnedFrameFailure = true;
                if (!buffers->scaledPauseImage.empty()) {
                    cv::Mat frame = buffers->scaledPauseImage.clone();
                    return frame;
                }
            } else {
                buffers->pauseMedia.warnedFrameFailure = false;
                cv::Mat frame;
                if (pauseImage.size() == size) {
                    buffers->scaledPauseImage = pauseImage.clone();
                } else {
                    cv::resize(pauseImage, buffers->scaledPauseImage, size, 0.0, 0.0, cv::INTER_LINEAR);
                }
                frame = buffers->scaledPauseImage.clone();
                if (config->pauseImageShowStatusText) {
                    const int marginX = size.width / 8;
                    const cv::Point textPosition(
                        config->pauseImageTextPosition.x >= 0 ? config->pauseImageTextPosition.x : marginX + 32,
                        config->pauseImageTextPosition.y >= 0 ? config->pauseImageTextPosition.y : size.height / 2 - 10);
                    renderPauseStatusText(frame, status, *config, *buffers, textPosition);
                }
                return frame;
            }
        }
    } else if (config != nullptr && !config->pauseImageEnabled) {
        if (buffers != nullptr) {
            buffers->pauseCamera.release();
        }
        logGeneratedStatusReason("pause.enabled=false");
    } else if (config != nullptr && config->pauseImagePath.empty()) {
        if (buffers != nullptr) {
            buffers->pauseCamera.release();
        }
        logGeneratedStatusReason("pause.image is empty");
    } else {
        if (buffers != nullptr) {
            buffers->pauseCamera.release();
        }
        logGeneratedStatusReason("pause media config unavailable");
    }

    cv::Mat frame(size, CV_8UC3, cv::Scalar(128, 128, 128));

    const int grid = std::max(40, size.width / 32);
    for (int x = 0; x < size.width; x += grid) {
        cv::line(frame, cv::Point(x, 0), cv::Point(x, size.height), cv::Scalar(220, 220, 220), 1);
    }
    for (int y = 0; y < size.height; y += grid) {
        cv::line(frame, cv::Point(0, y), cv::Point(size.width, y), cv::Scalar(220, 220, 220), 1);
    }

    cv::ellipse(frame, cv::Point(size.width / 2, size.height / 2), cv::Size(size.width / 3, size.height / 2), 0.0, 0.0, 360.0, cv::Scalar(230, 230, 230), 2);
    cv::line(frame, cv::Point(size.width / 2, 0), cv::Point(size.width / 2, size.height), cv::Scalar(230, 230, 230), 1);
    cv::line(frame, cv::Point(0, size.height / 2), cv::Point(size.width, size.height / 2), cv::Scalar(230, 230, 230), 1);

    const int marginX = size.width / 8;
    const int top = size.height / 6;
    const int barHeight = std::max(60, size.height / 7);
    const std::array<cv::Scalar, 8> bars {
        cv::Scalar(255, 255, 255),
        cv::Scalar(0, 255, 255),
        cv::Scalar(255, 255, 0),
        cv::Scalar(0, 255, 0),
        cv::Scalar(255, 0, 255),
        cv::Scalar(0, 0, 255),
        cv::Scalar(255, 0, 0),
        cv::Scalar(0, 0, 0),
    };
    const int barWidth = (size.width - 2 * marginX) / static_cast<int>(bars.size());
    for (std::size_t i = 0; i < bars.size(); ++i) {
        cv::rectangle(frame, cv::Rect(marginX + static_cast<int>(i) * barWidth, top, barWidth, barHeight), bars[i], cv::FILLED);
    }

    const int grayTop = top + barHeight;
    const int graySteps = 24;
    const int grayWidth = (size.width - 2 * marginX) / graySteps;
    for (int i = 0; i < graySteps; ++i) {
        const int value = static_cast<int>((255.0 * i) / std::max(1, graySteps - 1));
        cv::rectangle(frame, cv::Rect(marginX + i * grayWidth, grayTop, grayWidth, barHeight / 2), cv::Scalar(value, value, value), cv::FILLED);
    }

    const int stripeTop = grayTop + barHeight / 2;
    for (int x = marginX; x < size.width - marginX; x += 6) {
        const bool dark = ((x - marginX) / 6) % 2 == 0;
        cv::line(frame, cv::Point(x, stripeTop), cv::Point(x, stripeTop + barHeight / 2), dark ? cv::Scalar(0, 0, 0) : cv::Scalar(255, 255, 255), 3);
    }

    const int rampTop = size.height - top - barHeight;
    for (int y = 0; y < barHeight; ++y) {
        const double fade = static_cast<double>(y) / std::max(1, barHeight - 1);
        for (int x = marginX; x < size.width - marginX; ++x) {
            const double level = static_cast<double>(x - marginX) / std::max(1, size.width - 2 * marginX - 1);
            frame.at<cv::Vec3b>(rampTop + y, x) = cv::Vec3b(
                static_cast<unsigned char>(255.0 * (1.0 - level) * (0.35 + 0.65 * fade)),
                static_cast<unsigned char>(255.0 * level * (0.35 + 0.65 * fade)),
                static_cast<unsigned char>(255.0 * (0.15 + 0.85 * level)));
        }
    }

    cv::rectangle(frame, cv::Rect(marginX, size.height / 2 - 70, size.width - 2 * marginX, 140), cv::Scalar(245, 245, 245), cv::FILLED);
    cv::putText(frame, status, cv::Point(marginX + 32, size.height / 2 - 10), cv::FONT_HERSHEY_SIMPLEX, 1.6, cv::Scalar(20, 20, 20), 4, cv::LINE_AA);
    cv::putText(frame, "JONImageProcessor generated test pattern", cv::Point(marginX + 34, size.height / 2 + 42), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(60, 60, 60), 2, cv::LINE_AA);
    return frame;
}

bool pathExists(const std::string& path)
{
    std::ifstream file(path);
    return static_cast<bool>(file);
}

cv::Mat applyBackgroundOverlay(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    const RgbColor& color,
    double alpha)
{
    if (frame.empty() || personMask.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        cv::Mat normalizedPersonMask;
        mask.convertTo(normalizedPersonMask, CV_32FC1, 1.0 / 255.0);

        cv::Mat backgroundAlpha = (1.0 - normalizedPersonMask) * alpha;
        std::vector<cv::Mat> alphaChannels(frame.channels(), backgroundAlpha);
        cv::Mat backgroundAlpha3;
        cv::merge(alphaChannels, backgroundAlpha3);

        cv::Mat frameFloat;
        frame.convertTo(frameFloat, CV_32FC3);

        cv::Mat colorOverlay(frame.size(), frame.type(), cv::Scalar(color.b, color.g, color.r));
        cv::Mat colorFloat;
        colorOverlay.convertTo(colorFloat, CV_32FC3);

        cv::Mat inverseAlpha3 = cv::Scalar::all(1.0) - backgroundAlpha3;
        cv::Mat resultFloat = frameFloat.mul(inverseAlpha3) + colorFloat.mul(backgroundAlpha3);
        cv::Mat result;
        resultFloat.convertTo(result, frame.type());
        return result;
    }

    const int alphaScale = std::clamp(static_cast<int>(std::round(alpha * 255.0)), 0, 255);
    const int overlayB = std::clamp(color.b, 0, 255);
    const int overlayG = std::clamp(color.g, 0, 255);
    const int overlayR = std::clamp(color.r, 0, 255);

    std::array<unsigned char, 256> overlayAlphaByMask {};
    std::array<unsigned char, 256> sourceAlphaByMask {};
    for (int maskValue = 0; maskValue < 256; ++maskValue) {
        const int overlayAlpha = ((255 - maskValue) * alphaScale + 127) / 255;
        overlayAlphaByMask[maskValue] = static_cast<unsigned char>(overlayAlpha);
        sourceAlphaByMask[maskValue] = static_cast<unsigned char>(255 - overlayAlpha);
    }

    cv::Mat result(frame.size(), frame.type());
    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int overlayAlpha = overlayAlphaByMask[maskRow[x]];
                const int sourceAlpha = sourceAlphaByMask[maskRow[x]];
                destination[x][0] = static_cast<unsigned char>((source[x][0] * sourceAlpha + overlayB * overlayAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * sourceAlpha + overlayG * overlayAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * sourceAlpha + overlayR * overlayAlpha + 127) / 255);
            }
        }
    });

    return result;
}

int blurKernelSize(int blurStrength)
{
    const int radius = std::clamp(blurStrength, 1, 100);
    return radius * 2 + 1;
}

int blurDownscaleFactor(int blurStrength)
{
    if (blurStrength >= 32) {
        return 4;
    }
    if (blurStrength >= 12) {
        return 2;
    }
    return 1;
}

cv::Mat applyBackgroundBlur(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    int blurStrength,
    BackgroundEffectBuffers& buffers)
{
    if (frame.empty() || personMask.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        return frame;
    }

    const int downscaleFactor = blurDownscaleFactor(blurStrength);
    if (downscaleFactor > 1) {
        const cv::Size downscaledSize(
            std::max(1, frame.cols / downscaleFactor),
            std::max(1, frame.rows / downscaleFactor));
        cv::resize(frame, buffers.downscaledFrame, downscaledSize, 0.0, 0.0, cv::INTER_AREA);

        const int downscaledKernel = blurKernelSize(std::max(1, blurStrength / downscaleFactor));
        cv::GaussianBlur(
            buffers.downscaledFrame,
            buffers.downscaledBlurredFrame,
            cv::Size(downscaledKernel, downscaledKernel),
            0.0);
        cv::resize(buffers.downscaledBlurredFrame, buffers.blurredFrame, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
    } else {
        const int kernelSize = blurKernelSize(blurStrength);
        cv::GaussianBlur(frame, buffers.blurredFrame, cv::Size(kernelSize, kernelSize), 0.0);
    }

    buffers.result.create(frame.size(), frame.type());

    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* blurred = buffers.blurredFrame.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = buffers.result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int personAlpha = maskRow[x];
                const int backgroundAlpha = 255 - personAlpha;
                destination[x][0] = static_cast<unsigned char>((source[x][0] * personAlpha + blurred[x][0] * backgroundAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * personAlpha + blurred[x][1] * backgroundAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * personAlpha + blurred[x][2] * backgroundAlpha + 127) / 255);
            }
        }
    });

    return buffers.result;
}

cv::Mat applyBackgroundImage(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    const cv::Mat& backgroundImage,
    BackgroundEffectBuffers& buffers)
{
    if (frame.empty() || personMask.empty() || backgroundImage.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        return frame;
    }

    cv::resize(backgroundImage, buffers.scaledBackgroundImage, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);

    buffers.result.create(frame.size(), frame.type());

    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* background = buffers.scaledBackgroundImage.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = buffers.result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int personAlpha = maskRow[x];
                const int backgroundAlpha = 255 - personAlpha;
                destination[x][0] = static_cast<unsigned char>((source[x][0] * personAlpha + background[x][0] * backgroundAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * personAlpha + background[x][1] * backgroundAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * personAlpha + background[x][2] * backgroundAlpha + 127) / 255);
            }
        }
    });

    return buffers.result;
}

cv::Mat applyMaskPostprocessing(
    const cv::Mat& mask,
    cv::Mat& previousMask,
    const ProcessorConfig& config)
{
    if (mask.empty()) {
        previousMask.release();
        return mask;
    }

    cv::Mat processed;
    if (mask.type() == CV_8UC1) {
        processed = mask.clone();
    } else {
        cv::cvtColor(mask, processed, cv::COLOR_BGR2GRAY);
    }

    const double highThreshold = std::clamp(config.maskThreshold, 0.0, 1.0) * 255.0;
    const double lowThreshold = std::max(0.0, (std::clamp(config.maskThreshold, 0.0, 1.0) - 0.15) * 255.0);

    cv::Mat strongForeground;
    cv::threshold(processed, strongForeground, highThreshold, 255, cv::THRESH_BINARY);

    cv::Mat foreground = strongForeground;
    if (!previousMask.empty() && previousMask.size() == processed.size()) {
        cv::Mat previousForeground;
        cv::Mat weakForeground;
        cv::threshold(previousMask, previousForeground, 127, 255, cv::THRESH_BINARY);
        cv::threshold(processed, weakForeground, lowThreshold, 255, cv::THRESH_BINARY);
        cv::bitwise_and(previousForeground, weakForeground, weakForeground);
        cv::bitwise_or(strongForeground, weakForeground, foreground);
    }

    cv::Mat alpha = cv::Mat::zeros(processed.size(), CV_8UC1);
    processed.copyTo(alpha, foreground);

    if (config.maskMorphology != MaskMorphologyMode::Off) {
        const int closeSize = config.maskMorphology == MaskMorphologyMode::Strong ? 15 : 7;
        const int dilateSize = config.maskMorphology == MaskMorphologyMode::Strong ? 21 : 9;
        const int blurSize = config.maskMorphology == MaskMorphologyMode::Strong ? 21 : 9;

        const cv::Mat closeKernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(closeSize, closeSize));
        cv::morphologyEx(foreground, foreground, cv::MORPH_CLOSE, closeKernel);

        const cv::Mat dilateKernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(dilateSize, dilateSize));
        cv::dilate(foreground, foreground, dilateKernel);

        alpha.setTo(255, foreground);
        cv::GaussianBlur(alpha, alpha, cv::Size(blurSize, blurSize), 0.0);
    }
    processed = alpha;

    if (config.maskSmoothing > 0.0 && !previousMask.empty() && previousMask.size() == processed.size()) {
        cv::Mat smoothed;
        cv::addWeighted(
            previousMask,
            config.maskSmoothing,
            processed,
            1.0 - config.maskSmoothing,
            0.0,
            smoothed);
        processed = smoothed;
    }

    previousMask = processed.clone();
    return processed;
}

cv::Size fitSizePreservingAspect(cv::Size sourceSize, cv::Size bounds)
{
    if (sourceSize.width <= 0 || sourceSize.height <= 0 || bounds.width <= 0 || bounds.height <= 0) {
        return bounds;
    }

    const double sourceAspect = static_cast<double>(sourceSize.width) / static_cast<double>(sourceSize.height);
    const double boundsAspect = static_cast<double>(bounds.width) / static_cast<double>(bounds.height);

    if (sourceAspect > boundsAspect) {
        const int height = std::max(1, static_cast<int>(std::round(bounds.width / sourceAspect)));
        return cv::Size(bounds.width, height);
    }

    const int width = std::max(1, static_cast<int>(std::round(bounds.height * sourceAspect)));
    return cv::Size(width, bounds.height);
}

std::string sizeToString(cv::Size size)
{
    std::ostringstream stream;
    stream << size.width << "x" << size.height;
    return stream.str();
}

std::string screenInfoToString(const ScreenInfo& screenInfo)
{
    if (!screenInfo.available) {
        return "unknown";
    }

    return sizeToString(screenInfo.size);
}

std::string operatingSystemString()
{
    utsname info {};
    if (uname(&info) != 0) {
        return "unknown";
    }

    std::ostringstream stream;
    stream << info.sysname << ' ' << info.release << ' ' << info.machine;
    return stream.str();
}

std::size_t effectiveDroppedFrames(const LowLatencyCaptureStats& stats, std::size_t processedFrames)
{
    const std::size_t unprocessedFrames = stats.capturedFrames > processedFrames
        ? stats.capturedFrames - processedFrames
        : 0;
    return std::max(stats.droppedFrames, unprocessedFrames);
}

void logStartupInfo(const ProcessorConfig& config, const ScreenInfo& screenInfo)
{
    const std::string inputSource = !config.inputPath.empty()
        ? config.inputPath
        : config.devicePath;

    LOG_INFO("JONImageProcessor starting");
    if (jonImageProcessorHasReleaseVersion()) {
        LOG_INFO("Release version: " << JON_IMAGE_PROCESSOR_RELEASE_VERSION);
    } else {
        LOG_VERBOSE("Program version: " << jonImageProcessorReleaseVersionOrUnreleased());
        LOG_INFO("Git version: " << JON_IMAGE_PROCESSOR_GIT_VERSION);
    }
    LOG_INFO("Build date: " << __DATE__ << " " << __TIME__ << " on " << JON_IMAGE_PROCESSOR_BUILD_HOST);
    LOG_INFO("Operating system: " << operatingSystemString());
    LOG_INFO("OpenCV version: " << CV_VERSION);
    LOG_VERBOSE("Primary screen size: " << screenInfoToString(screenInfo));
    LOG_VERBOSE("Input source: " << inputSource);
    LOG_VERBOSE("Display mode: " << displayModeToString(DisplayMode::Fill));
    LOG_VERBOSE("Display backend: " << displayBackendToString(config.displayBackend));
    LOG_VERBOSE("Processing size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Mask backend: tensorrt");
    LOG_VERBOSE("Segmentation size: " << config.segmentationWidth << "x" << config.segmentationHeight);
    LOG_VERBOSE("Mask model: " << (config.maskModelPath.empty() ? "none" : config.maskModelPath));
    LOG_VERBOSE("Mask threshold: " << config.maskThreshold);
    LOG_VERBOSE("Mask smoothing: " << config.maskSmoothing);
    LOG_VERBOSE("Mask morphology: " << maskMorphologyModeToString(config.maskMorphology));
    LOG_VERBOSE("Background effect: " << backgroundEffectToString(config.backgroundEffect));
    LOG_VERBOSE("Background image: " << (config.backgroundImagePath.empty() ? "none" : config.backgroundImagePath));
    LOG_VERBOSE("Background image folder: " << config.backgroundImageFolder);
    LOG_VERBOSE("Background loop if video: " << (config.backgroundLoopIfVideo ? "true" : "false"));
    LOG_VERBOSE("Pause image enabled: " << (config.pauseImageEnabled ? "true" : "false"));
    LOG_VERBOSE("Pause source: " << pauseSourceToString(config.pauseSource));
    LOG_VERBOSE("Secondary camera pipeline: " << (config.secondaryCameraPipeline.empty() ? "none" : config.secondaryCameraPipeline));
    LOG_VERBOSE("Pause image: " << (config.pauseImagePath.empty() ? "none" : config.pauseImagePath));
    LOG_VERBOSE("Pause image folder: " << config.pauseImageFolder);
    LOG_VERBOSE("Pause loop if video: " << (config.pauseLoopIfVideo ? "true" : "false"));
    LOG_VERBOSE("Pause image status text: " << (config.pauseImageShowStatusText ? "true" : "false"));
    LOG_VERBOSE("Pause image text color: "
        << config.pauseImageTextColor.r << ","
        << config.pauseImageTextColor.g << ","
        << config.pauseImageTextColor.b << ","
        << config.pauseImageTextColor.a);
    LOG_VERBOSE("Pause image text position: "
        << (config.pauseImageTextPosition.x < 0 || config.pauseImageTextPosition.y < 0
            ? std::string("auto")
            : std::to_string(config.pauseImageTextPosition.x) + "x" + std::to_string(config.pauseImageTextPosition.y)));
    LOG_VERBOSE("Pause image text size: " << config.pauseImageTextSize);
    LOG_VERBOSE("Pause image font: " << config.pauseImageFont);
    LOG_VERBOSE("Pause image font directory: " << config.pauseImageFontDirectory);
    LOG_VERBOSE("Pause image font align: " << pauseFontAlignToString(config.pauseImageFontAlign));
    LOG_VERBOSE("Pause preserve aspect ratio: " << (config.pausePreserveAspectRatio ? "true" : "false"));
    LOG_VERBOSE("Background overlay color: "
        << config.backgroundOverlayColor.r << ","
        << config.backgroundOverlayColor.g << ","
        << config.backgroundOverlayColor.b);
    LOG_VERBOSE("Background overlay alpha: " << config.backgroundOverlayAlpha);
    LOG_VERBOSE("Blur strength: " << config.blurStrength);
    LOG_VERBOSE("Camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Camera connect timeout: " << config.cameraConnectTimeoutSeconds << "s");
    LOG_VERBOSE("Fullscreen: " << (config.fullscreen ? "true" : "false"));
    LOG_VERBOSE("Daemon mode: " << (config.noDaemon ? "false" : "true"));
    LOG_VERBOSE("Benchmark: " << (config.benchmark ? "true" : "false"));
    LOG_VERBOSE("No display: " << (config.noDisplay ? "true" : "false"));
    LOG_VERBOSE("No mask: " << (config.noMask ? "true" : "false"));
    LOG_VERBOSE("No overlay: " << (config.noOverlay ? "true" : "false"));
    if (config.outputWidth > 0 && config.outputHeight > 0) {
        LOG_VERBOSE("Output canvas override: " << config.outputWidth << "x" << config.outputHeight);
    } else {
        LOG_VERBOSE("Output canvas override: auto");
    }
}

void logPerformance(
    std::size_t processedFrames,
    std::size_t intervalFrames,
    std::chrono::steady_clock::time_point startedAt,
    std::chrono::steady_clock::time_point intervalStartedAt,
    std::chrono::steady_clock::time_point now)
{
    const std::chrono::duration<double> intervalDuration = now - intervalStartedAt;
    const std::chrono::duration<double> totalDuration = now - startedAt;
    if (intervalDuration.count() <= 0.0 || totalDuration.count() <= 0.0) {
        return;
    }

    const double currentFps = static_cast<double>(intervalFrames) / intervalDuration.count();
    const double averageFps = static_cast<double>(processedFrames) / totalDuration.count();
    LOG_VERBOSE("FPS: " << currentFps << " avg=" << averageFps << " frames=" << processedFrames);
}

} // namespace

VideoProcessor::VideoProcessor(ProcessorConfig config)
    : config_(std::move(config))
{
}

int VideoProcessor::run()
{
    const ScreenInfo screenInfo = detectPrimaryScreen();
    logStartupInfo(config_, screenInfo);
    RuntimeState runtimeState(config_);
    IPCServer ipcServer(runtimeState, config_.ipcSocketPath);
    if (!ipcServer.start()) {
        return ExitRuntimeError;
    }
    LOG_INFO("Display backend: " << displayBackendToString(config_.displayBackend));
    const bool usingCamera = config_.inputPath.empty();
    LOG_INFO("Capture backend: " << (usingCamera ? "v4l2" : "opencv-file"));
    const bool lowLatencyMode = usingCamera;
    LOG_INFO("Low latency mode: " << (lowLatencyMode ? "enabled" : "disabled"));

    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    const cv::Size outputSize(config_.width, config_.height);
    const bool hasExplicitDisplaySize = config_.outputWidth > 0 && config_.outputHeight > 0;
    const bool useScreenDisplaySize = config_.fullscreen && screenInfo.available && !hasExplicitDisplaySize;
    const cv::Size configuredDisplaySize = hasExplicitDisplaySize
        ? cv::Size(config_.outputWidth, config_.outputHeight)
        : (useScreenDisplaySize ? screenInfo.size : outputSize);
    const bool forceConfiguredDisplaySize = hasExplicitDisplaySize || useScreenDisplaySize;
    const bool showWindow = !config_.noDisplay;
    const bool maskEnabled = !config_.noMask;
    const bool overlayEnabled = !config_.noOverlay && maskEnabled;

    std::unique_ptr<ICaptureBackend> captureBackend = CaptureBackendFactory::create(config_);
    if (!captureBackend) {
        LOG_ERROR("Cannot create capture backend");
        return ExitRuntimeError;
    }

    std::unique_ptr<IMaskBackend> maskBackend;
    if (maskEnabled) {
        maskBackend = std::make_unique<TensorRtMaskBackend>();

        if (!maskBackend->initialize(config_)) {
            LOG_ERROR("Cannot initialize mask backend: tensorrt");
            return ExitRuntimeError;
        }
        LOG_INFO("Mask backend: " << maskBackend->name());
    } else {
        LOG_INFO("Mask backend: none");
    }

    std::unique_ptr<IDisplayBackend> displayBackend;
    DisplayBackendConfig displayConfig;
    displayConfig.displayMode = DisplayMode::Fill;
    displayConfig.processingSize = outputSize;
    displayConfig.canvasFallbackSize = configuredDisplaySize;
    displayConfig.screenInfo = screenInfo;
    displayConfig.fullscreen = config_.fullscreen;
    displayConfig.forceCanvasFallbackSize = forceConfiguredDisplaySize;
    displayConfig.useScreenCanvasFallback = useScreenDisplaySize;
    bool displayReady = !showWindow;
    if (showWindow) {
        displayBackend = DisplayBackendFactory::create(config_.displayBackend);
        if (!displayBackend) {
            LOG_ERROR("Cannot create display backend: " << displayBackendToString(config_.displayBackend));
            return ExitRuntimeError;
        }
        displayReady = displayBackend->initialize(displayConfig);
        if (!displayReady) {
            LOG_WARNING("Display backend is not available, waiting for display reconnect: "
                << displayBackendToString(config_.displayBackend));
            displayBackend->shutdown();
        }
    }

    auto openCapture = [&]() {
        if (usingCamera && !pathExists(config_.devicePath)) {
            return false;
        }
        return captureBackend->open(config_);
    };

    bool captureOpened = displayReady ? openCapture() : false;
    if (!captureOpened && !usingCamera && displayReady) {
        return ExitRuntimeError;
    }
    if (!captureOpened && usingCamera && displayReady) {
        LOG_WARNING("Camera is not available, showing disconnected test image");
    }

    std::size_t frameIndex = 0;
    cv::Mat frame;
    BenchmarkRecorder benchmark(config_.benchmark, config_.verbose);
    LowLatencyFrameCapture lowLatencyCapture;
    bool captureActive = lowLatencyMode && config_.cameraEnabled && captureOpened;
    if (captureActive) {
        lowLatencyCapture.start(*captureBackend);
    }
    auto nextReconnectAttempt = std::chrono::steady_clock::now();
    auto nextDisplayReconnectAttempt = std::chrono::steady_clock::now() + DisplayReconnectInterval;
    std::chrono::steady_clock::time_point cameraDevicePresentSince {};
    std::chrono::steady_clock::time_point cameraEnableStartedAt {};
    bool cameraWasDisabledByRuntime = !config_.cameraEnabled;
    bool cameraRuntimePaused = !config_.cameraEnabled;
    const auto startedAt = std::chrono::steady_clock::now();
    auto intervalStartedAt = startedAt;
    std::size_t intervalFrames = 0;
    cv::Mat previousOutputMask;
    BackgroundEffectBuffers backgroundEffectBuffers;
    StatusFrameBuffers statusFrameBuffers;
    MediaFile backgroundMedia;
    if (config_.backgroundEffect == BackgroundEffect::Image && overlayEnabled) {
        const std::string resolvedBackgroundPath = mediaPath(config_.backgroundImageFolder, config_.backgroundImagePath);
        if (!backgroundMedia.ensureLoaded(resolvedBackgroundPath, config_.backgroundLoopIfVideo, outputSize, "background")) {
            LOG_ERROR("Cannot read background media: " << resolvedBackgroundPath);
            return ExitRuntimeError;
        }
        LOG_INFO("Background media loaded: " << resolvedBackgroundPath);
    }

    bool stoppedBySignal = false;
    while (true) {
        if (shutdownRequested()) {
            stoppedBySignal = true;
            break;
        }

        ProcessorConfig runtimeConfig = runtimeState.configSnapshot();
        const auto pipelineStartedAt = std::chrono::steady_clock::now();
        if (showWindow && !displayReady) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextDisplayReconnectAttempt) {
                displayReady = displayBackend->initialize(displayConfig);
                if (displayReady) {
                    LOG_INFO("Display backend connected: " << displayBackendToString(config_.displayBackend));
                    captureOpened = openCapture();
                    if (!captureOpened && !usingCamera) {
                        LOG_ERROR("Cannot open input file: " << config_.inputPath);
                        break;
                    }
                    if (!captureOpened && usingCamera) {
                        LOG_WARNING("Camera is not available, showing disconnected test image");
                    }
                } else {
                    displayBackend->shutdown();
                    nextDisplayReconnectAttempt = now + DisplayReconnectInterval;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        bool readOk = false;
        bool syntheticFrame = false;
        if (lowLatencyMode && usingCamera && !runtimeConfig.cameraEnabled) {
            if (captureOpened && !captureActive) {
                lowLatencyCapture.start(*captureBackend);
                captureActive = true;
            }
            if (!cameraRuntimePaused) {
                LOG_INFO("Camera input paused by runtime config; keeping V4L2 device open");
            }
            cameraRuntimePaused = true;
            cameraDevicePresentSince = {};
            nextReconnectAttempt = std::chrono::steady_clock::now();
            cameraEnableStartedAt = {};
            cameraWasDisabledByRuntime = true;
            frame = makeStatusFrame(outputSize, "Camera OFF", &runtimeConfig, &statusFrameBuffers);
            syntheticFrame = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            readOk = true;
        } else if (lowLatencyMode) {
            statusFrameBuffers.pauseCamera.release();
            if (cameraRuntimePaused) {
                LOG_INFO("Camera input resumed by runtime config");
                cameraRuntimePaused = false;
            }
            if (cameraWasDisabledByRuntime && cameraEnableStartedAt == std::chrono::steady_clock::time_point {}) {
                cameraEnableStartedAt = std::chrono::steady_clock::now();
            }
            if (!captureOpened) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextReconnectAttempt) {
                    const bool cameraDevicePresent = pathExists(config_.devicePath);
                    if (cameraDevicePresent && cameraDevicePresentSince == std::chrono::steady_clock::time_point {}) {
                        cameraDevicePresentSince = now;
                        LOG_INFO("Camera device appeared, waiting before reconnect: " << config_.devicePath);
                    } else if (!cameraDevicePresent) {
                        cameraDevicePresentSince = {};
                    }

                    if (cameraDevicePresent && now - cameraDevicePresentSince >= CameraReconnectSettleTime) {
                        captureOpened = captureBackend->open(config_);
                        if (captureOpened) {
                            cv::Mat reconnectFrame;
                            if (captureBackend->read(reconnectFrame) && !reconnectFrame.empty()) {
                                frame = reconnectFrame;
                                readOk = true;
                                LOG_INFO("Camera reconnected");
                                cameraDevicePresentSince = {};
                                cameraWasDisabledByRuntime = false;
                                cameraEnableStartedAt = {};
                                lowLatencyCapture.start(*captureBackend);
                                captureActive = true;
                                LOG_INFO("Camera input enabled by runtime config");
                            } else {
                                captureBackend->close();
                                captureOpened = false;
                                LOG_WARNING("Camera device opened but did not deliver a frame, keeping disconnected test image");
                            }
                        }
                        nextReconnectAttempt = now + CameraReconnectInterval;
                    } else {
                        nextReconnectAttempt = now + std::chrono::seconds(1);
                    }
                }
                if (!captureOpened && !readOk) {
                    const auto cameraConnectTimeout = std::chrono::seconds(runtimeConfig.cameraConnectTimeoutSeconds);
                    const bool keepCameraConnectingStatus = cameraWasDisabledByRuntime
                        && cameraEnableStartedAt != std::chrono::steady_clock::time_point {}
                        && std::chrono::steady_clock::now() - cameraEnableStartedAt < cameraConnectTimeout;
                    frame = makeStatusFrame(
                        outputSize,
                        keepCameraConnectingStatus ? "Camera connecting..." : "Camera DISCONNECTED",
                        &runtimeConfig,
                        &statusFrameBuffers);
                    syntheticFrame = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    readOk = true;
                }
            }
            if (captureOpened && !captureActive && !readOk) {
                lowLatencyCapture.start(*captureBackend);
                captureActive = true;
                LOG_INFO("Camera input enabled by runtime config");
            }
            if (captureActive) {
                std::chrono::steady_clock::duration captureWait {};
                std::chrono::steady_clock::duration frameHandover {};
                readOk = lowLatencyCapture.waitForLatestFrame(frame, captureWait, frameHandover);
                if (readOk) {
                    statusFrameBuffers.pauseCamera.release();
                    benchmark.add(BenchmarkStage::CaptureWait, captureWait);
                    benchmark.add(BenchmarkStage::FrameHandover, frameHandover);
                } else if (!shutdownRequested()) {
                    lowLatencyCapture.stop();
                    captureActive = false;
                    captureBackend->close();
                    captureOpened = false;
                    nextReconnectAttempt = std::chrono::steady_clock::now() + CameraReconnectInterval;
                    cameraDevicePresentSince = {};
                    cameraWasDisabledByRuntime = false;
                    cameraEnableStartedAt = {};
                    LOG_WARNING("Camera disconnected, showing disconnected test image");
                    frame = makeStatusFrame(outputSize, "Camera DISCONNECTED", &runtimeConfig, &statusFrameBuffers);
                    syntheticFrame = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    readOk = true;
                }
            }
        } else {
            const auto decodeStartedAt = std::chrono::steady_clock::now();
            readOk = captureBackend->read(frame);
            const auto decodeEndedAt = std::chrono::steady_clock::now();
            if (readOk) {
                benchmark.add(BenchmarkStage::Decode, decodeEndedAt - decodeStartedAt);
            }
        }

        if (!readOk) {
            if (shutdownRequested()) {
                stoppedBySignal = true;
            }
            break;
        }

        if (frame.empty()) {
            continue;
        }

        const auto processingStartedAt = std::chrono::steady_clock::now();

        const cv::Size frameProcessingSize = showWindow
            ? fitSizePreservingAspect(frame.size(), outputSize)
            : outputSize;

        cv::Mat resized;
        {
            BenchmarkScope timer(benchmark, BenchmarkStage::Resize);
            cv::resize(frame, resized, frameProcessingSize, 0.0, 0.0, cv::INTER_LINEAR);
        }

        cv::Mat outputMask;
        const bool runtimeMaskEnabled = !syntheticFrame && !runtimeConfig.noMask;
        const bool runtimeOverlayEnabled = !runtimeConfig.noOverlay && runtimeMaskEnabled;
        if (runtimeMaskEnabled && !maskBackend) {
            maskBackend = std::make_unique<TensorRtMaskBackend>();
            if (!maskBackend->initialize(runtimeConfig)) {
                LOG_ERROR("Cannot initialize mask backend: tensorrt");
                break;
            }
            LOG_INFO("Mask backend: " << maskBackend->name());
        }

        if (runtimeMaskEnabled && maskBackend) {
            maskBackend->updateConfig(runtimeConfig);
            cv::Mat segmentationMask;
            MaskTimings maskTimings;
            if (!maskBackend->generate(resized, frameIndex, segmentationMask, maskTimings)) {
                LOG_ERROR("Mask backend failed to generate a mask");
                break;
            }
            benchmark.add(BenchmarkStage::SegmentationPreprocess, maskTimings.preprocess);
            benchmark.add(BenchmarkStage::SegmentationInference, maskTimings.inference);
            benchmark.add(BenchmarkStage::SegmentationPostprocess, maskTimings.postprocess);

            if (!segmentationMask.empty()) {
                BenchmarkScope timer(benchmark, BenchmarkStage::MaskUpscale);
                cv::resize(segmentationMask, outputMask, resized.size(), 0.0, 0.0, cv::INTER_LINEAR);
            }

            if (!outputMask.empty()) {
                BenchmarkScope timer(benchmark, BenchmarkStage::MaskPostprocess);
                outputMask = applyMaskPostprocessing(outputMask, previousOutputMask, runtimeConfig);
            } else {
                previousOutputMask.release();
            }
        } else {
            previousOutputMask.release();
        }

        cv::Mat outputFrame;
        if (runtimeOverlayEnabled && !outputMask.empty()) {
            if (runtimeConfig.backgroundEffect == BackgroundEffect::Blur) {
                BenchmarkScope timer(benchmark, BenchmarkStage::BackgroundBlur);
                outputFrame = applyBackgroundBlur(
                    resized,
                    outputMask,
                    runtimeConfig.blurStrength,
                    backgroundEffectBuffers);
            } else if (runtimeConfig.backgroundEffect == BackgroundEffect::Image) {
                const std::string resolvedBackgroundPath = mediaPath(runtimeConfig.backgroundImageFolder, runtimeConfig.backgroundImagePath);
                if (!backgroundMedia.ensureLoaded(resolvedBackgroundPath, runtimeConfig.backgroundLoopIfVideo, resized.size(), "background")) {
                    LOG_WARNING("Cannot read background media: " << resolvedBackgroundPath);
                    backgroundEffectBuffers.scaledBackgroundImage.release();
                }
                cv::Mat backgroundFrame;
                const bool hasBackgroundFrame = backgroundMedia.read(backgroundFrame) && !backgroundFrame.empty();
                if (backgroundMedia.isVideo || backgroundMedia.isHtml) {
                    backgroundEffectBuffers.scaledBackgroundImage.release();
                }
                BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
                outputFrame = applyBackgroundImage(
                    resized,
                    outputMask,
                    backgroundFrame,
                    backgroundEffectBuffers);
                if (!hasBackgroundFrame) {
                    outputFrame = resized;
                }
            } else if (runtimeConfig.backgroundEffect == BackgroundEffect::Color) {
                BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
                outputFrame = applyBackgroundOverlay(
                    resized,
                    outputMask,
                    runtimeConfig.backgroundOverlayColor,
                    runtimeConfig.backgroundOverlayAlpha);
            } else {
                outputFrame = resized;
            }
        } else {
            outputFrame = resized;
        }

        if (showWindow) {
            BenchmarkScope timer(benchmark, BenchmarkStage::Display);
            if (!displayBackend->render(outputFrame)) {
                LOG_WARNING("Display backend render failed, waiting for display reconnect");
                displayBackend->shutdown();
                displayReady = false;
                nextDisplayReconnectAttempt = std::chrono::steady_clock::now() + DisplayReconnectInterval;
                if (captureActive) {
                    lowLatencyCapture.stop();
                    captureActive = false;
                }
                if (usingCamera && captureOpened) {
                    captureBackend->close();
                    captureOpened = false;
                }
                continue;
            }
        }

        ++frameIndex;
        ++intervalFrames;
        const auto frameEndedAt = std::chrono::steady_clock::now();
        benchmark.add(BenchmarkStage::ProcessingTotal, frameEndedAt - processingStartedAt);
        benchmark.add(BenchmarkStage::PipelineTotal, frameEndedAt - pipelineStartedAt);
        benchmark.frameCompleted();
        runtimeState.updateBenchmark(benchmark.snapshot());
        benchmark.maybeLogProgress();

        if (config_.verbose) {
            const auto now = std::chrono::steady_clock::now();
            if (now - intervalStartedAt >= std::chrono::seconds(1)) {
                logPerformance(frameIndex, intervalFrames, startedAt, intervalStartedAt, now);
                if (lowLatencyMode) {
                    const auto stats = lowLatencyCapture.stats();
                    LOG_VERBOSE("Frames captured: " << stats.capturedFrames);
                    LOG_VERBOSE("Frames processed: " << frameIndex);
                    LOG_VERBOSE("Frames dropped/overwritten: " << effectiveDroppedFrames(stats, frameIndex));
                }
                intervalStartedAt = now;
                intervalFrames = 0;
            }
        }
    }

    if (shutdownRequested()) {
        stoppedBySignal = true;
    }

    if (captureActive) {
        lowLatencyCapture.stop();
    }
    if (lowLatencyMode) {
        const auto stats = lowLatencyCapture.stats();
        const std::size_t droppedFrames = effectiveDroppedFrames(stats, frameIndex);
        LOG_VERBOSE("Frames captured: " << stats.capturedFrames);
        LOG_VERBOSE("Frames processed: " << frameIndex);
        LOG_VERBOSE("Frames dropped/overwritten: " << droppedFrames);
        benchmark.setCaptureStats(stats.capturedFrames, droppedFrames, stats.elapsed);
    }

    if (stoppedBySignal) {
        LOG_INFO("Shutdown requested, stopping JONImageProcessor");
    }

    if (frameIndex == 0 && !stoppedBySignal) {
        LOG_ERROR("No frames could be read");
        return ExitRuntimeError;
    }

    LOG_INFO("Processed frames: " << frameIndex);
    benchmark.logSummary();
    ipcServer.stop();
    if (displayBackend) {
        displayBackend->shutdown();
    }
    captureBackend->close();

    return ExitOk;
}
