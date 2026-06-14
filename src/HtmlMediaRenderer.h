#pragma once

#include <opencv2/core.hpp>

#include <memory>
#include <string>

class HtmlMediaRenderer {
public:
    HtmlMediaRenderer();
    ~HtmlMediaRenderer();

    HtmlMediaRenderer(const HtmlMediaRenderer&) = delete;
    HtmlMediaRenderer& operator=(const HtmlMediaRenderer&) = delete;

    bool load(const std::string& path, const cv::Size& size, std::string& error);
    bool render(cv::Mat& frame, std::string& error);
    void reset();

    static bool supported();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
