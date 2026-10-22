#include "Thumbnails.hpp"
#include "ExternalTools.hpp"
#include "SearchUtils.hpp"
#include "XdgUtils.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace Core::Thumbnails {

namespace {

QString percentEncodePath(const std::filesystem::path& path) {
    return QString::fromUtf8(path.c_str());
}

QString cacheStem(const std::filesystem::path& path, int page) {
    struct stat st {};
    QString signature = QString::fromUtf8(path.c_str());
    if (stat(path.c_str(), &st) == 0) {
        signature += QStringLiteral("|%1|%2|%3|%4")
                         .arg(static_cast<qint64>(st.st_mtim.tv_sec))
                         .arg(static_cast<qint64>(st.st_mtim.tv_nsec))
                         .arg(static_cast<qulonglong>(st.st_size))
                         .arg(page);
    } else {
        signature += QStringLiteral("|0|0|0|%1").arg(page);
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(signature.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString formatDuration(double seconds) {
    if (seconds <= 0 || !std::isfinite(seconds)) return {};
    const int total = static_cast<int>(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
                                               .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString probeVideoInfo(const std::filesystem::path& path,
                       const std::shared_ptr<std::atomic_bool>& cancelToken) {
    const QString ffprobe = Core::Tools::findTool(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) return {};
    const auto result = Core::Tools::runSync(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
         QStringLiteral("v:0"), QStringLiteral("-show_entries"),
         QStringLiteral("stream=width,height"), QStringLiteral("-show_entries"),
         QStringLiteral("format=duration"), QStringLiteral("-of"),
         QStringLiteral("json"), percentEncodePath(path)},
        4000, 256 * 1024, cancelToken);
    if (result.output.isEmpty()) return {};

    QString out;
    const QString json = QString::fromUtf8(result.output);
    const QRegularExpression widthRe(QStringLiteral("\"width\"\\s*:\\s*(\\d+)"));
    const QRegularExpression heightRe(QStringLiteral("\"height\"\\s*:\\s*(\\d+)"));
    const QRegularExpression durationRe(QStringLiteral("\"duration\"\\s*:\\s*\"?([0-9.]+)"));
    const auto wm = widthRe.match(json);
    const auto hm = heightRe.match(json);
    const auto dm = durationRe.match(json);
    if (wm.hasMatch() && hm.hasMatch()) {
        out = QStringLiteral("%1×%2").arg(wm.captured(1), hm.captured(1));
    }
    if (dm.hasMatch()) {
        bool ok = false;
        const double seconds = dm.captured(1).toDouble(&ok);
        if (ok) {
            const QString duration = formatDuration(seconds);
            if (!duration.isEmpty()) out += (out.isEmpty() ? QString() : QStringLiteral(" · ")) + duration;
        }
    }
    return out;
}

bool renderVideoFrame(const std::filesystem::path& path, int width,
                      const QString& output,
                      const std::shared_ptr<std::atomic_bool>& cancelToken,
                      QString& error) {
    const QString thumbnailer = Core::Tools::findTool(QStringLiteral("ffmpegthumbnailer"));
    if (!thumbnailer.isEmpty()) {
        const auto result = Core::Tools::runSync(
            thumbnailer,
            {QStringLiteral("-i"), percentEncodePath(path), QStringLiteral("-o"), output,
             QStringLiteral("-s"), QString::number(width), QStringLiteral("-t"),
             QStringLiteral("10%")},
            15000, 256 * 1024, cancelToken);
        if (result.cancelled) {
            error = QStringLiteral("Cancelled");
            return false;
        }
        if (QFileInfo::exists(output)) return true;
        if (!result.error.isEmpty()) error = QString::fromUtf8(result.error).trimmed();
    }

    const QString ffmpeg = Core::Tools::findTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        if (error.isEmpty()) {
            error = QStringLiteral("ffmpegthumbnailer / ffmpeg is not installed");
        }
        return false;
    }
    const auto result = Core::Tools::runSync(
        ffmpeg,
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
         QStringLiteral("-ss"), QStringLiteral("1"), QStringLiteral("-i"),
         percentEncodePath(path), QStringLiteral("-frames:v"), QStringLiteral("1"),
         QStringLiteral("-vf"), QStringLiteral("scale=%1:-2").arg(width),
         QStringLiteral("-y"), output},
        20000, 512 * 1024, cancelToken);
    if (result.cancelled) {
        error = QStringLiteral("Cancelled");
        return false;
    }
    if (!QFileInfo::exists(output)) {
        error = QString::fromUtf8(result.error).trimmed();
        if (error.isEmpty()) error = QStringLiteral("ffmpeg could not extract a frame");
        return false;
    }
    return true;
}

} // namespace

QString cacheDirectory() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        const char* home = std::getenv("HOME");
        base = home ? QString::fromUtf8(home) + QStringLiteral("/.cache/atlifm")
                    : QDir::tempPath() + QStringLiteral("/atlifm-cache");
    }
    const QString dir = base + QStringLiteral("/thumbnails");
    QDir().mkpath(dir);
    return dir;
}

QString cacheKey(const std::filesystem::path& path, int page) {
    return cacheStem(path, page);
}

QImage loadCached(const std::filesystem::path& path, int page) {
    const QString file = cacheDirectory() + QLatin1Char('/') + cacheKey(path, page) +
                         QStringLiteral(".png");
    if (!QFileInfo::exists(file)) return {};
    QImage image(file);
    return image;
}

void storeCached(const std::filesystem::path& path, const QImage& image, int page) {
    if (image.isNull()) return;
    const QString file = cacheDirectory() + QLatin1Char('/') + cacheKey(path, page) +
                         QStringLiteral(".png");
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) return;
    if (!image.save(&out, "PNG")) {
        out.cancelWriting();
        return;
    }
    out.commit();

    static std::atomic<int> writes{0};
    if ((++writes % 12) == 0) pruneCache();
}

void pruneCache(qint64 maxBytes, int maxFiles) {
    static QMutex mutex;
    QMutexLocker lock(&mutex);

    QDir dir(cacheDirectory());
    QFileInfoList files = dir.entryInfoList({QStringLiteral("*.png")},
                                            QDir::Files | QDir::NoSymLinks,
                                            QDir::Time);
    qint64 total = 0;
    for (const QFileInfo& info : files) total += info.size();

    int kept = 0;
    for (const QFileInfo& info : files) {
        if (total <= maxBytes && kept < maxFiles) {
            ++kept;
            continue;
        }
        total -= info.size();
        QFile::remove(info.absoluteFilePath());
    }
}

Result renderVideo(const std::filesystem::path& path, int width,
                   const std::shared_ptr<std::atomic_bool>& cancelToken) {
    Result result;
    if (Core::Tools::findTool(QStringLiteral("ffmpegthumbnailer")).isEmpty() &&
        Core::Tools::findTool(QStringLiteral("ffmpeg")).isEmpty()) {
        result.error = QStringLiteral("Video preview needs ffmpegthumbnailer or ffmpeg");
        return result;
    }

    QTemporaryDir temp;
    if (!temp.isValid()) {
        result.error = QStringLiteral("Cannot create a temporary directory");
        return result;
    }
    const QString output = temp.filePath(QStringLiteral("video.png"));
    if (!renderVideoFrame(path, width, output, cancelToken, result.error)) {
        result.cancelled = cancelToken && cancelToken->load();
        return result;
    }
    QImage image(output);
    if (image.isNull()) {
        result.error = QStringLiteral("Cannot decode the extracted frame");
        return result;
    }
    storeCached(path, image);

    std::error_code sizeEc;
    const uintmax_t size = std::filesystem::file_size(path, sizeEc);
    const QString probe = probeVideoInfo(path, cancelToken);
    QStringList parts;
    if (!probe.isEmpty()) parts << probe;
    parts << Core::XdgUtils::formatSize(sizeEc ? 0 : size);
    result.image = image;
    result.info = QStringLiteral("Video · %1").arg(parts.join(QStringLiteral(" · ")));
    return result;
}

Result renderPdfPage(const std::filesystem::path& path, int page, int width,
                     const std::shared_ptr<std::atomic_bool>& cancelToken) {
    Result result;
    if (page < 1) page = 1;
    const QString tool = Core::Tools::findTool(QStringLiteral("pdftoppm"));
    if (tool.isEmpty()) {
        result.error = QStringLiteral("PDF preview needs pdftoppm (poppler-utils)");
        return result;
    }

    QTemporaryDir temp;
    if (!temp.isValid()) {
        result.error = QStringLiteral("Cannot create a temporary directory");
        return result;
    }
    const QString prefix = temp.filePath(QStringLiteral("page"));
    auto run = [&](bool singleFile) {
        QStringList args{QStringLiteral("-f"), QString::number(page),
                         QStringLiteral("-l"), QString::number(page),
                         QStringLiteral("-scale-to"), QString::number(width),
                         QStringLiteral("-png")};
        if (singleFile) args << QStringLiteral("-singlefile");
        args << percentEncodePath(path) << prefix;
        return Core::Tools::runSync(tool, args, 30000, 512 * 1024, cancelToken);
    };

    auto proc = run(true);
    QString output = prefix + QStringLiteral(".png");
    if (!QFileInfo::exists(output)) {
        proc = run(false);
        output = QStringLiteral("%1-%2.png").arg(prefix).arg(page);
    }
    if (proc.cancelled || (cancelToken && cancelToken->load())) {
        result.cancelled = true;
        result.error = QStringLiteral("Cancelled");
        return result;
    }
    if (!QFileInfo::exists(output)) {
        result.error = page == 1
            ? QString::fromUtf8(proc.error).trimmed()
            : QStringLiteral("End of document");
        if (result.error.isEmpty()) result.error = QStringLiteral("pdftoppm produced no page");
        return result;
    }

    QImage image(output);
    if (image.isNull()) {
        result.error = QStringLiteral("Cannot decode the PDF page");
        return result;
    }
    storeCached(path, image, page);
    result.image = image;
    result.info = QStringLiteral("PDF · page %1").arg(page);
    return result;
}

int pdfPageCount(const std::filesystem::path& path,
                 const std::shared_ptr<std::atomic_bool>& cancelToken) {
    const QString tool = Core::Tools::findTool(QStringLiteral("pdfinfo"));
    if (tool.isEmpty()) return -1;
    const auto result = Core::Tools::runSync(
        tool, {percentEncodePath(path)}, 5000, 256 * 1024, cancelToken);
    if (result.cancelled || result.output.isEmpty()) return -1;
    const QRegularExpression pagesRe(QStringLiteral("^Pages:\\s*(\\d+)"),
                                     QRegularExpression::MultilineOption);
    const auto match = pagesRe.match(QString::fromUtf8(result.output));
    if (!match.hasMatch()) return -1;
    return match.captured(1).toInt();
}

} // namespace Core::Thumbnails
