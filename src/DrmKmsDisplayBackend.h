#pragma once

#include "IDisplayBackend.h"

#if defined(JON_ENABLE_DRM_DISPLAY)

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <string>

class DrmKmsDisplayBackend : public IDisplayBackend {
public:
    bool initialize(const DisplayBackendConfig& config) override;
    bool render(const cv::Mat& frame) override;
    void shutdown() override;

private:
    struct FrameBuffer {
        gbm_bo* bo = nullptr;
        uint32_t fbId = 0;
    };

    bool openDrmDevice();
    bool initializeDrmMode();
    bool initializeEgl();
    bool initializeGl();
    bool updateTexture(const cv::Mat& frame);
    bool present();
    void destroyFrameBuffer(FrameBuffer& frameBuffer);
    void logDisplayState(const cv::Mat& frame);

    DisplayBackendConfig config_;
    int drmFd_ = -1;
    uint32_t connectorId_ = 0;
    uint32_t crtcId_ = 0;
    drmModeModeInfo mode_ {};
    drmModeCrtc* originalCrtc_ = nullptr;

    gbm_device* gbmDevice_ = nullptr;
    gbm_surface* gbmSurface_ = nullptr;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    GLuint program_ = 0;
    GLuint texture_ = 0;
    GLint positionAttribute_ = -1;
    GLint texCoordAttribute_ = -1;
    GLint textureUniform_ = -1;
    cv::Size textureSize_;

    FrameBuffer currentFrameBuffer_;
    bool initialized_ = false;
    bool hasLoggedDisplayState_ = false;
};

#endif
