#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace reader {
struct Size { double width, height; };
struct Point { double x, y; };
struct Rect { double x, y, width, height; };
struct Anchor { size_t page = 0; double x = .5, y = 0; };

// Coordinates are physical client pixels; every page fits the width independently.
class Layout {
public:
    std::vector<Size> pages;
    std::vector<double> tops;
    double width = 1, height = 1, zoom = 1, gap = 6;
    double scrollX = 0, scrollY = 0, totalHeight = 0;
    static constexpr double minZoom = .25, maxZoom = 8;

    double pageWidth() const { return width * zoom; }
    double pageLeft() const { return std::max(0.0, (width - pageWidth()) / 2); }
    double maxX() const { return std::max(0.0, pageWidth() - width); }
    double maxY() const { return std::max(0.0, totalHeight - height); }
    void clamp() {
        scrollX = std::clamp(scrollX, 0.0, maxX());
        scrollY = std::clamp(scrollY, 0.0, maxY());
    }
    void reflow() {
        tops.clear(); totalHeight = 0;
        for (const auto& p : pages) {
            tops.push_back(totalHeight);
            totalHeight += pageWidth() * p.height / p.width + gap;
        }
        if (!pages.empty()) totalHeight -= gap;
        clamp();
    }
    size_t pageAt(double y) const {
        if (tops.empty()) return 0;
        auto i = std::upper_bound(tops.begin(), tops.end(), y);
        return i == tops.begin() ? 0 : size_t(i - tops.begin() - 1);
    }
    Rect rect(size_t i) const {
        return {pageLeft() - scrollX, tops.at(i) - scrollY,
                pageWidth(), pageWidth() * pages.at(i).height / pages.at(i).width};
    }
    Anchor capture(Point point) const {
        if (pages.empty()) return {};
        const size_t i = pageAt(point.y + scrollY);
        return {i, (point.x + scrollX - pageLeft()) / pageWidth(),
            (point.y + scrollY - tops[i]) /
            (pageWidth() * pages[i].height / pages[i].width)};
    }
    void restore(Anchor a, Point point) {
        if (pages.empty()) return;
        a.page = std::min(a.page, pages.size() - 1);
        scrollX = pageLeft() + a.x * pageWidth() - point.x;
        scrollY = tops[a.page] + a.y * pageWidth() *
                  pages[a.page].height / pages[a.page].width - point.y;
        clamp();
    }
    void zoomAt(double value, Point point) {
        if (!std::isfinite(value)) return;
        const auto anchor = capture(point);
        zoom = std::clamp(value, minZoom, maxZoom);
        reflow(); restore(anchor, point);
    }
    void resize(double w, double h) {
        const auto anchor = capture({width / 2, height / 2});
        width = std::max(1.0, w); height = std::max(1.0, h);
        reflow(); restore(anchor, {width / 2, height / 2});
    }
    void pan(double dx, double dy) { scrollX += dx; scrollY += dy; clamp(); }
    std::vector<size_t> visible(double margin = 0) const {
        std::vector<size_t> result;
        if (pages.empty()) return result;
        for (size_t i = pageAt(std::max(0.0, scrollY - margin)); i < pages.size(); ++i) {
            if (tops[i] > scrollY + height + margin) break;
            result.push_back(i);
        }
        return result;
    }
};
inline Size rasterSize(Size page, double width) {
    double height = width * page.height / page.width;
    constexpr double budget = 16000000.0;
    const double factor = std::min({1.0, 8192.0 / width, 8192.0 / height,
                                   std::sqrt(budget / (width * height))});
    return {std::max(1.0, std::floor(width * factor)),
            std::max(1.0, std::floor(height * factor))};
}
}
