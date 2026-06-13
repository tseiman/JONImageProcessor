#include "ConfigFile.h"

#include "Logger.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace {

struct Json {
    enum class Type { Null, Object, String, Number, Boolean };
    Type type = Type::Null;
    std::map<std::string, Json> object;
    std::string text;
    double number = 0.0;
    bool boolean = false;
};

class Parser {
public:
    explicit Parser(std::string input)
        : input_(std::move(input))
    {
    }

    bool parse(Json& value, std::string& error)
    {
        skipWs();
        if (!parseValue(value)) {
            error = formatError();
            return false;
        }
        skipWs();
        if (pos_ != input_.size()) {
            setError("Unexpected trailing data");
            error = formatError();
            return false;
        }
        return true;
    }

private:
    void setError(const std::string& message)
    {
        if (errorMessage_.empty()) {
            errorMessage_ = message;
            errorPos_ = pos_;
        }
    }

    std::string formatError() const
    {
        std::size_t line = 1;
        std::size_t column = 1;
        const std::size_t limit = std::min(errorPos_, input_.size());
        for (std::size_t index = 0; index < limit; ++index) {
            if (input_[index] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        std::ostringstream stream;
        stream << (errorMessage_.empty() ? "Invalid JSON syntax" : errorMessage_)
               << " at line " << line << ", column " << column;
        return stream.str();
    }

    void skipWs()
    {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\n' || input_[pos_] == '\r' || input_[pos_] == '\t')) ++pos_;
    }

    bool parseValue(Json& value)
    {
        skipWs();
        if (pos_ >= input_.size()) {
            setError("Unexpected end of JSON");
            return false;
        }
        if (input_[pos_] == '{') return parseObject(value);
        if (input_[pos_] == '"') {
            value.type = Json::Type::String;
            return parseString(value.text);
        }
        if (input_.compare(pos_, 4, "true") == 0) {
            value.type = Json::Type::Boolean;
            value.boolean = true;
            pos_ += 4;
            return true;
        }
        if (input_.compare(pos_, 5, "false") == 0) {
            value.type = Json::Type::Boolean;
            value.boolean = false;
            pos_ += 5;
            return true;
        }
        char* end = nullptr;
        value.number = std::strtod(input_.c_str() + pos_, &end);
        if (end == input_.c_str() + pos_) {
            setError("Expected JSON value");
            return false;
        }
        value.type = Json::Type::Number;
        pos_ = static_cast<std::size_t>(end - input_.c_str());
        return true;
    }

    bool parseString(std::string& out)
    {
        if (pos_ >= input_.size() || input_[pos_++] != '"') {
            setError("Expected string");
            return false;
        }
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    setError("Unterminated escape sequence");
                    return false;
                }
                out.push_back(input_[pos_++]);
            } else {
                out.push_back(c);
            }
        }
        setError("Unterminated string");
        return false;
    }

    bool parseObject(Json& value)
    {
        value.type = Json::Type::Object;
        ++pos_;
        skipWs();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < input_.size()) {
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= input_.size() || input_[pos_++] != ':') {
                setError("Expected ':' after object key");
                return false;
            }
            Json child;
            if (!parseValue(child)) return false;
            value.object[key] = child;
            skipWs();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                ++pos_;
                skipWs();
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == '}') {
                ++pos_;
                return true;
            }
            setError("Expected ',' or '}' in object");
            return false;
        }
        setError("Unterminated object");
        return false;
    }

    std::string input_;
    std::size_t pos_ = 0;
    std::size_t errorPos_ = 0;
    std::string errorMessage_;
};

bool fileExists(const std::string& path)
{
    std::ifstream file(path);
    return static_cast<bool>(file);
}

std::string executableDir()
{
    char buffer[4096] {};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) return ".";
    std::string path(buffer, static_cast<std::size_t>(size));
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

const Json* objectChild(const Json& root, const std::string& name)
{
    if (root.type != Json::Type::Object) return nullptr;
    const auto it = root.object.find(name);
    return it == root.object.end() ? nullptr : &it->second;
}

bool readString(const Json& object, const std::string& key, std::string& out, std::string& error)
{
    const Json* value = objectChild(object, key);
    if (!value) return true;
    if (value->type != Json::Type::String) {
        error = "Invalid JSON type for " + key;
        return false;
    }
    out = value->text;
    return true;
}

bool readNumber(const Json& object, const std::string& key, double& out, std::string& error)
{
    const Json* value = objectChild(object, key);
    if (!value) return true;
    if (value->type != Json::Type::Number) {
        error = "Invalid JSON type for " + key;
        return false;
    }
    out = value->number;
    return true;
}

bool readBoolean(const Json& object, const std::string& key, bool& out, std::string& error)
{
    const Json* value = objectChild(object, key);
    if (!value) return true;
    if (value->type != Json::Type::Boolean) {
        error = "Invalid JSON type for " + key;
        return false;
    }
    out = value->boolean;
    return true;
}

bool parsePositiveInt(const std::string& value, int& out)
{
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > 16384) return false;
    out = static_cast<int>(parsed);
    return true;
}

bool parseSizeValue(const std::string& value, int& width, int& height)
{
    const std::size_t x = value.find('x');
    if (x == std::string::npos || x == 0 || x == value.size() - 1 || value.find('x', x + 1) != std::string::npos) return false;
    return parsePositiveInt(value.substr(0, x), width) && parsePositiveInt(value.substr(x + 1), height);
}

bool parsePositionValue(const std::string& value, Point2i& position)
{
    if (value == "auto") {
        position = Point2i {};
        return true;
    }
    const std::size_t x = value.find('x');
    if (x == std::string::npos || x == 0 || x == value.size() - 1 || value.find('x', x + 1) != std::string::npos) return false;
    char* end = nullptr;
    const std::string xPart = value.substr(0, x);
    const long parsedX = std::strtol(xPart.c_str(), &end, 10);
    if (end == xPart.c_str() || *end != '\0' || parsedX < 0 || parsedX > 16384) return false;
    const std::string yPart = value.substr(x + 1);
    const long parsedY = std::strtol(yPart.c_str(), &end, 10);
    if (end == yPart.c_str() || *end != '\0' || parsedY < 0 || parsedY > 16384) return false;
    position.x = static_cast<int>(parsedX);
    position.y = static_cast<int>(parsedY);
    return true;
}

bool parseUnit(double value)
{
    return value >= 0.0 && value <= 1.0;
}

bool parseColor(const std::string& value, RgbColor& color)
{
    int parts[3] {};
    const char* current = value.c_str();
    char* end = nullptr;
    for (int i = 0; i < 3; ++i) {
        const long parsed = std::strtol(current, &end, 10);
        if (end == current || parsed < 0 || parsed > 255) return false;
        parts[i] = static_cast<int>(parsed);
        if (i < 2) {
            if (*end != ',') return false;
            current = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    color = RgbColor {parts[0], parts[1], parts[2]};
    return true;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseRgbaHexColor(const std::string& value, RgbaColor& color)
{
    if (value.size() != 8) return false;
    int parts[4] {};
    for (int index = 0; index < 4; ++index) {
        const int high = hexValue(value[static_cast<std::size_t>(index * 2)]);
        const int low = hexValue(value[static_cast<std::size_t>(index * 2 + 1)]);
        if (high < 0 || low < 0) return false;
        parts[index] = high * 16 + low;
    }
    color = RgbaColor {parts[0], parts[1], parts[2], parts[3]};
    return true;
}

bool parsePauseFont(const std::string& value)
{
    return value == "plain" || value == "simplex" || value == "duplex"
        || value == "complex" || value == "triplex" || value == "complex-small"
        || value == "script-simplex" || value == "script-complex";
}

void warnUnknownFields(const Json& object, const std::string& prefix, const std::initializer_list<const char*> allowed)
{
    if (object.type != Json::Type::Object) return;
    for (const auto& item : object.object) {
        bool known = false;
        for (const char* key : allowed) {
            if (item.first == key) known = true;
        }
        if (!known) LOG_WARNING("Ignoring unknown JSON config field: " << prefix << item.first);
    }
}

bool applyConfig(const Json& root, ProcessorConfig& config, ConfigLoadResult& result, std::string& error)
{
    if (root.type != Json::Type::Object) {
        error = "JSON config root must be an object";
        return false;
    }
    warnUnknownFields(root, "", {"camera", "processing", "segmentation", "background", "pause", "output", "ipc", "display", "diagnostics"});

    if (const Json* camera = objectChild(root, "camera")) {
        if (camera->type != Json::Type::Object) { error = "camera must be an object"; return false; }
        warnUnknownFields(*camera, "camera.", {"device", "format", "connectTimeoutSeconds"});
        if (!readString(*camera, "device", config.devicePath, error)) return false;
        std::string format;
        if (!readString(*camera, "format", format, error)) return false;
        if (!format.empty()) {
            if (format == "MJPG") config.cameraFormat = CameraFormat::MJPG;
            else if (format == "YUYV") config.cameraFormat = CameraFormat::YUYV;
            else { error = "Invalid camera.format"; return false; }
        }
        double connectTimeoutSeconds = static_cast<double>(config.cameraConnectTimeoutSeconds);
        if (!readNumber(*camera, "connectTimeoutSeconds", connectTimeoutSeconds, error)) return false;
        config.cameraConnectTimeoutSeconds = static_cast<int>(connectTimeoutSeconds);
        if (connectTimeoutSeconds != static_cast<double>(config.cameraConnectTimeoutSeconds)
            || config.cameraConnectTimeoutSeconds < 1
            || config.cameraConnectTimeoutSeconds > 300) {
            error = "Invalid camera.connectTimeoutSeconds";
            return false;
        }
    }

    if (const Json* processing = objectChild(root, "processing")) {
        if (processing->type != Json::Type::Object) { error = "processing must be an object"; return false; }
        warnUnknownFields(*processing, "processing.", {"size"});
        std::string size;
        if (!readString(*processing, "size", size, error)) return false;
        if (!size.empty() && !parseSizeValue(size, config.width, config.height)) { error = "Invalid processing.size"; return false; }
    }

    if (const Json* segmentation = objectChild(root, "segmentation")) {
        if (segmentation->type != Json::Type::Object) { error = "segmentation must be an object"; return false; }
        warnUnknownFields(*segmentation, "segmentation.", {"size", "maskModel", "threshold", "smoothing", "morphology"});
        std::string size;
        if (!readString(*segmentation, "size", size, error)) return false;
        if (!size.empty() && !parseSizeValue(size, config.segmentationWidth, config.segmentationHeight)) { error = "Invalid segmentation.size"; return false; }
        if (!readString(*segmentation, "maskModel", config.maskModelPath, error)) return false;
        if (!readNumber(*segmentation, "threshold", config.maskThreshold, error) || !parseUnit(config.maskThreshold)) { error = "Invalid segmentation.threshold"; return false; }
        if (!readNumber(*segmentation, "smoothing", config.maskSmoothing, error) || !parseUnit(config.maskSmoothing)) { error = "Invalid segmentation.smoothing"; return false; }
        std::string morphology;
        if (!readString(*segmentation, "morphology", morphology, error)) return false;
        if (!morphology.empty()) {
            if (morphology == "off") config.maskMorphology = MaskMorphologyMode::Off;
            else if (morphology == "light") config.maskMorphology = MaskMorphologyMode::Light;
            else if (morphology == "strong") config.maskMorphology = MaskMorphologyMode::Strong;
            else { error = "Invalid segmentation.morphology"; return false; }
        }
    }

    if (const Json* background = objectChild(root, "background")) {
        if (background->type != Json::Type::Object) { error = "background must be an object"; return false; }
        warnUnknownFields(*background, "background.", {"effect", "image", "overlayColor", "overlayAlpha", "blurStrength"});
        std::string effect;
        if (!readString(*background, "effect", effect, error)) return false;
        if (!effect.empty()) {
            if (effect == "color") config.backgroundEffect = BackgroundEffect::Color;
            else if (effect == "blur") config.backgroundEffect = BackgroundEffect::Blur;
            else if (effect == "image") config.backgroundEffect = BackgroundEffect::Image;
            else { error = "Invalid background.effect"; return false; }
        }
        if (!readString(*background, "image", config.backgroundImagePath, error)) return false;
        std::string color;
        if (!readString(*background, "overlayColor", color, error)) return false;
        if (!color.empty() && !parseColor(color, config.backgroundOverlayColor)) { error = "Invalid background.overlayColor"; return false; }
        if (!readNumber(*background, "overlayAlpha", config.backgroundOverlayAlpha, error) || !parseUnit(config.backgroundOverlayAlpha)) { error = "Invalid background.overlayAlpha"; return false; }
        double blur = static_cast<double>(config.blurStrength);
        if (!readNumber(*background, "blurStrength", blur, error)) return false;
        config.blurStrength = static_cast<int>(blur);
        if (blur != static_cast<double>(config.blurStrength) || config.blurStrength < 1 || config.blurStrength > 100) { error = "Invalid background.blurStrength"; return false; }
    }

    if (const Json* pause = objectChild(root, "pause")) {
        if (pause->type != Json::Type::Object) { error = "pause must be an object"; return false; }
        warnUnknownFields(*pause, "pause.", {"enabled", "image", "showStatusText", "textColor", "textPosition", "textSize", "font"});
        if (!readBoolean(*pause, "enabled", config.pauseImageEnabled, error)) return false;
        if (!readString(*pause, "image", config.pauseImagePath, error)) return false;
        if (!readBoolean(*pause, "showStatusText", config.pauseImageShowStatusText, error)) return false;
        std::string textColor;
        if (!readString(*pause, "textColor", textColor, error)) return false;
        if (!textColor.empty() && !parseRgbaHexColor(textColor, config.pauseImageTextColor)) { error = "Invalid pause.textColor"; return false; }
        std::string textPosition;
        if (!readString(*pause, "textPosition", textPosition, error)) return false;
        if (!textPosition.empty() && !parsePositionValue(textPosition, config.pauseImageTextPosition)) { error = "Invalid pause.textPosition"; return false; }
        double textSize = config.pauseImageTextSize;
        if (!readNumber(*pause, "textSize", textSize, error)) return false;
        if (textSize < 0.1 || textSize > 10.0) { error = "Invalid pause.textSize"; return false; }
        config.pauseImageTextSize = textSize;
        std::string font;
        if (!readString(*pause, "font", font, error)) return false;
        if (!font.empty()) {
            if (!parsePauseFont(font)) { error = "Invalid pause.font"; return false; }
            config.pauseImageFont = font;
        }
    }

    if (const Json* output = objectChild(root, "output")) {
        if (output->type != Json::Type::Object) { error = "output must be an object"; return false; }
        warnUnknownFields(*output, "output.", {"size"});
        std::string size;
        if (!readString(*output, "size", size, error)) return false;
        if (size == "auto" || size.empty()) {
            config.outputWidth = 0;
            config.outputHeight = 0;
        } else if (!parseSizeValue(size, config.outputWidth, config.outputHeight)) {
            error = "Invalid output.size";
            return false;
        }
    }

    if (const Json* ipc = objectChild(root, "ipc")) {
        if (ipc->type != Json::Type::Object) { error = "ipc must be an object"; return false; }
        warnUnknownFields(*ipc, "ipc.", {"socket"});
        if (!readString(*ipc, "socket", config.ipcSocketPath, error)) return false;
    }

    if (const Json* display = objectChild(root, "display")) {
        result.displayConfigured = true;
        if (display->type != Json::Type::Object) { error = "display must be an object"; return false; }
        warnUnknownFields(*display, "display.", {"backend", "mode"});
        std::string backend;
        if (!readString(*display, "backend", backend, error)) return false;
        if (!backend.empty()) {
            if (backend == "highgui") config.displayBackend = DisplayBackendType::HighGui;
            else if (backend == "drm") config.displayBackend = DisplayBackendType::Drm;
            else { error = "Invalid display.backend"; return false; }
        }
        std::string mode;
        if (!readString(*display, "mode", mode, error)) return false;
        if (mode == "fullscreen") config.fullscreen = true;
        else if (!mode.empty()) LOG_WARNING("Ignoring unsupported JSON config field value: display.mode=" << mode);
    }

    if (const Json* diagnostics = objectChild(root, "diagnostics")) {
        if (diagnostics->type != Json::Type::Object) { error = "diagnostics must be an object"; return false; }
        warnUnknownFields(*diagnostics, "diagnostics.", {"benchmark"});
        if (!readBoolean(*diagnostics, "benchmark", config.benchmark, error)) return false;
    }

    return true;
}

} // namespace

std::string findDefaultConfigPath(const char*)
{
    if (fileExists("/etc/jonimageprocessor.json")) return "/etc/jonimageprocessor.json";
    const std::string besideExecutable = executableDir() + "/jonimageprocessor.json";
    if (fileExists(besideExecutable)) return besideExecutable;
    return {};
}

bool loadJsonConfigFile(const std::string& path, ProcessorConfig& config, ConfigLoadResult& result, std::string& error)
{
    std::ifstream file(path);
    if (!file) {
        error = "Cannot read config file: " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    Json root;
    Parser parser(buffer.str());
    std::string parseError;
    if (!parser.parse(root, parseError)) {
        error = "Invalid JSON config file: " + path + ": " + parseError;
        return false;
    }
    if (!applyConfig(root, config, result, error)) {
        error = path + ": " + error;
        return false;
    }
    result.loaded = true;
    LOG_INFO("Loaded config file: " << path);
    return true;
}
