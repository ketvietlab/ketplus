#pragma once

#include <QStringList>

namespace ketplus {

// Registers every font under :/ketplus/fonts (KÉT Sans and KÉT Sans Mono, compiled in by
// ketplus_cm_add_ket_fonts). Call once, before the kit or a window reads a font family. Returns
// the families that were registered, e.g. "KET Sans" and "KET Sans Mono".
QStringList registerKetFonts();

} // namespace ketplus
