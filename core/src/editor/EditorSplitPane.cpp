#include "editor/EditorSplitPane.h"

#include "editor/EditorWidget.h"

#include <ScintillaMessages.h>

#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

QString titleFor(const EditorWidget* source) {
    auto title = source->document().displayName();
    if (source->document().isModified()) {
        title.append(QStringLiteral(" •"));
    }
    return title;
}

} // namespace

EditorSplitPane::EditorSplitPane(QWidget* parent)
    : QWidget(parent), tabBar_(new QTabBar(this)), editor_(new EditorWidget(this)) {
    setObjectName(QStringLiteral("editorSplitPane"));
    editor_->setObjectName(QStringLiteral("splitEditor"));
    tabBar_->setObjectName(QStringLiteral("splitTabBar"));
    tabBar_->setDocumentMode(true);
    tabBar_->setExpanding(false);
    tabBar_->setMovable(true);
    tabBar_->setElideMode(Qt::ElideMiddle);
    tabBar_->setUsesScrollButtons(true);
    tabBar_->setFocusPolicy(Qt::NoFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabBar_);
    layout->addWidget(editor_, 1);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](const int index) {
        if (!switching_) {
            activateIndex(index);
        }
    });
}

EditorWidget* EditorSplitPane::editor() const noexcept { return editor_; }

EditorWidget* EditorSplitPane::currentSource() const { return current_; }

QTabBar* EditorSplitPane::tabBar() const noexcept { return tabBar_; }

QList<EditorWidget*> EditorSplitPane::sources() const {
    QList<EditorWidget*> result;
    for (int index = 0; index < tabBar_->count(); ++index) {
        if (auto* source = qvariant_cast<QPointer<EditorWidget>>(tabBar_->tabData(index)).data()) {
            result.append(source);
        }
    }
    return result;
}

bool EditorSplitPane::containsSource(const EditorWidget* source) const {
    return source != nullptr && indexOfSource(source) >= 0;
}

int EditorSplitPane::indexOfSource(const EditorWidget* source) const {
    for (int index = 0; index < tabBar_->count(); ++index) {
        if (qvariant_cast<QPointer<EditorWidget>>(tabBar_->tabData(index)).data() == source) {
            return index;
        }
    }
    return -1;
}

void EditorSplitPane::showSource(EditorWidget* source) {
    if (source == nullptr) {
        return;
    }
    int index = indexOfSource(source);
    if (index < 0) {
        switching_ = true;
        index = tabBar_->addTab(titleFor(source));
        tabBar_->setTabData(index, QVariant::fromValue(QPointer<EditorWidget>(source)));
        tabBar_->setTabToolTip(index, source->document().isUntitled()
                                          ? source->document().displayName()
                                          : source->document().filePath());
        addCloseButton(index, source);
        switching_ = false;
    }
    if (tabBar_->currentIndex() == index) {
        activateIndex(index);
    } else {
        tabBar_->setCurrentIndex(index);
    }
}

void EditorSplitPane::addCloseButton(const int index, EditorWidget* source) {
    auto* button = new QToolButton(tabBar_);
    button->setText(QString::fromUtf8("×"));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(QStringLiteral("Close in split"));
    button->setAccessibleName(
        QStringLiteral("Close %1 in split").arg(source->document().displayName()));
    button->setProperty("kvRole", QStringLiteral("tabClose"));
    const QPointer<EditorWidget> guarded(source);
    connect(button, &QToolButton::clicked, this, [this, guarded] {
        if (guarded != nullptr) {
            removeSource(guarded);
        }
    });
    tabBar_->setTabButton(index, QTabBar::RightSide, button);
}

void EditorSplitPane::removeSource(EditorWidget* source) {
    const int index = indexOfSource(source);
    if (index < 0) {
        return;
    }
    viewStates_.remove(source);
    const bool wasCurrent = source == current_;
    if (wasCurrent) {
        current_ = nullptr;
    }
    switching_ = true;
    tabBar_->removeTab(index);
    switching_ = false;
    if (tabBar_->count() == 0) {
        emit emptied();
        return;
    }
    if (wasCurrent) {
        activateIndex(tabBar_->currentIndex());
    }
}

void EditorSplitPane::refreshCurrentSource() {
    if (current_ == nullptr) {
        return;
    }
    editor_->shareDocumentWith(*current_);
    emit currentSourceChanged(current_);
}

void EditorSplitPane::updateSourceTitle(EditorWidget* source) {
    const int index = indexOfSource(source);
    if (index >= 0) {
        tabBar_->setTabText(index, titleFor(source));
        tabBar_->setTabToolTip(index, source->document().isUntitled()
                                          ? source->document().displayName()
                                          : source->document().filePath());
    }
}

void EditorSplitPane::activateIndex(const int index) {
    auto* source = qvariant_cast<QPointer<EditorWidget>>(tabBar_->tabData(index)).data();
    if (source == nullptr) {
        return;
    }
    if (source != current_) {
        saveViewState();
        current_ = source;
        editor_->shareDocumentWith(*source);
        restoreViewState(source);
    }
    emit currentSourceChanged(source);
}

void EditorSplitPane::saveViewState() {
    if (current_ == nullptr) {
        return;
    }
    viewStates_.insert(current_.data(),
                       {.caret = editor_->send(message(Scintilla::Message::GetCurrentPos)),
                        .anchor = editor_->send(message(Scintilla::Message::GetAnchor)),
                        .firstVisibleLine =
                            editor_->send(message(Scintilla::Message::GetFirstVisibleLine))});
}

void EditorSplitPane::restoreViewState(EditorWidget* source) {
    // A new tab starts where the source view is, like opening the same file twice.
    const ViewState state = viewStates_.value(
        source, {.caret = source->send(message(Scintilla::Message::GetCurrentPos)),
                 .anchor = source->send(message(Scintilla::Message::GetAnchor)),
                 .firstVisibleLine =
                     source->send(message(Scintilla::Message::GetFirstVisibleLine))});
    editor_->send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(state.anchor),
                  static_cast<sptr_t>(state.caret));
    editor_->send(message(Scintilla::Message::SetFirstVisibleLine),
                  static_cast<uptr_t>(state.firstVisibleLine));
}

} // namespace ketplus
