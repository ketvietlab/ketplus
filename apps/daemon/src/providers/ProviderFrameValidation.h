#pragma once

#include <QByteArrayView>

namespace ketplus::daemon {

// Qt's JSON decoder accepts raw control bytes in strings. Protocol frames must
// use JSON escapes there; only JSON whitespace is allowed outside strings.
[[nodiscard]] inline bool providerFrameHasValidControlBytes(QByteArrayView frame) {
    bool inString = false;
    bool escaped = false;
    for (const unsigned char byte : frame) {
        if (byte < 0x20 && (inString || (byte != '\t' && byte != '\r' && byte != '\n')))
            return false;
        if (inString && escaped) {
            escaped = false;
        } else if (inString && byte == '\\') {
            escaped = true;
        } else if (byte == '"') {
            inString = !inString;
        }
    }
    return true;
}

} // namespace ketplus::daemon
