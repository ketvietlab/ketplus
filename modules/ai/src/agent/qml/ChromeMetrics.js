.pragma library

// Match InterfaceZoomMetrics::geometry; font tokens are already scaled.
function px(value, tokens) {
    if (value <= 1) return value;
    return Math.round(value * (tokens.zoomFactor || 1));
}
