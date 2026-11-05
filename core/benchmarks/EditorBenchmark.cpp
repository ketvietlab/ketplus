#include "core/Document.h"
#include "editor/EditorWidget.h"
#include "editor/SymbolExtractor.h"
#include "ui/Theme.h"
#include "workspace/FuzzyMatcher.h"
#include "workspace/WorkspaceFileIndex.h"
#include "workspace/WorkspaceSearch.h"

#include <ScintillaMessages.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <functional>
#include <vector>

namespace {

constexpr qsizetype mebibyte = 1024 * 1024;

struct Sample final {
    double medianMilliseconds{0.0};
    double minimumMilliseconds{0.0};
    double maximumMilliseconds{0.0};
};

QByteArray makeDocument(const qsizetype size) {
    static const QByteArray line =
        "const alpha = beta + gamma; // KetPlus benchmark source line 0123456789\n";
    QByteArray content;
    content.reserve(size);
    while (content.size() < size) {
        content.append(line);
    }
    content.resize(size);
    return content;
}

Sample measure(const int iterations, const std::function<void()>& operation,
               const std::function<void()>& setup = {}) {
    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(iterations));
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (setup) {
            setup();
        }
        QElapsedTimer timer;
        timer.start();
        operation();
        timings.push_back(static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0);
    }
    std::ranges::sort(timings);
    return {
        .medianMilliseconds = timings.at(timings.size() / 2),
        .minimumMilliseconds = timings.front(),
        .maximumMilliseconds = timings.back(),
    };
}

void printMetric(QTextStream& output, const QString& name, const Sample& sample,
                 const int iterations, const qsizetype bytes = 0) {
    output << "METRIC " << name
           << " median_ms=" << QString::number(sample.medianMilliseconds, 'f', 3)
           << " min_ms=" << QString::number(sample.minimumMilliseconds, 'f', 3)
           << " max_ms=" << QString::number(sample.maximumMilliseconds, 'f', 3)
           << " iterations=" << iterations;
    if (bytes > 0 && sample.medianMilliseconds > 0.0) {
        const double sizeMebibytes = static_cast<double>(bytes) / static_cast<double>(mebibyte);
        output << " mib=" << QString::number(sizeMebibytes, 'f', 1) << " throughput_mib_s="
               << QString::number(sizeMebibytes * 1000.0 / sample.medianMilliseconds, 'f', 1);
    }
    output << '\n';
    output.flush();
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QTextStream output(stdout);

    const QStringList arguments = application.arguments();
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--write-fixture")) {
        bool sizeIsValid = false;
        const qsizetype size = arguments.at(3).toLongLong(&sizeIsValid);
        QFile fixture(arguments.at(2));
        if (!sizeIsValid || size <= 0 || !fixture.open(QIODevice::WriteOnly) ||
            fixture.write(makeDocument(size)) != size) {
            return 2;
        }
        return 0;
    }

    const QByteArray oneMebibyte = makeDocument(mebibyte);
    const QByteArray tenMebibytes = makeDocument(10 * mebibyte);
    const QByteArray fiftyMebibytes = makeDocument(50 * mebibyte);
    ketplus::EditorWidget editor;

    editor.setText(oneMebibyte);
    printMetric(output, QStringLiteral("set_text_1_mib"),
                measure(15, [&] { editor.setText(oneMebibyte); }), 15, oneMebibyte.size());

    editor.configureLexerForPath(QStringLiteral("benchmark.cpp"));
    ketplus::ThemePalette palette;
    auto typography = ketplus::EditorSettings::defaults();
    bool useAlternateTypography = false;
    printMetric(
        output, QStringLiteral("apply_typography_1_mib"),
        measure(15, [&] {
            useAlternateTypography = !useAlternateTypography;
            typography.fontSizePixels = useAlternateTypography ? 14 : 13;
            typography.lineHeightPixels = useAlternateTypography ? 25 : 24;
            editor.setEditorSettings(typography);
            editor.applyTheme(palette);
        }),
        15, oneMebibyte.size());

    // Same work with code folding off, to separate fold-level cost from theme cost.
    auto foldingOff = editor.viewOptions();
    foldingOff.codeFolding = false;
    editor.setViewOptions(foldingOff);
    printMetric(
        output, QStringLiteral("apply_typography_no_folding_1_mib"),
        measure(15, [&] {
            useAlternateTypography = !useAlternateTypography;
            typography.fontSizePixels = useAlternateTypography ? 14 : 13;
            typography.lineHeightPixels = useAlternateTypography ? 25 : 24;
            editor.setEditorSettings(typography);
            editor.applyTheme(palette);
        }),
        15, oneMebibyte.size());
    foldingOff.codeFolding = true;
    editor.setViewOptions(foldingOff);

    editor.setText(tenMebibytes);
    printMetric(output, QStringLiteral("set_text_10_mib"),
                measure(9, [&] { editor.setText(tenMebibytes); }), 9, tenMebibytes.size());

    editor.setText(fiftyMebibytes);
    printMetric(output, QStringLiteral("set_text_50_mib"),
                measure(5, [&] { editor.setText(fiftyMebibytes); }), 5, fiftyMebibytes.size());

    QByteArray searchable = tenMebibytes;
    static const QByteArray needle = "KETPLUS_UNIQUE_NEEDLE";
    searchable.replace(searchable.size() - needle.size(), needle.size(), needle);
    editor.setText(searchable);
    printMetric(
        output, QStringLiteral("find_at_end_10_mib"),
        measure(15, [&] { editor.findText(QString::fromLatin1(needle), false, true, false); }), 15,
        searchable.size());

    int replacementCount = 0;
    const auto replaceSample = measure(
        5,
        [&] {
            replacementCount =
                editor.replaceAll(QStringLiteral("alpha"), QStringLiteral("omega"), true, true);
        },
        [&] { editor.setText(tenMebibytes); });
    printMetric(output, QStringLiteral("replace_all_10_mib"), replaceSample, 5,
                tenMebibytes.size());
    output << "DETAIL replace_all_matches=" << replacementCount << '\n';

    // Features added after 0.1.6. Baseline builds do not have these metrics.
    editor.setText(oneMebibyte);
    printMetric(output, QStringLiteral("configure_lexer_folding_1_mib"),
                measure(9, [&] { editor.configureLexerForPath(QStringLiteral("benchmark.cpp")); }),
                9, oneMebibyte.size());

    int highlightCount = 0;
    printMetric(output, QStringLiteral("highlight_matches_1_mib"), measure(9, [&] {
                    highlightCount = editor.highlightMatches(
                        QStringLiteral("alpha"), ketplus::SearchOptions{.matchCase = true});
                }),
                9, oneMebibyte.size());
    output << "DETAIL highlight_matches_capped=" << highlightCount << '\n';

    // Selection highlights only scan visible lines, so give the editor a real viewport.
    editor.resize(1200, 900);
    editor.show();
    QApplication::processEvents();
    editor.setCaretPosition(0);
    editor.send(static_cast<unsigned int>(Scintilla::Message::SetSel), 6, 11);
    int selectionMatches = 0;
    printMetric(output, QStringLiteral("selection_highlight_1_mib"),
                measure(15, [&] { selectionMatches = editor.highlightSelectionMatches(); }), 15,
                oneMebibyte.size());
    output << "DETAIL selection_matches_visible=" << selectionMatches
           << " lines_on_screen="
           << editor.send(static_cast<unsigned int>(Scintilla::Message::LinesOnScreen)) << '\n';
    editor.hide();

    editor.setCaretPosition(oneMebibyte.size() / 2);
    editor.send(static_cast<unsigned int>(Scintilla::Message::AddText), 3,
                reinterpret_cast<sptr_t>("alp"));
    printMetric(output, QStringLiteral("word_completion_1_mib"), measure(15, [&] {
                    editor.showWordCompletions(true);
                    editor.send(static_cast<unsigned int>(Scintilla::Message::AutoCCancel));
                }),
                15, oneMebibyte.size());

    printMetric(output, QStringLiteral("sort_lines_1_mib"),
                measure(5, [&] { editor.sortLines(true); }, [&] { editor.setText(oneMebibyte); }),
                5, oneMebibyte.size());
    printMetric(output, QStringLiteral("toggle_comment_1_mib"), measure(
                    5, [&] { editor.toggleComment(); },
                    [&] {
                        editor.setText(oneMebibyte);
                        editor.configureLexerForPath(QStringLiteral("benchmark.cpp"));
                        editor.selectAllText();
                    }),
                5, oneMebibyte.size());
    printMetric(output, QStringLiteral("trim_trailing_whitespace_1_mib"),
                measure(5, [&] { editor.trimTrailingWhitespace(); },
                        [&] { editor.setText(oneMebibyte); }),
                5, oneMebibyte.size());

    int symbolCount = 0;
    printMetric(output, QStringLiteral("extract_symbols_1_mib"), measure(9, [&] {
                    symbolCount = static_cast<int>(
                        ketplus::extractDocumentSymbols(oneMebibyte, QStringLiteral("c-cpp")).size());
                }),
                9, oneMebibyte.size());
    output << "DETAIL symbols=" << symbolCount << '\n';

    printMetric(output, QStringLiteral("detect_encoding_10_mib"),
                measure(9, [&] { static_cast<void>(ketplus::Document::detectEncoding(tenMebibytes)); }),
                9, tenMebibytes.size());

    QStringList paths;
    for (int index = 0; index < 50000; ++index) {
        paths.append(QStringLiteral("src/module%1/component%2/SourceFile%3.cpp")
                         .arg(index % 40)
                         .arg(index % 400)
                         .arg(index));
    }
    int fuzzyMatches = 0;
    printMetric(output, QStringLiteral("fuzzy_match_50k_paths"), measure(9, [&] {
                    fuzzyMatches = 0;
                    for (const QString& path : paths) {
                        fuzzyMatches += ketplus::fuzzyMatchScore(u"cmpsrc123", path) >= 0 ? 1 : 0;
                    }
                }),
                9);
    output << "DETAIL fuzzy_matches=" << fuzzyMatches << '\n';

    QTemporaryDir workspace;
    if (workspace.isValid()) {
        constexpr int workspaceFileCount = 2000;
        constexpr qsizetype workspaceFileBytes = 20 * 1024;
        const QByteArray fileContent = makeDocument(workspaceFileBytes);
        const QDir root(workspace.path());
        for (int index = 0; index < workspaceFileCount; ++index) {
            const QString relative = QStringLiteral("dir%1/file%2.ts").arg(index % 50).arg(index);
            root.mkpath(QFileInfo(root.filePath(relative)).path());
            QFile file(root.filePath(relative));
            if (file.open(QIODevice::WriteOnly)) {
                file.write(fileContent);
            }
        }
        const qsizetype workspaceBytes = workspaceFileCount * workspaceFileBytes;

        printMetric(output, QStringLiteral("collect_workspace_files_2k"),
                    measure(5, [&] { static_cast<void>(ketplus::collectWorkspaceFiles(root.path())); }),
                    5);

        const ketplus::WorkspaceSearchOptions options{.query = QStringLiteral("gamma"),
                                                      .matchCase = true};
        QString error;
        const QRegularExpression expression = ketplus::buildSearchExpression(options, &error);
        ketplus::WorkspaceSearchSummary summary;
        printMetric(output, QStringLiteral("search_workspace_2k_files_40_mib"), measure(3, [&] {
                        summary = ketplus::searchWorkspace(root.path(), options, expression, {},
                                                           nullptr, {});
                    }),
                    3, workspaceBytes);
        output << "DETAIL workspace_search_files=" << summary.fileCount
               << " matches=" << summary.matchCount << " truncated=" << summary.truncated << '\n';

        const ketplus::WorkspaceSearchOptions rareOptions{.query = QStringLiteral("NOT_PRESENT_\\d+"),
                                                          .regex = true};
        const QRegularExpression rareExpression = ketplus::buildSearchExpression(rareOptions, &error);
        printMetric(output, QStringLiteral("search_workspace_regex_no_match_40_mib"), measure(3, [&] {
                        summary = ketplus::searchWorkspace(root.path(), rareOptions, rareExpression,
                                                           {}, nullptr, {});
                    }),
                    3, workspaceBytes);
    }

    return 0;
}
