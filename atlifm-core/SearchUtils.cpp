#include "SearchUtils.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cctype>
#include <vector>

namespace Core::Search {

namespace {

bool isBoundary(QChar previous) {
    return previous.isSpace() || previous == QLatin1Char('/') || previous == QLatin1Char('_') ||
           previous == QLatin1Char('-') || previous == QLatin1Char('.');
}

// совпадение символов слова и бонусы за начало и подряд идущие буквы
int scoreToken(const QString& needle,
               const QString& hay,
               const QString& original) {
    if (needle.isEmpty()) return 0;
    if (hay.isEmpty() || needle.size() > hay.size()) return -1;

    int score = 0;
    int qi = 0;
    int last = -2;
    int first = -1;
    for (int i = 0; i < hay.size() && qi < needle.size(); ++i) {
        if (hay.at(i) != needle.at(qi)) continue;
        if (first < 0) first = i;
        score += 10;
        if (i == last + 1) score += 12;
        if (i == 0 || isBoundary(original.at(i - 1))) score += 18;
        if (original.at(i).isUpper()) score += 2;
        last = i;
        ++qi;
    }
    if (qi != needle.size()) return -1;

    // раннее вхождение релевантнее и легкий штраф за длину пути
    score += std::max(0, 24 - first);
    score -= std::min(18, static_cast<int>(hay.size() / 10));
    return score;
}

QStringList splitTokens(const QString& query) {
    return query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QString unescapeMountField(QString value) {
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    return value;
}

struct MountTable {
    std::vector<std::filesystem::path> roots;
    QSet<QString> points;
};

MountTable readMountTable() {
    MountTable out;
    QSet<QString> roots;
    QFile file(QStringLiteral("/proc/mounts"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const QStringList fields = QString::fromUtf8(line).split(
            QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 3) continue;
        const QString source = unescapeMountField(fields.at(0));
        const QString point = unescapeMountField(fields.at(1));
        const QString fsType = fields.at(2);
        if (point.isEmpty() || !point.startsWith(QLatin1Char('/'))) continue;
        const std::filesystem::path mountPoint =
            std::filesystem::path(point.toUtf8().toStdString()).lexically_normal();
        out.points.insert(QString::fromUtf8(mountPoint.c_str()));
        if (!source.startsWith(QLatin1String("/dev/")) ||
            source.startsWith(QLatin1String("/dev/loop"))) {
            continue;
        }
        const QString normalized = QString::fromUtf8(mountPoint.c_str());
        if (normalized.startsWith(QLatin1String("/var/")) ||
            normalized.startsWith(QLatin1String("/boot")) ||
            normalized == QLatin1String("/efi") ||
            (normalized.startsWith(QLatin1String("/run/")) &&
             !normalized.startsWith(QLatin1String("/run/media/")))) {
            continue;
        }
        if (fsType == QLatin1String("squashfs") || fsType == QLatin1String("iso9660")) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(mountPoint, ec)) {
            continue;
        }
        if (roots.contains(normalized)) continue;
        roots.insert(normalized);
        out.roots.push_back(mountPoint);
    }
    std::sort(out.roots.begin(), out.roots.end());
    return out;
}

const MountTable& mountTable() {
    static const MountTable table = readMountTable();
    return table;
}

}

int fuzzyScore(const QString& query, const QString& text) {
    if (query.isEmpty()) return 0;
    if (text.isEmpty()) return -1;

    const QStringList tokens = splitTokens(query);
    if (tokens.isEmpty()) return 0;

    const QString hay = text.toLower();
    const QString base = QFileInfo(text).fileName().toLower();
    const QString baseOriginal = QFileInfo(text).fileName();
    const QString queryLower = query.toLower();

    int total = 0;
    for (const QString& rawToken : tokens) {
        const QString token = rawToken.toLower();
        if (token.isEmpty()) continue;

        const int inBase = scoreToken(token, base, baseOriginal);
        const int inPath = scoreToken(token, hay, text);
        const int best = std::max(inBase, inPath);
        if (best < 0) return -1;
        total += best;
        // совпадение в имени файла важнее совпадения в родительском каталоге
        if (inBase >= 0) total += 16;
    }

    if (hay.contains(queryLower)) total += 45;
    if (base == queryLower) total += 190;
    else if (base.startsWith(queryLower)) total += 130;
    else if (base.contains(queryLower)) total += 55;

    return total;
}

QString displayPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    if (ec || rel.empty()) return QString::fromUtf8(path.c_str());
    return QString::fromUtf8(rel.c_str());
}

std::vector<std::filesystem::path> searchRoots() {
    return readMountTable().roots;
}

bool isMountPoint(const std::filesystem::path& path) {
    const std::string normalized = path.lexically_normal().string();
    return mountTable().points.contains(QString::fromUtf8(normalized.c_str()));
}

bool isVideoFile(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" ||
           ext == ".mov" || ext == ".m4v" || ext == ".flv" || ext == ".wmv" ||
           ext == ".mpg" || ext == ".mpeg" || ext == ".ts" || ext == ".3gp";
}

bool isPdfFile(const std::filesystem::path& path) {
    return QString::fromUtf8(path.extension().c_str()).compare(".pdf", Qt::CaseInsensitive) == 0;
}

} // namespace Core::Search
