#include "ipc/IPCServer.h"

#include "Logger.h"

#include <opencv2/imgcodecs.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace {

struct JsonValue {
    enum class Type { Missing, String, Number, Boolean };
    Type type = Type::Missing;
    std::string text;
    double number = 0.0;
    bool boolean = false;
};

std::string errorResponse(const std::string& error)
{
    return "{\"ok\":false,\"error\":\"" + error + "\"}\n";
}

std::string escapeJson(const std::string& value)
{
    std::string out;
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool parseObject(const std::string& line, std::map<std::string, JsonValue>& out)
{
    std::size_t i = 0;
    auto skipWs = [&]() {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) {
            ++i;
        }
    };
    auto parseString = [&]() -> std::string {
        std::string result;
        if (i >= line.size() || line[i++] != '"') {
            return {};
        }
        while (i < line.size()) {
            const char c = line[i++];
            if (c == '"') {
                return result;
            }
            if (c == '\\' && i < line.size()) {
                result.push_back(line[i++]);
            } else {
                result.push_back(c);
            }
        }
        return {};
    };

    skipWs();
    if (i >= line.size() || line[i++] != '{') {
        return false;
    }
    while (true) {
        skipWs();
        if (i < line.size() && line[i] == '}') {
            ++i;
            skipWs();
            return i == line.size();
        }
        const std::string key = parseString();
        if (key.empty()) {
            return false;
        }
        skipWs();
        if (i >= line.size() || line[i++] != ':') {
            return false;
        }
        skipWs();

        JsonValue value;
        if (i < line.size() && line[i] == '"') {
            value.type = JsonValue::Type::String;
            value.text = parseString();
        } else if (line.compare(i, 4, "true") == 0) {
            value.type = JsonValue::Type::Boolean;
            value.boolean = true;
            i += 4;
        } else if (line.compare(i, 5, "false") == 0) {
            value.type = JsonValue::Type::Boolean;
            value.boolean = false;
            i += 5;
        } else {
            char* end = nullptr;
            value.number = std::strtod(line.c_str() + i, &end);
            if (end == line.c_str() + i) {
                return false;
            }
            value.type = JsonValue::Type::Number;
            i = static_cast<std::size_t>(end - line.c_str());
        }
        out[key] = value;
        skipWs();
        if (i < line.size() && line[i] == ',') {
            ++i;
            continue;
        }
        if (i < line.size() && line[i] == '}') {
            continue;
        }
        return false;
    }
}

std::string colorToString(const RgbColor& color)
{
    std::ostringstream out;
    out << color.r << "," << color.g << "," << color.b;
    return out.str();
}

bool parseColor(const std::string& value, RgbColor& color)
{
    int parts[3] {};
    const char* current = value.c_str();
    char* end = nullptr;
    for (int i = 0; i < 3; ++i) {
        const long parsed = std::strtol(current, &end, 10);
        if (end == current || parsed < 0 || parsed > 255) {
            return false;
        }
        parts[i] = static_cast<int>(parsed);
        if (i < 2) {
            if (*end != ',') {
                return false;
            }
            current = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    color = RgbColor {parts[0], parts[1], parts[2]};
    return true;
}

bool setStringEnum(const std::string& value, ProcessorConfig& config, const std::string& key)
{
    if (key == "mask_morphology" || key == "segmentation.morphology") {
        if (value == "off") config.maskMorphology = MaskMorphologyMode::Off;
        else if (value == "light") config.maskMorphology = MaskMorphologyMode::Light;
        else if (value == "strong") config.maskMorphology = MaskMorphologyMode::Strong;
        else return false;
        return true;
    }
    if (key == "background_effect" || key == "background.effect") {
        if (value == "color") config.backgroundEffect = BackgroundEffect::Color;
        else if (value == "blur") config.backgroundEffect = BackgroundEffect::Blur;
        else if (value == "image") config.backgroundEffect = BackgroundEffect::Image;
        else return false;
        return true;
    }
    return false;
}

std::string benchmarkJson(const BenchmarkSnapshot& b)
{
    std::ostringstream out;
    out << "{\"frames_processed\":" << b.framesProcessed
        << ",\"fps\":" << b.fps
        << ",\"avg_frame_ms\":" << b.avgFrameMs
        << ",\"processing_total_ms\":" << b.processingTotalMs
        << ",\"pipeline_total_ms\":" << b.pipelineTotalMs
        << "}";
    return out.str();
}

std::string valueJson(const ProcessorConfig& c, const std::string& key, const BenchmarkSnapshot& b)
{
    if (key == "mask_threshold" || key == "segmentation.threshold") return std::to_string(c.maskThreshold);
    if (key == "mask_smoothing" || key == "segmentation.smoothing") return std::to_string(c.maskSmoothing);
    if (key == "mask_morphology" || key == "segmentation.morphology") return "\"" + maskMorphologyModeToString(c.maskMorphology) + "\"";
    if (key == "background_effect" || key == "background.effect") return "\"" + backgroundEffectToString(c.backgroundEffect) + "\"";
    if (key == "background_image" || key == "background.image") return "\"" + escapeJson(c.backgroundImagePath) + "\"";
    if (key == "background_overlay_color" || key == "background.overlayColor") return "\"" + colorToString(c.backgroundOverlayColor) + "\"";
    if (key == "background_overlay_alpha" || key == "background.overlayAlpha") return std::to_string(c.backgroundOverlayAlpha);
    if (key == "blur_strength" || key == "background.blurStrength") return std::to_string(c.blurStrength);
    if (key == "no_mask" || key == "runtime.noMask") return c.noMask ? "true" : "false";
    if (key == "no_overlay" || key == "runtime.noOverlay") return c.noOverlay ? "true" : "false";
    if (key == "camera.enabled") return c.cameraEnabled ? "true" : "false";
    if (key == "benchmark") return benchmarkJson(b);
    return {};
}

bool knownKey(const std::string& key)
{
    return key == "mask_threshold" || key == "mask_smoothing" || key == "mask_morphology"
        || key == "background_effect" || key == "background_image" || key == "background_overlay_color"
        || key == "background_overlay_alpha" || key == "blur_strength" || key == "no_mask"
        || key == "no_overlay" || key == "benchmark"
        || key == "segmentation.threshold" || key == "segmentation.smoothing" || key == "segmentation.morphology"
        || key == "background.effect" || key == "background.image" || key == "background.overlayColor"
        || key == "background.overlayAlpha" || key == "background.blurStrength"
        || key == "runtime.noMask" || key == "runtime.noOverlay" || key == "camera.enabled";
}

std::string validateRuntimeConfig(const ProcessorConfig& config)
{
    if (config.backgroundEffect == BackgroundEffect::Image) {
        if (config.backgroundImagePath.empty()) {
            return "background_image is required for background_effect image";
        }
        if (cv::imread(config.backgroundImagePath, cv::IMREAD_COLOR).empty()) {
            return "background_image cannot be read";
        }
    }
    return {};
}

} // namespace

IPCServer::IPCServer(RuntimeState& runtimeState, std::string socketPath)
    : runtimeState_(runtimeState)
    , socketPath_(std::move(socketPath))
{
}

IPCServer::~IPCServer()
{
    stop();
}

bool IPCServer::start()
{
    if (socketPath_ == "none") {
        return true;
    }
    ::unlink(socketPath_.c_str());
    serverFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        LOG_ERROR("Cannot create IPC socket: " << std::strerror(errno));
        return false;
    }
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(serverFd_, 8) != 0) {
        LOG_ERROR("Cannot bind IPC socket " << socketPath_ << ": " << std::strerror(errno));
        stop();
        return false;
    }
    running_ = true;
    thread_ = std::thread(&IPCServer::run, this);
    LOG_INFO("IPC socket: " << socketPath_);
    return true;
}

void IPCServer::stop()
{
    running_ = false;
    if (serverFd_ >= 0) {
        ::shutdown(serverFd_, SHUT_RDWR);
        ::close(serverFd_);
        serverFd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (socketPath_ != "none") {
        ::unlink(socketPath_.c_str());
    }
}

void IPCServer::run()
{
    while (running_) {
        const int clientFd = ::accept(serverFd_, nullptr, nullptr);
        if (clientFd >= 0) {
            handleClient(clientFd);
            ::close(clientFd);
        }
    }
}

void IPCServer::handleClient(int clientFd)
{
    std::string line;
    char ch = 0;
    while (::read(clientFd, &ch, 1) == 1 && ch != '\n') {
        line.push_back(ch);
        if (line.size() > 8192) {
            break;
        }
    }
    const std::string response = handleLine(line);
    (void)::write(clientFd, response.data(), response.size());
}

std::string IPCServer::handleLine(const std::string& line)
{
    std::map<std::string, JsonValue> request;
    if (!parseObject(line, request) || request["cmd"].type != JsonValue::Type::String) {
        return errorResponse("invalid json");
    }
    const std::string cmd = request["cmd"].text;
    const ProcessorConfig current = runtimeState_.configSnapshot();
    const BenchmarkSnapshot benchmark = runtimeState_.benchmarkSnapshot();

    if (cmd == "list") {
        std::ostringstream out;
        out << "{\"ok\":true,\"values\":{"
            << "\"camera\":{\"enabled\":" << (current.cameraEnabled ? "true" : "false") << "}"
            << ",\"segmentation\":{\"threshold\":" << current.maskThreshold
            << ",\"smoothing\":" << current.maskSmoothing
            << ",\"morphology\":\"" << maskMorphologyModeToString(current.maskMorphology) << "\"}"
            << ",\"background\":{\"effect\":\"" << backgroundEffectToString(current.backgroundEffect)
            << "\",\"image\":\"" << escapeJson(current.backgroundImagePath)
            << "\",\"overlayColor\":\"" << colorToString(current.backgroundOverlayColor)
            << "\",\"overlayAlpha\":" << current.backgroundOverlayAlpha
            << ",\"blurStrength\":" << current.blurStrength << "}"
            << ",\"runtime\":{\"noMask\":" << (current.noMask ? "true" : "false")
            << ",\"noOverlay\":" << (current.noOverlay ? "true" : "false") << "}";
        if (current.benchmark) {
            out << ",\"benchmark\":" << benchmarkJson(benchmark);
        }
        out << "}}\n";
        return out.str();
    }

    if ((cmd == "get" || cmd == "set") && request["key"].type != JsonValue::Type::String) {
        return errorResponse("unknown key");
    }
    const std::string key = request["key"].text;
    if (cmd == "get") {
        if (!knownKey(key)) {
            return errorResponse("unknown key");
        }
        if (key == "benchmark" && !current.benchmark) {
            return errorResponse("benchmark is not enabled");
        }
        return "{\"ok\":true,\"key\":\"" + key + "\",\"value\":" + valueJson(current, key, benchmark) + "}\n";
    }
    if (cmd != "set") {
        return errorResponse("unknown command");
    }
    if (key == "benchmark") {
        return errorResponse("benchmark is read-only");
    }
    if (!knownKey(key)) {
        return errorResponse("unknown key");
    }

    ProcessorConfig updated = current;
    const JsonValue value = request["value"];
    if (key == "mask_threshold" || key == "segmentation.threshold"
        || key == "mask_smoothing" || key == "segmentation.smoothing"
        || key == "background_overlay_alpha" || key == "background.overlayAlpha") {
        if (value.type != JsonValue::Type::Number) return errorResponse("invalid value type");
        if (value.number < 0.0 || value.number > 1.0) return errorResponse("invalid value");
        if (key == "mask_threshold" || key == "segmentation.threshold") updated.maskThreshold = value.number;
        else if (key == "mask_smoothing" || key == "segmentation.smoothing") updated.maskSmoothing = value.number;
        else updated.backgroundOverlayAlpha = value.number;
    } else if (key == "blur_strength" || key == "background.blurStrength") {
        if (value.type != JsonValue::Type::Number) return errorResponse("invalid value type");
        const int blur = static_cast<int>(value.number);
        if (value.number != static_cast<double>(blur) || blur < 1 || blur > 100) return errorResponse("invalid value");
        updated.blurStrength = blur;
    } else if (key == "mask_morphology" || key == "segmentation.morphology" || key == "background_effect" || key == "background.effect") {
        if (value.type != JsonValue::Type::String) return errorResponse("invalid value type");
        if (!setStringEnum(value.text, updated, key)) return errorResponse("invalid value");
    } else if (key == "background_image" || key == "background.image") {
        if (value.type != JsonValue::Type::String) return errorResponse("invalid value type");
        updated.backgroundImagePath = value.text;
    } else if (key == "background_overlay_color" || key == "background.overlayColor") {
        if (value.type != JsonValue::Type::String) return errorResponse("invalid value type");
        if (!parseColor(value.text, updated.backgroundOverlayColor)) return errorResponse("invalid value");
    } else if (key == "no_mask" || key == "runtime.noMask" || key == "no_overlay" || key == "runtime.noOverlay") {
        if (value.type != JsonValue::Type::Boolean) return errorResponse("invalid value type");
        if (key == "no_mask" || key == "runtime.noMask") updated.noMask = value.boolean;
        else updated.noOverlay = value.boolean;
    } else if (key == "camera.enabled") {
        if (value.type != JsonValue::Type::Boolean) return errorResponse("invalid value type");
        updated.cameraEnabled = value.boolean;
    }
    const std::string configError = validateRuntimeConfig(updated);
    if (!configError.empty()) {
        return errorResponse(configError);
    }
    runtimeState_.updateConfig(updated);
    return "{\"ok\":true}\n";
}
