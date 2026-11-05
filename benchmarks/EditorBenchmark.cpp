#include "editor/EditorWidget.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
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

    return 0;
}
