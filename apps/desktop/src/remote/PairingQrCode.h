#pragma once

#include <QImage>
#include <QString>

namespace ketplus {

[[nodiscard]] QImage makePairingQrCode(const QString& value, int pixels);

} // namespace ketplus
