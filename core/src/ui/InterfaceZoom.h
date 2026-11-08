#pragma once

#include "KetAppearance.h"

#include <QtGlobal>

namespace ketplus {

// Product chrome zoom. Users pick a named size; pixels stay an internal
// mapping onto the shared AppearanceSettings.interfaceFontSizePixels field.
enum class InterfaceZoom { Small, Medium, Large };

struct InterfaceZoomMetrics final {
    static constexpr int smallPixels = 12;
    static constexpr int mediumPixels = 14;
    static constexpr int largePixels = 16;
    static constexpr int designedAtPixels = mediumPixels;

    [[nodiscard]] static InterfaceZoom fromPixels(const int pixels) {
        if (pixels <= 13)
            return InterfaceZoom::Small;
        if (pixels >= 15)
            return InterfaceZoom::Large;
        return InterfaceZoom::Medium;
    }

    [[nodiscard]] static int pixels(const InterfaceZoom zoom) {
        switch (zoom) {
        case InterfaceZoom::Small:
            return smallPixels;
        case InterfaceZoom::Large:
            return largePixels;
        case InterfaceZoom::Medium:
            break;
        }
        return mediumPixels;
    }

    [[nodiscard]] static int snap(const int storedPixels) {
        return pixels(fromPixels(storedPixels));
    }

    // Geometry has no text legibility floor: zero stays zero, hairlines stay one.
    [[nodiscard]] static double factor(const int basePixels) {
        return double(snap(basePixels)) / designedAtPixels;
    }
    [[nodiscard]] static int geometry(const int designedPixels, const int basePixels) {
        return qRound(ket::Appearance::scale(designedPixels, factor(basePixels)));
    }

    // Text alone has a minimum readable size.
    [[nodiscard]] static int scale(const int designedPixels, const int basePixels) {
        return qMax(8, geometry(designedPixels, basePixels));
    }
};

} // namespace ketplus
