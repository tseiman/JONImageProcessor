#include "HtmlMediaRenderer.h"

#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(JON_ENABLE_WPE_HTML_RENDERER)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gio/gio.h>
#include <glib.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

#include <atomic>
#include <mutex>
#endif

namespace {

#if defined(JON_ENABLE_WPE_HTML_RENDERER)

using GlEGLImageTargetTexture2DOES = void (*)(GLenum, GLeglImageOES);

std::string fileUri(const std::string& path)
{
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (!uri) {
        if (error) g_error_free(error);
        return {};
    }
    std::string result(uri);
    g_free(uri);
    return result;
}

void iterateMainContext()
{
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

#endif

} // namespace

#if defined(JON_ENABLE_WPE_HTML_RENDERER)

struct HtmlMediaRenderer::Impl {
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    WebKitWebView* webView = nullptr;
    WebKitWebViewBackend* webViewBackend = nullptr;
    wpe_view_backend_exportable_fdo* exportable = nullptr;
    cv::Size size;
    cv::Mat latestFrame;
    std::mutex frameMutex;
    std::atomic<bool> loaded {false};
    std::atomic<bool> haveFrame {false};
    GlEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES = nullptr;

    ~Impl()
    {
        reset();
    }

    void reset()
    {
        iterateMainContext();
        if (webView) {
            g_object_unref(webView);
            webView = nullptr;
        }
        if (webViewBackend) {
            g_object_unref(webViewBackend);
            webViewBackend = nullptr;
        }
        if (exportable) {
            wpe_view_backend_exportable_fdo_destroy(exportable);
            exportable = nullptr;
        }
        if (eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface != EGL_NO_SURFACE) {
                eglDestroySurface(eglDisplay, eglSurface);
                eglSurface = EGL_NO_SURFACE;
            }
            if (eglContext != EGL_NO_CONTEXT) {
                eglDestroyContext(eglDisplay, eglContext);
                eglContext = EGL_NO_CONTEXT;
            }
            eglTerminate(eglDisplay);
            eglDisplay = EGL_NO_DISPLAY;
        }
        latestFrame.release();
        loaded = false;
        haveFrame = false;
    }

    bool initializeEgl(std::string& error)
    {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL_NO_DISPLAY || !eglInitialize(eglDisplay, nullptr, nullptr)) {
            using EglGetPlatformDisplayExt = EGLDisplay (*)(EGLenum, void*, const EGLint*);
            auto* getPlatformDisplay = reinterpret_cast<EglGetPlatformDisplayExt>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (getPlatformDisplay) {
                eglDisplay = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            }
        }
        if (eglDisplay == EGL_NO_DISPLAY || !eglInitialize(eglDisplay, nullptr, nullptr)) {
            error = "failed to initialize EGL display for WPE HTML renderer";
            return false;
        }

        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint configCount = 0;
        if (!eglChooseConfig(eglDisplay, configAttributes, &eglConfig, 1, &configCount) || configCount < 1) {
            error = "failed to choose EGL config for WPE HTML renderer";
            return false;
        }

        const EGLint surfaceAttributes[] = {
            EGL_WIDTH, std::max(1, size.width),
            EGL_HEIGHT, std::max(1, size.height),
            EGL_NONE
        };
        eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, surfaceAttributes);
        if (eglSurface == EGL_NO_SURFACE) {
            error = "failed to create EGL pbuffer for WPE HTML renderer";
            return false;
        }

        const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes);
        if (eglContext == EGL_NO_CONTEXT || !eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            error = "failed to create EGL context for WPE HTML renderer";
            return false;
        }

        glEGLImageTargetTexture2DOES = reinterpret_cast<GlEGLImageTargetTexture2DOES>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!glEGLImageTargetTexture2DOES) {
            error = "EGL image import extension is unavailable for WPE HTML renderer";
            return false;
        }

        if (!wpe_fdo_initialize_for_egl_display(eglDisplay)) {
            error = "failed to initialize WPEBackend-fdo for EGL display";
            return false;
        }
        return true;
    }

    static void exportEglImage(void* data, EGLImageKHR image)
    {
        auto* self = static_cast<Impl*>(data);
        if (image) {
            self->copyEglImage(image, self->size.width, self->size.height);
            wpe_view_backend_exportable_fdo_egl_dispatch_release_image(self->exportable, image);
        }
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
    }

    static void exportFdoEglImage(void* data, wpe_fdo_egl_exported_image* image)
    {
        auto* self = static_cast<Impl*>(data);
        if (image) {
            const auto width = static_cast<int>(wpe_fdo_egl_exported_image_get_width(image));
            const auto height = static_cast<int>(wpe_fdo_egl_exported_image_get_height(image));
            self->copyEglImage(wpe_fdo_egl_exported_image_get_egl_image(image), width, height);
            wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, image);
        }
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
    }

    static void exportShmBuffer(void*, wpe_fdo_shm_exported_buffer*)
    {
    }

    void copyEglImage(EGLImageKHR image, int width, int height)
    {
        if (!image || width <= 0 || height <= 0 || eglDisplay == EGL_NO_DISPLAY) {
            return;
        }
        eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

        GLuint texture = 0;
        GLuint framebuffer = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            cv::Mat rgba(height, width, CV_8UC4);
            glViewport(0, 0, width, height);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data);
            cv::flip(rgba, rgba, 0);
            cv::Mat bgr;
            cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame = std::move(bgr);
            haveFrame = true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
    }

    bool load(const std::string& path, const cv::Size& nextSize, std::string& error)
    {
        reset();
        size = nextSize;
        if (size.width <= 0 || size.height <= 0) {
            error = "invalid WPE HTML render size";
            return false;
        }
        if (!initializeEgl(error)) {
            return false;
        }
        if (!wpe_loader_init("libWPEBackend-fdo-1.0.so")) {
            error = "failed to load WPEBackend-fdo runtime library";
            return false;
        }

        static const wpe_view_backend_exportable_fdo_egl_client client {
            exportEglImage,
            exportFdoEglImage,
            exportShmBuffer,
            nullptr,
            nullptr
        };
        exportable = wpe_view_backend_exportable_fdo_egl_create(&client, this, static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height));
        if (!exportable) {
            error = "failed to create WPEBackend-fdo exportable EGL view";
            return false;
        }

        webViewBackend = webkit_web_view_backend_new(wpe_view_backend_exportable_fdo_get_view_backend(exportable), nullptr, nullptr);
        webView = webkit_web_view_new(webViewBackend);
        if (!webView) {
            error = "failed to create WPE WebView";
            return false;
        }

        WebKitColor transparent {0, 0, 0, 0};
        webkit_web_view_set_background_color(webView, &transparent);

        const std::string uri = fileUri(path);
        if (uri.empty()) {
            error = "failed to create file URI for HTML media";
            return false;
        }
        webkit_web_view_load_uri(webView, uri.c_str());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!haveFrame && std::chrono::steady_clock::now() < deadline) {
            iterateMainContext();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        loaded = true;
        return true;
    }

    bool render(cv::Mat& frame, std::string&)
    {
        iterateMainContext();
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!latestFrame.empty()) {
            frame = latestFrame;
            return true;
        }
        return false;
    }
};

#else

struct HtmlMediaRenderer::Impl {
};

#endif

HtmlMediaRenderer::HtmlMediaRenderer()
    : impl_(std::make_unique<Impl>())
{
}

HtmlMediaRenderer::~HtmlMediaRenderer() = default;

bool HtmlMediaRenderer::supported()
{
#if defined(JON_ENABLE_WPE_HTML_RENDERER)
    return true;
#else
    return false;
#endif
}

bool HtmlMediaRenderer::load(const std::string& path, const cv::Size& size, std::string& error)
{
#if defined(JON_ENABLE_WPE_HTML_RENDERER)
    return impl_->load(path, size, error);
#else
    (void)path;
    (void)size;
    error = "HTML media is not supported in this build";
    return false;
#endif
}

bool HtmlMediaRenderer::render(cv::Mat& frame, std::string& error)
{
#if defined(JON_ENABLE_WPE_HTML_RENDERER)
    return impl_->render(frame, error);
#else
    (void)frame;
    error = "HTML media is not supported in this build";
    return false;
#endif
}

void HtmlMediaRenderer::reset()
{
#if defined(JON_ENABLE_WPE_HTML_RENDERER)
    impl_->reset();
#endif
}
