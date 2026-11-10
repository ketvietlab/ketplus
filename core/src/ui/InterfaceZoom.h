#pragma once

#include "KetAppearance.h"

#include <QList>
#include <QtGlobal>

namespace ketplus {

// Product chrome zoom (ADR 0104). Users pick one of the steps; geometry, kit widgets and
// legacy product text scale by the step's percent. The interface font size is separate.
struct InterfaceZoomMetrics final {
    static constexpr int defaultPercent = 100;
    static constexpr int minimumPercent = 50;
    static constexpr int maximumPercent = 200;
    // The size the product's designed pixels and the former Medium step were drawn at.
    static constexpr int designedAtPixels = 14;

    [[nodiscard]] static QList<int> steps() { return {90, 100, 110, 125}; }

    // The step closest to a percent; ties go to the smaller step.
    [[nodiscard]] static int nearestStep(const double percent) {
        int nearest = defaultPercent;
        double distance = -1;
        for (const int step : steps()) {
            const double current = qAbs(step - percent);
            if (distance < 0 || current < distance) {
                nearest = step;
                distance = current;
            }
        }
        return nearest;
    }

    // Before ADR 0104 the interface pixels were the zoom: 12/14/16 for Small/Medium/Large.
    [[nodiscard]] static int fromLegacyPixels(const int pixels) {
        return nearestStep(100.0 * qBound(8, pixels, 24) / designedAtPixels);
    }

    // The zoom ThemeManager applies. It is set before the application font changes, so a
    // surface that restyles on that change reads the new zoom; the kit's follows later.
    [[nodiscard]] static int current();

    [[nodiscard]] static int snap(const int percent) {
        return qBound(minimumPercent, percent, maximumPercent);
    }

    // Geometry has no text legibility floor: zero stays zero, hairlines stay one.
    [[nodiscard]] static double factor(const int percent) { return snap(percent) / 100.0; }
    [[nodiscard]] static int geometry(const int designedPixels, const int percent) {
        return qRound(ket::Appearance::scale(designedPixels, factor(percent)));
    }

    // Text alone has a minimum readable size.
    [[nodiscard]] static int scale(const int designedPixels, const int percent) {
        return qMax(8, geometry(designedPixels, percent));
    }
};

} // namespace ketplus
