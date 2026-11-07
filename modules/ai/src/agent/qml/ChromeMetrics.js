.pragma library
.import Ket 1.0 as Kit

// Existing product geometry calls use the same rounding and hairline rules as kit controls.
function px(value, tokens) {
    return Kit.KetTheme.px(value, tokens.zoomFactor || Kit.KetTheme.zoom)
}
