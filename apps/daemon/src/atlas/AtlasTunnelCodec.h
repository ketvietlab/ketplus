#pragma once
#include <QByteArray>
#include <QJsonObject>

namespace ketplus::daemon {
struct AtlasTunnelFrame {
    QJsonObject header;
    QByteArray body;
};
class AtlasTunnelCodec {
  public:
    AtlasTunnelCodec(QByteArray key, QString channel, bool daemon);
    QByteArray seal(const AtlasTunnelFrame& frame);
    bool open(const QByteArray& wire, AtlasTunnelFrame* frame);
    static QByteArray randomKey();

  private:
    QByteArray key_;
    QString channel_;
    bool daemon_;
    quint64 sent_{0}, received_{0};
};
} // namespace ketplus::daemon
