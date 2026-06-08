#include "DrmKmsDisplayBackend.h"

#if defined(JON_ENABLE_DRM_DISPLAY)

#include "DisplayRenderer.h"
#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* VertexShaderSource = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main()
{
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

constexpr const char* FragmentShaderSource = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main()
{
    vec4 color = texture2D(uTexture, vTexCoord);
    gl_FragColor = vec4(color.b, color.g, color.r, 1.0);
}
)";

std::string sizeToString(cv::Size size)
{
    std::ostringstream stream;
    stream << size.width << "x" << size.height;
    return stream.str();
}

std::string displayModeName(DisplayMode mode)
{
    return displayModeToString(mode);
}

std::string drmError(const std::string& action)
{
    return action + ": " + std::strerror(errno);
}

void logDrmMasterHint()
{
    if (errno == EACCES || errno == EPERM) {
        LOG_ERROR("DRM/KMS permission denied. The display may be owned by a desktop compositor/display manager; stop it or run JONImageProcessor as the boot-time display owner.");
    }
}

bool hasStdinQuitRequest()
{
    pollfd descriptor {};
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;

    if (poll(&descriptor, 1, 0) <= 0 || (descriptor.revents & POLLIN) == 0) {
        return false;
    }

    char value = 0;
    if (read(STDIN_FILENO, &value, 1) != 1) {
        return false;
    }

    return value == 'q' || value == 'Q' || value == 27;
}

GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return shader;
    }

    char log[1024] {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOG_ERROR("DRM display shader compilation failed: " << log);
    glDeleteShader(shader);
    return 0;
}

GLuint createProgram()
{
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, VertexShaderSource);
    if (vertexShader == 0) {
        return 0;
    }

    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, FragmentShaderSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) {
        return program;
    }

    char log[1024] {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOG_ERROR("DRM display shader link failed: " << log);
    glDeleteProgram(program);
    return 0;
}

cv::Rect2d fitDestinationRect(cv::Size sourceSize, cv::Size canvasSize)
{
    const double sourceAspect = static_cast<double>(sourceSize.width) / sourceSize.height;
    const double canvasAspect = static_cast<double>(canvasSize.width) / canvasSize.height;

    double width = canvasSize.width;
    double height = canvasSize.height;
    if (sourceAspect > canvasAspect) {
        height = width / sourceAspect;
    } else {
        width = height * sourceAspect;
    }

    return cv::Rect2d(
        (canvasSize.width - width) * 0.5,
        (canvasSize.height - height) * 0.5,
        width,
        height);
}

void calculateRenderGeometry(
    cv::Size sourceSize,
    cv::Size canvasSize,
    DisplayMode displayMode,
    GLfloat vertices[16])
{
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = static_cast<double>(canvasSize.width);
    double y1 = static_cast<double>(canvasSize.height);
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = 1.0;
    double v1 = 1.0;

    if (displayMode == DisplayMode::Fit) {
        const cv::Rect2d destination = fitDestinationRect(sourceSize, canvasSize);
        x0 = destination.x;
        y0 = destination.y;
        x1 = destination.x + destination.width;
        y1 = destination.y + destination.height;
    } else if (displayMode == DisplayMode::Fill) {
        const double sourceAspect = static_cast<double>(sourceSize.width) / sourceSize.height;
        const double canvasAspect = static_cast<double>(canvasSize.width) / canvasSize.height;
        if (sourceAspect > canvasAspect) {
            const double visibleWidth = canvasAspect / sourceAspect;
            u0 = (1.0 - visibleWidth) * 0.5;
            u1 = 1.0 - u0;
        } else {
            const double visibleHeight = sourceAspect / canvasAspect;
            v0 = (1.0 - visibleHeight) * 0.5;
            v1 = 1.0 - v0;
        }
    }

    const auto toNdcX = [canvasSize](double x) {
        return static_cast<GLfloat>((x / canvasSize.width) * 2.0 - 1.0);
    };
    const auto toNdcY = [canvasSize](double y) {
        return static_cast<GLfloat>(1.0 - (y / canvasSize.height) * 2.0);
    };

    vertices[0] = toNdcX(x0);
    vertices[1] = toNdcY(y0);
    vertices[2] = static_cast<GLfloat>(u0);
    vertices[3] = static_cast<GLfloat>(v0);

    vertices[4] = toNdcX(x0);
    vertices[5] = toNdcY(y1);
    vertices[6] = static_cast<GLfloat>(u0);
    vertices[7] = static_cast<GLfloat>(v1);

    vertices[8] = toNdcX(x1);
    vertices[9] = toNdcY(y0);
    vertices[10] = static_cast<GLfloat>(u1);
    vertices[11] = static_cast<GLfloat>(v0);

    vertices[12] = toNdcX(x1);
    vertices[13] = toNdcY(y1);
    vertices[14] = static_cast<GLfloat>(u1);
    vertices[15] = static_cast<GLfloat>(v1);
}

void pageFlipHandler(int, unsigned int, unsigned int, unsigned int, void* data)
{
    *static_cast<bool*>(data) = false;
}

} // namespace

bool DrmKmsDisplayBackend::initialize(const DisplayBackendConfig& config)
{
    config_ = config;

    if (!openDrmDevice() || !initializeDrmMode()) {
        shutdown();
        return false;
    }

    if (!initializeEgl() || !initializeGl()) {
        LOG_WARNING("DRM/KMS GBM/EGL path is unavailable, falling back to DRM dumb buffers");
        if (texture_ != 0) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        if (program_ != 0) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        if (eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface_ != EGL_NO_SURFACE) {
                eglDestroySurface(eglDisplay_, eglSurface_);
            }
            if (eglContext_ != EGL_NO_CONTEXT) {
                eglDestroyContext(eglDisplay_, eglContext_);
            }
            eglTerminate(eglDisplay_);
        }
        if (gbmSurface_) {
            gbm_surface_destroy(gbmSurface_);
        }
        if (gbmDevice_) {
            gbm_device_destroy(gbmDevice_);
        }

        eglDisplay_ = EGL_NO_DISPLAY;
        eglSurface_ = EGL_NO_SURFACE;
        eglContext_ = EGL_NO_CONTEXT;
        gbmSurface_ = nullptr;
        gbmDevice_ = nullptr;

        if (!initializeDumbBuffers()) {
            shutdown();
            return false;
        }
        useDumbBuffers_ = true;
    }

    initialized_ = true;
    LOG_VERBOSE("Display backend initialized: drm");
    return true;
}

bool DrmKmsDisplayBackend::openDrmDevice()
{
    for (int index = 0; index < 8; ++index) {
        const std::string path = "/dev/dri/card" + std::to_string(index);
        drmFd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (drmFd_ < 0) {
            continue;
        }

        drmModeRes* resources = drmModeGetResources(drmFd_);
        bool hasConnectedDisplay = false;
        if (resources) {
            for (int connectorIndex = 0; connectorIndex < resources->count_connectors; ++connectorIndex) {
                drmModeConnector* connector = drmModeGetConnector(drmFd_, resources->connectors[connectorIndex]);
                if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
                    hasConnectedDisplay = true;
                }
                if (connector) {
                    drmModeFreeConnector(connector);
                }
                if (hasConnectedDisplay) {
                    break;
                }
            }
            drmModeFreeResources(resources);
        }

        if (hasConnectedDisplay) {
            LOG_INFO("Opening DRM/KMS display device: " << path);
            return true;
        }

        close(drmFd_);
        drmFd_ = -1;
    }

    LOG_ERROR("Failed to find a DRM/KMS device with a connected display under /dev/dri/card*");
    return false;
}

bool DrmKmsDisplayBackend::initializeDrmMode()
{
    drmModeRes* resources = drmModeGetResources(drmFd_);
    if (!resources) {
        LOG_ERROR(drmError("drmModeGetResources failed"));
        return false;
    }

    drmModeConnector* selectedConnector = nullptr;
    for (int index = 0; index < resources->count_connectors; ++index) {
        drmModeConnector* connector = drmModeGetConnector(drmFd_, resources->connectors[index]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            selectedConnector = connector;
            break;
        }
        if (connector) {
            drmModeFreeConnector(connector);
        }
    }

    if (!selectedConnector) {
        drmModeFreeResources(resources);
        LOG_ERROR("No connected DRM/KMS display connector with an active mode was found");
        return false;
    }

    connectorId_ = selectedConnector->connector_id;
    mode_ = selectedConnector->modes[0];
    for (int index = 0; index < selectedConnector->count_modes; ++index) {
        if ((selectedConnector->modes[index].type & DRM_MODE_TYPE_PREFERRED) != 0) {
            mode_ = selectedConnector->modes[index];
            break;
        }
    }

    drmModeEncoder* encoder = nullptr;
    if (selectedConnector->encoder_id != 0) {
        encoder = drmModeGetEncoder(drmFd_, selectedConnector->encoder_id);
    }
    if (!encoder) {
        for (int index = 0; index < selectedConnector->count_encoders; ++index) {
            encoder = drmModeGetEncoder(drmFd_, selectedConnector->encoders[index]);
            if (encoder) {
                break;
            }
        }
    }

    if (!encoder) {
        drmModeFreeConnector(selectedConnector);
        drmModeFreeResources(resources);
        LOG_ERROR("No DRM/KMS encoder available for the selected connector");
        return false;
    }

    crtcId_ = encoder->crtc_id != 0 ? encoder->crtc_id : resources->crtcs[0];
    originalCrtc_ = drmModeGetCrtc(drmFd_, crtcId_);

    LOG_INFO("DRM/KMS display mode: " << mode_.hdisplay << "x" << mode_.vdisplay << " @ " << mode_.vrefresh << " Hz");

    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(selectedConnector);
    drmModeFreeResources(resources);
    return true;
}

bool DrmKmsDisplayBackend::initializeEgl()
{
    gbmDevice_ = gbm_create_device(drmFd_);
    if (!gbmDevice_) {
        LOG_ERROR("gbm_create_device failed");
        return false;
    }

    const std::vector<uint32_t> formats = {
        GBM_FORMAT_XRGB8888,
        GBM_FORMAT_ARGB8888
    };
    const std::vector<uint32_t> usageFlags = {
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING,
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
        GBM_BO_USE_SCANOUT,
        GBM_BO_USE_RENDERING
    };

    for (const uint32_t format : formats) {
        for (const uint32_t usage : usageFlags) {
            gbmSurface_ = gbm_surface_create(
                gbmDevice_,
                mode_.hdisplay,
                mode_.vdisplay,
                format,
                usage);
            if (gbmSurface_) {
                LOG_VERBOSE("DRM/KMS GBM surface created: format=0x" << std::hex << format << std::dec << " usage=0x" << std::hex << usage << std::dec);
                break;
            }
        }
        if (gbmSurface_) {
            break;
        }
    }
    if (!gbmSurface_) {
        LOG_ERROR("gbm_surface_create failed");
        return false;
    }

    eglDisplay_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbmDevice_));
    if (eglDisplay_ == EGL_NO_DISPLAY || eglInitialize(eglDisplay_, nullptr, nullptr) != EGL_TRUE) {
        LOG_ERROR("eglInitialize failed");
        return false;
    }

    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig eglConfig = nullptr;
    EGLint configCount = 0;
    if (eglChooseConfig(eglDisplay_, configAttributes, &eglConfig, 1, &configCount) != EGL_TRUE || configCount == 0) {
        LOG_ERROR("eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    eglContext_ = eglCreateContext(eglDisplay_, eglConfig, EGL_NO_CONTEXT, contextAttributes);
    if (eglContext_ == EGL_NO_CONTEXT) {
        LOG_ERROR("eglCreateContext failed");
        return false;
    }

    eglSurface_ = eglCreateWindowSurface(
        eglDisplay_,
        eglConfig,
        reinterpret_cast<EGLNativeWindowType>(gbmSurface_),
        nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOG_ERROR("eglCreateWindowSurface failed");
        return false;
    }

    if (eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) != EGL_TRUE) {
        LOG_ERROR("eglMakeCurrent failed");
        return false;
    }

    return true;
}

bool DrmKmsDisplayBackend::initializeGl()
{
    program_ = createProgram();
    if (program_ == 0) {
        return false;
    }

    positionAttribute_ = glGetAttribLocation(program_, "aPosition");
    texCoordAttribute_ = glGetAttribLocation(program_, "aTexCoord");
    textureUniform_ = glGetUniformLocation(program_, "uTexture");
    if (positionAttribute_ < 0 || texCoordAttribute_ < 0 || textureUniform_ < 0) {
        LOG_ERROR("DRM display shader attributes could not be resolved");
        return false;
    }

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

bool DrmKmsDisplayBackend::initializeDumbBuffers()
{
    dumbBuffers_.resize(2);
    for (auto& buffer : dumbBuffers_) {
        drm_mode_create_dumb createRequest {};
        createRequest.width = mode_.hdisplay;
        createRequest.height = mode_.vdisplay;
        createRequest.bpp = 32;

        if (ioctl(drmFd_, DRM_IOCTL_MODE_CREATE_DUMB, &createRequest) != 0) {
            LOG_ERROR(drmError("DRM_IOCTL_MODE_CREATE_DUMB failed"));
            return false;
        }

        buffer.handle = createRequest.handle;
        buffer.pitch = createRequest.pitch;
        buffer.size = createRequest.size;

        if (drmModeAddFB(drmFd_, mode_.hdisplay, mode_.vdisplay, 24, 32, buffer.pitch, buffer.handle, &buffer.fbId) != 0) {
            LOG_ERROR(drmError("drmModeAddFB failed for DRM dumb buffer"));
            return false;
        }

        drm_mode_map_dumb mapRequest {};
        mapRequest.handle = buffer.handle;
        if (ioctl(drmFd_, DRM_IOCTL_MODE_MAP_DUMB, &mapRequest) != 0) {
            LOG_ERROR(drmError("DRM_IOCTL_MODE_MAP_DUMB failed"));
            return false;
        }

        buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd_, mapRequest.offset);
        if (buffer.map == MAP_FAILED) {
            buffer.map = nullptr;
            LOG_ERROR(drmError("mmap failed for DRM dumb buffer"));
            return false;
        }

        std::memset(buffer.map, 0, buffer.size);
    }

    LOG_INFO("DRM/KMS dumb-buffer display path initialized");
    return true;
}

bool DrmKmsDisplayBackend::updateTexture(const cv::Mat& frame)
{
    cv::Mat uploadFrame;
    if (frame.type() == CV_8UC3) {
        uploadFrame = frame.isContinuous() ? frame : frame.clone();
    } else {
        cv::cvtColor(frame, uploadFrame, cv::COLOR_GRAY2BGR);
    }

    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (textureSize_ != uploadFrame.size()) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGB,
            uploadFrame.cols,
            uploadFrame.rows,
            0,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            uploadFrame.data);
        textureSize_ = uploadFrame.size();
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            uploadFrame.cols,
            uploadFrame.rows,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            uploadFrame.data);
    }

    return glGetError() == GL_NO_ERROR;
}

bool DrmKmsDisplayBackend::render(const cv::Mat& frame)
{
    if (!initialized_) {
        LOG_ERROR("Display backend is not initialized");
        return false;
    }
    if (frame.empty()) {
        return true;
    }
    if (useDumbBuffers_) {
        return renderDumbBuffer(frame);
    }

    if (!updateTexture(frame)) {
        LOG_ERROR("DRM display texture upload failed");
        return false;
    }

    const cv::Size canvasSize(mode_.hdisplay, mode_.vdisplay);
    GLfloat vertices[16] {};
    calculateRenderGeometry(frame.size(), canvasSize, config_.displayMode, vertices);

    glViewport(0, 0, canvasSize.width, canvasSize.height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glUniform1i(textureUniform_, 0);

    glVertexAttribPointer(positionAttribute_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glEnableVertexAttribArray(positionAttribute_);
    glVertexAttribPointer(texCoordAttribute_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(texCoordAttribute_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (eglSwapBuffers(eglDisplay_, eglSurface_) != EGL_TRUE) {
        LOG_ERROR("eglSwapBuffers failed");
        return false;
    }

    if (!present()) {
        return false;
    }

    logDisplayState(frame);
    return !hasStdinQuitRequest();
}

bool DrmKmsDisplayBackend::renderDumbBuffer(const cv::Mat& frame)
{
    if (dumbBuffers_.empty()) {
        LOG_ERROR("DRM dumb buffers are not initialized");
        return false;
    }

    DumbBuffer& targetBuffer = dumbBuffers_[nextDumbBufferIndex_];
    const cv::Size canvasSize(mode_.hdisplay, mode_.vdisplay);
    const DisplayRenderResult displayResult = renderForDisplay(frame, canvasSize, config_.displayMode);

    cv::Mat bgrFrame;
    if (displayResult.frame.type() == CV_8UC3) {
        bgrFrame = displayResult.frame.isContinuous() ? displayResult.frame : displayResult.frame.clone();
    } else {
        cv::cvtColor(displayResult.frame, bgrFrame, cv::COLOR_GRAY2BGR);
    }

    auto* destinationBase = static_cast<unsigned char*>(targetBuffer.map);
    for (int y = 0; y < bgrFrame.rows; ++y) {
        const auto* source = bgrFrame.ptr<cv::Vec3b>(y);
        auto* destination = destinationBase + static_cast<std::size_t>(y) * targetBuffer.pitch;
        for (int x = 0; x < bgrFrame.cols; ++x) {
            destination[x * 4 + 0] = source[x][0];
            destination[x * 4 + 1] = source[x][1];
            destination[x * 4 + 2] = source[x][2];
            destination[x * 4 + 3] = 0;
        }
    }

    if (!presentDumbBuffer(targetBuffer)) {
        return false;
    }

    currentDumbBufferIndex_ = nextDumbBufferIndex_;
    nextDumbBufferIndex_ = (nextDumbBufferIndex_ + 1) % static_cast<int>(dumbBuffers_.size());
    logDisplayState(frame);
    return !hasStdinQuitRequest();
}

bool DrmKmsDisplayBackend::present()
{
    gbm_bo* bo = gbm_surface_lock_front_buffer(gbmSurface_);
    if (!bo) {
        LOG_ERROR("gbm_surface_lock_front_buffer failed");
        return false;
    }

    FrameBuffer nextFrameBuffer;
    nextFrameBuffer.bo = bo;
    const uint32_t handle = gbm_bo_get_handle(bo).u32;
    const uint32_t stride = gbm_bo_get_stride(bo);

    if (drmModeAddFB(drmFd_, mode_.hdisplay, mode_.vdisplay, 24, 32, stride, handle, &nextFrameBuffer.fbId) != 0) {
        LOG_ERROR(drmError("drmModeAddFB failed"));
        gbm_surface_release_buffer(gbmSurface_, bo);
        return false;
    }

    if (currentFrameBuffer_.fbId == 0) {
        if (drmModeSetCrtc(drmFd_, crtcId_, nextFrameBuffer.fbId, 0, 0, &connectorId_, 1, &mode_) != 0) {
            LOG_ERROR(drmError("drmModeSetCrtc failed"));
            logDrmMasterHint();
            destroyFrameBuffer(nextFrameBuffer);
            return false;
        }
    } else {
        bool waitingForFlip = true;
        if (drmModePageFlip(drmFd_, crtcId_, nextFrameBuffer.fbId, DRM_MODE_PAGE_FLIP_EVENT, &waitingForFlip) != 0) {
            LOG_ERROR(drmError("drmModePageFlip failed"));
            logDrmMasterHint();
            destroyFrameBuffer(nextFrameBuffer);
            return false;
        }

        drmEventContext eventContext {};
        eventContext.version = DRM_EVENT_CONTEXT_VERSION;
        eventContext.page_flip_handler = pageFlipHandler;
        while (waitingForFlip) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(drmFd_, &fds);
            if (select(drmFd_ + 1, &fds, nullptr, nullptr, nullptr) < 0) {
                LOG_ERROR(drmError("select failed while waiting for DRM page flip"));
                destroyFrameBuffer(nextFrameBuffer);
                return false;
            }
            drmHandleEvent(drmFd_, &eventContext);
        }
    }

    destroyFrameBuffer(currentFrameBuffer_);
    currentFrameBuffer_ = nextFrameBuffer;
    return true;
}

bool DrmKmsDisplayBackend::presentDumbBuffer(DumbBuffer& nextBuffer)
{
    if (currentDumbBufferIndex_ < 0) {
        if (drmModeSetCrtc(drmFd_, crtcId_, nextBuffer.fbId, 0, 0, &connectorId_, 1, &mode_) != 0) {
            LOG_ERROR(drmError("drmModeSetCrtc failed"));
            logDrmMasterHint();
            return false;
        }
        return true;
    }

    bool waitingForFlip = true;
    if (drmModePageFlip(drmFd_, crtcId_, nextBuffer.fbId, DRM_MODE_PAGE_FLIP_EVENT, &waitingForFlip) != 0) {
        LOG_ERROR(drmError("drmModePageFlip failed"));
        logDrmMasterHint();
        return false;
    }

    drmEventContext eventContext {};
    eventContext.version = DRM_EVENT_CONTEXT_VERSION;
    eventContext.page_flip_handler = pageFlipHandler;
    while (waitingForFlip) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(drmFd_, &fds);
        if (select(drmFd_ + 1, &fds, nullptr, nullptr, nullptr) < 0) {
            LOG_ERROR(drmError("select failed while waiting for DRM page flip"));
            return false;
        }
        drmHandleEvent(drmFd_, &eventContext);
    }

    return true;
}

void DrmKmsDisplayBackend::destroyFrameBuffer(FrameBuffer& frameBuffer)
{
    if (frameBuffer.fbId != 0 && drmFd_ >= 0) {
        drmModeRmFB(drmFd_, frameBuffer.fbId);
    }
    if (frameBuffer.bo && gbmSurface_) {
        gbm_surface_release_buffer(gbmSurface_, frameBuffer.bo);
    }
    frameBuffer = FrameBuffer {};
}

void DrmKmsDisplayBackend::destroyDumbBuffer(DumbBuffer& buffer)
{
    if (buffer.map) {
        munmap(buffer.map, buffer.size);
    }
    if (buffer.fbId != 0 && drmFd_ >= 0) {
        drmModeRmFB(drmFd_, buffer.fbId);
    }
    if (buffer.handle != 0 && drmFd_ >= 0) {
        drm_mode_destroy_dumb destroyRequest {};
        destroyRequest.handle = buffer.handle;
        ioctl(drmFd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyRequest);
    }
    buffer = DumbBuffer {};
}

void DrmKmsDisplayBackend::logDisplayState(const cv::Mat& frame)
{
    if (hasLoggedDisplayState_) {
        return;
    }

    LOG_VERBOSE("Display input frame: " << sizeToString(frame.size()));
    LOG_VERBOSE("Processing size: " << sizeToString(config_.processingSize));
    LOG_VERBOSE("Screen size: " << mode_.hdisplay << "x" << mode_.vdisplay);
    LOG_VERBOSE("Canvas size: " << mode_.hdisplay << "x" << mode_.vdisplay);
    LOG_VERBOSE("Display mode: " << displayModeName(config_.displayMode));
    LOG_VERBOSE("Display backend render path: " << (useDumbBuffers_ ? "DRM/KMS dumb buffers" : "DRM/KMS + GBM/EGL/GLES2"));
    hasLoggedDisplayState_ = true;
}

void DrmKmsDisplayBackend::shutdown()
{
    if (originalCrtc_ && drmFd_ >= 0) {
        drmModeSetCrtc(
            drmFd_,
            originalCrtc_->crtc_id,
            originalCrtc_->buffer_id,
            originalCrtc_->x,
            originalCrtc_->y,
            &connectorId_,
            1,
            &originalCrtc_->mode);
    }

    destroyFrameBuffer(currentFrameBuffer_);
    for (auto& buffer : dumbBuffers_) {
        destroyDumbBuffer(buffer);
    }
    dumbBuffers_.clear();

    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }

    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
        }
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
        }
        eglTerminate(eglDisplay_);
    }

    if (gbmSurface_) {
        gbm_surface_destroy(gbmSurface_);
    }
    if (gbmDevice_) {
        gbm_device_destroy(gbmDevice_);
    }
    if (originalCrtc_) {
        drmModeFreeCrtc(originalCrtc_);
    }
    if (drmFd_ >= 0) {
        close(drmFd_);
    }

    eglDisplay_ = EGL_NO_DISPLAY;
    eglSurface_ = EGL_NO_SURFACE;
    eglContext_ = EGL_NO_CONTEXT;
    gbmSurface_ = nullptr;
    gbmDevice_ = nullptr;
    originalCrtc_ = nullptr;
    drmFd_ = -1;
    nextDumbBufferIndex_ = 0;
    currentDumbBufferIndex_ = -1;
    useDumbBuffers_ = false;
    initialized_ = false;
}

#endif
