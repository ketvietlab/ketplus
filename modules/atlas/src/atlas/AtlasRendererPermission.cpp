#include "atlas/AtlasRendererPermission.h"

#include "KetButton.h"
#include "KetDialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScopeGuard>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardPaths>
#include <QVBoxLayout>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#endif

namespace ketplus {
namespace {
QByteArray readBounded(const QString& path, qint64 limit) {
    QFile file;
#ifdef Q_OS_UNIX
    // A pathname check alone races replacement with a FIFO before open().
    const int descriptor = ::open(QFile::encodeName(path).constData(),
                                  O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (descriptor < 0)
        return {};
    auto close = qScopeGuard([descriptor] { ::close(descriptor); });
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        metadata.st_size > limit)
        return {};
#elif defined(Q_OS_WIN)
    const auto nativePath = QDir::toNativeSeparators(path);
    const auto handle =
        CreateFileW(reinterpret_cast<const wchar_t*>(nativePath.utf16()), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    auto closeHandle = qScopeGuard([handle] { CloseHandle(handle); });
    BY_HANDLE_FILE_INFORMATION metadata{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &metadata) ||
        (metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        metadata.nFileSizeHigh || metadata.nFileSizeLow > quint64(limit))
        return {};
    const int descriptor =
        _open_osfhandle(reinterpret_cast<qintptr>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0)
        return {};
    closeHandle.dismiss();
    auto close = qScopeGuard([descriptor] { _close(descriptor); });
#else
    return {}; // An unsupported file API cannot grant renderer execution.
#endif
#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
        return {};
    close.dismiss();
    const auto bytes = file.read(limit + 1);
    return file.error() == QFileDevice::NoError && bytes.size() <= limit ? bytes : QByteArray{};
#endif
}
QString quoted(const QString& value) {
    QString visible;
    for (const auto character : value) {
        if (character.category() == QChar::Other_Format ||
            character.category() == QChar::Other_Control)
            visible +=
                QStringLiteral("\\u%1").arg(uint(character.unicode()), 4, 16, QLatin1Char('0'));
        else
            visible += character;
    }
    const auto array = QJsonDocument(QJsonArray{visible}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}
} // namespace

AtlasRendererPermission AtlasRendererPermission::read(const AtlasDescriptor& atlas,
                                                      const QProcessEnvironment& environment,
                                                      quint16 port) {
    AtlasRendererPermission result;
    const QFileInfo contract(atlas.directory + QStringLiteral("/atlas.renderer.json"));
    result.present = contract.exists() || contract.isSymLink();
    if (!result.present)
        return result;
    const auto invalid = [&] {
        result.error = QObject::tr("The renderer command could not be verified. Opening static "
                                   "content without running the renderer.");
        return result;
    };
    result.manifestPath = QFileInfo(atlas.path).canonicalFilePath();
    result.contractPath = contract.canonicalFilePath();
    result.manifest = readBounded(result.manifestPath, AtlasIndex::maximumManifestBytes);
    result.contract = readBounded(result.contractPath, 1024 * 1024);
    if (result.manifest.isEmpty() || result.contract.isEmpty())
        return invalid();
    const auto object = QJsonDocument::fromJson(result.contract).object();
    if (object.value(QStringLiteral("version")).toInt() != 1 ||
        !object.value(QStringLiteral("command")).isArray())
        return invalid();
    const auto directory = QFileInfo(result.contractPath).absolutePath();
    const auto cwd = object.value(QStringLiteral("cwd"));
    if (!cwd.isUndefined() && (!cwd.isString() || cwd.toString().trimmed().isEmpty()))
        return invalid();
    result.workingDirectory =
        QFileInfo(
            QDir(directory).filePath(cwd.isUndefined() ? QStringLiteral(".") : cwd.toString()))
            .canonicalFilePath();
    if (result.workingDirectory.isEmpty() || !QFileInfo(result.workingDirectory).isDir())
        return invalid();
    const auto arguments = object.value(QStringLiteral("command")).toArray();
    if (arguments.isEmpty() || arguments.size() > 128)
        return invalid();
    for (const auto& value : arguments) {
        if (!value.isString() || value.toString().trimmed().isEmpty() ||
            value.toString().size() > 4096 || value.toString().contains(QChar::Null))
            return invalid();
        // The CLI substitutes these three placeholders before spawning without a shell.
        auto argument = value.toString();
        argument.replace(QStringLiteral("{host}"), QStringLiteral("127.0.0.1"));
        argument.replace(QStringLiteral("{port}"), QString::number(port));
        argument.replace(QStringLiteral("{atlasDirectory}"), directory);
        result.command.append(argument);
    }
    auto executable = result.command.first();
    if (executable.contains(QLatin1Char('/')) || executable.contains(QLatin1Char('\\')))
        executable = QDir(result.workingDirectory).absoluteFilePath(executable);
    else {
        auto paths = environment.value(QStringLiteral("PATH")).split(QDir::listSeparator());
        for (auto& path : paths)
            path = QDir(result.workingDirectory).absoluteFilePath(path.isEmpty() ? "." : path);
        executable = QStandardPaths::findExecutable(executable, paths);
    }
    result.executable = QFileInfo(executable).canonicalFilePath();
    if (result.executable.isEmpty() || !QFileInfo(result.executable).isExecutable())
        return invalid();
    return result;
}

ket::Dialog* rendererPermissionDialog(QWidget* anchor, const AtlasRendererPermission& permission) {
    auto* dialog = new ket::Dialog(QObject::tr("Run Atlas renderer?"), anchor);
    dialog->setObjectName(QStringLiteral("atlasRendererPermission"));
    dialog->setCardWidth(640);
    auto* body = new QWidget(dialog);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* description = new QLabel(
        QObject::tr("This project wants to run a local program with your account's access. "
                    "Allow it for this launch only, or view static content without running it."),
        body);
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    layout->addWidget(description);
    QString details = QObject::tr("Executable: %1\nWorking directory: %2\nContract: %3\nArguments:")
                          .arg(quoted(permission.executable), quoted(permission.workingDirectory),
                               quoted(permission.contractPath));
    for (qsizetype i = 1; i < permission.command.size(); ++i)
        details += QStringLiteral("\n[%1] %2").arg(i).arg(quoted(permission.command.at(i)));
    auto* text = new QLabel(details, body);
    text->setObjectName(QStringLiteral("atlasRendererCommand"));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    text->setFocusPolicy(Qt::StrongFocus);
    auto* scroll = new QScrollArea(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(text);
    layout->addWidget(scroll);
    dialog->setBody(body);
    auto* view = dialog->addButton(QObject::tr("View static"), ket::ButtonVariant::Secondary);
    view->setObjectName(QStringLiteral("atlasRendererStatic"));
    QObject::connect(view, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* run = dialog->addButton(QObject::tr("Run once"), ket::ButtonVariant::Primary);
    run->setObjectName(QStringLiteral("atlasRendererRun"));
    QObject::connect(run, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* enter = new QShortcut(QKeySequence(key), dialog);
        enter->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(enter, &QShortcut::activated, dialog,
                         [run, view] { (run->hasFocus() ? run : view)->click(); });
    }
    dialog->open();
    view->setFocus();
    return dialog;
}

} // namespace ketplus
