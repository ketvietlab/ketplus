#include "app/PixelSelect.h"

#include "KetSelect.h"

#include <QHBoxLayout>
#include <QSignalBlocker>

namespace ketplus {

PixelSelect::PixelSelect(QWidget* parent) : QWidget(parent), select_(new ket::Select(this)) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(select_);
    setFocusProxy(select_);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    select_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRange(0, 99);
    connect(select_, &QComboBox::currentIndexChanged, this, [this] { emit valueChanged(value()); });
}

int PixelSelect::value() const { return select_->currentData().toInt(); }

void PixelSelect::setRange(int minimum, int maximum) {
    const int previous = value();
    minimum_ = minimum;
    maximum_ = qMax(minimum, maximum);
    {
        const QSignalBlocker blocker(select_);
        select_->clear();
        for (int pixels = minimum_; pixels <= maximum_; ++pixels)
            select_->addItem(tr("%1 px").arg(pixels), pixels);
        select_->setCurrentIndex(qBound(minimum_, previous, maximum_) - minimum_);
    }
    if (value() != previous)
        emit valueChanged(value());
    updateGeometry();
}

void PixelSelect::setMinimum(int minimum) { setRange(minimum, maximum_); }

void PixelSelect::setValue(int value) {
    select_->setCurrentIndex(qBound(minimum_, value, maximum_) - minimum_);
}

QSize PixelSelect::sizeHint() const { return select_->sizeHint(); }
QSize PixelSelect::minimumSizeHint() const { return select_->minimumSizeHint(); }

} // namespace ketplus
