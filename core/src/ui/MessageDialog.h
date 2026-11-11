#pragma once
#include <QList>
#include <QMessageBox>

#include <optional>
namespace ketplus::message_dialog {
struct Choice {
    QMessageBox::StandardButton value;
    QString text;
    bool destructive = false;
};
// Synchronous adapter for the existing document commands; presentation is the kit dialog.
QMessageBox::StandardButton choose(QWidget* parent, const QString& title, const QString& text,
                                   const QList<Choice>& choices,
                                   QMessageBox::StandardButton defaultButton = QMessageBox::Cancel);
QMessageBox::StandardButton warning(QWidget*, const QString&, const QString&,
                                    QMessageBox::StandardButtons = QMessageBox::Ok,
                                    QMessageBox::StandardButton = QMessageBox::NoButton);
QMessageBox::StandardButton critical(QWidget*, const QString&, const QString&,
                                     QMessageBox::StandardButtons = QMessageBox::Ok,
                                     QMessageBox::StandardButton = QMessageBox::NoButton);
QMessageBox::StandardButton information(QWidget*, const QString&, const QString&,
                                        QMessageBox::StandardButtons = QMessageBox::Ok,
                                        QMessageBox::StandardButton = QMessageBox::NoButton);
QMessageBox::StandardButton question(QWidget*, const QString&, const QString&,
                                     QMessageBox::StandardButtons = QMessageBox::Yes |
                                                                    QMessageBox::No,
                                     QMessageBox::StandardButton = QMessageBox::NoButton);
// A whole number from `minimum` to `maximum`, such as Go to Line; empty when cancelled.
std::optional<int> number(QWidget* parent, const QString& title, const QString& label, int value,
                          int minimum, int maximum, const QString& acceptText);
} // namespace ketplus::message_dialog
