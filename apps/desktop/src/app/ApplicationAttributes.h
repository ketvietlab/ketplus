#pragma once

#include <QCoreApplication>

namespace ketplus {

// Called before the QApplication exists, by main() and by tests that build a MainWindow.
inline void applyApplicationAttributes() {
    // A widget that gets its own native window (the Atlas builder chat, an Atlas web view, a
    // window container) otherwise makes all its siblings native too. The page holding the Agent
    // Workspace's QQuickView then gets a native window above the view's, and the conversation
    // and the other pages draw blank after an Atlas has been opened. KetPlus lays pages side by
    // side, never overlapping, so it needs no native siblings; ket-qt makes its overlays native
    // itself.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
}

} // namespace ketplus
