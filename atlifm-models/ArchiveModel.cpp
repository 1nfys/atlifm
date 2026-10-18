#include "ArchiveModel.hpp"
#include "FileSystemModel.hpp"
#include "../atlifm-core/Icons.hpp"

#include <QDate>
#include <QDateTime>
#include <QApplication>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTime>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

namespace Models {

namespace {

// нормализация «./sub/./a.txt» → «sub/a.txt», ведущие «..» отбрасываются,
// дерево не выходит за корень (защита от траверсала)
QString normalizeEntryPath(const QString& raw) {
    QString p = raw;
    p.replace('\\', '/');
    QStringList segs = p.split('/', Qt::SkipEmptyParts);
    segs.removeAll(QStringLiteral("."));
    while (!segs.isEmpty() && segs.first() == QStringLiteral("..")) segs.removeFirst();
    return segs.join('/');
}

// путь записан в стиле «./…» (листинг bsdtar/tar),
// от этого зависит имя для распаковки синтезированных каталогов
bool rawUsesDotSlash(const QString& raw) {
    return raw.startsWith(QStringLiteral("./")) || raw.startsWith(QStringLiteral(".\\"));
}

// «2026-10-17 00:40:49» / «Oct 17 00:40» → «17.10 00:40», непонятное как есть
QString formatEntryDate(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty()) return QString();

    QDateTime dt = QDateTime::fromString(s.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!dt.isValid()) dt = QDateTime::fromString(s.left(16), QStringLiteral("yyyy-MM-dd HH:mm"));

    if (!dt.isValid()) {
        static const QRegularExpression re(
            QStringLiteral(R"(^([A-Za-z]{3})\s+(\d{1,2})\s+(?:(\d{4})|(\d{1,2}:\d{2}))$)"));
        const QRegularExpressionMatch m = re.match(s.simplified());
        if (m.hasMatch()) {
            static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            int month = 1;
            const QString mname = m.captured(1);
            for (int i = 0; i < 12; ++i) {
                if (mname.compare(QString::fromLatin1(kMonths[i]), Qt::CaseInsensitive) == 0) {
                    month = i + 1;
                    break;
                }
            }
            const int day = m.captured(2).toInt();
            if (!m.captured(4).isEmpty()) {
                dt = QDateTime(QDate(QDate::currentDate().year(), month, day),
                               QTime::fromString(m.captured(4), QStringLiteral("HH:mm")));
            } else if (!m.captured(3).isEmpty()) {
                dt = QDateTime(QDate(m.captured(3).toInt(), month, day), QTime(0, 0));
            }
        }
    }
    if (dt.isValid()) return dt.toString(QStringLiteral("dd.MM hh:mm"));
    return s.left(16);
}

Icons::IconInfo iconForName(const QString& name, bool isDir) {
    // виртуальные пути на диске не существуют, isExecutableHint отключает лишний stat()
    return Icons::getIcon(std::filesystem::path(name.toStdString()), isDir,
                          false, false, std::optional<bool>(false));
}

}

ArchiveModel::ArchiveModel(QObject* parent)
    : QAbstractListModel(parent) {
}

ArchiveModel::~ArchiveModel() {
    // фоновые результаты отбрасывает проверка generation, QPointer защищает от исчезнувшего объекта
    m_generation++;
}

void ArchiveModel::setArchive(const std::filesystem::path& archivePath) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(archivePath, ec);
    const auto size = std::filesystem::file_size(archivePath, ec);
    const int64_t sigNs = ec ? -1
        : std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

    // тот же неизменившийся архив не перечитываем, сохраняет позицию навигации при возврате к превью
    if (archivePath == m_archivePath && (m_loaded || m_loading) &&
        sigNs == m_sigMtimeNs && static_cast<int64_t>(size) == m_sigSize) {
        return;
    }

    m_archivePath = archivePath;
    m_sigMtimeNs = sigNs;
    m_sigSize = ec ? -1 : static_cast<int64_t>(size);
    m_prefix.clear();
    m_loading = true;
    m_loaded = false;
    m_truncated = false;
    m_error.clear();
    m_fileCount = 0;
    m_dirCount = 0;
    m_totalSize = 0;
    m_entries.clear();

    const uint64_t generation = ++m_generation;

    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit prefixChanged();

    QPointer<ArchiveModel> self(this);
    QThreadPool::globalInstance()->start([self, generation, archivePath]() {
        std::vector<Core::XdgUtils::ArchiveEntry> entries;
        bool truncated = false;
        QString err;
        const bool ok = Core::XdgUtils::listArchive(archivePath, entries, truncated, err);
        QMetaObject::invokeMethod(qApp, [self, generation, ok, err,
                                         entries = std::move(entries), truncated]() mutable {
            if (!self) return;
            self->applyListing(generation, ok, err, std::move(entries), truncated);
        }, Qt::QueuedConnection);
    });
}

void ArchiveModel::applyListing(uint64_t generation, bool ok, const QString& error,
                                std::vector<Core::XdgUtils::ArchiveEntry> entries, bool truncated) {
    if (generation != m_generation) return;
    m_loading = false;
    m_entries = std::move(entries);
    m_truncated = truncated;

    if (!ok) {
        m_loaded = false;
        m_error = error.isEmpty() ? QStringLiteral("unknown error") : error;
        m_fileCount = 0;
        m_dirCount = 0;
        m_totalSize = 0;
        beginResetModel();
        m_rows.clear();
        endResetModel();
        emit prefixChanged();
        emit loadFailed(m_error);
        return;
    }

    // одиночный поток без записей (.zst/.lz4, 7z не отдаёт path),
    // синтезируем единственный файл из имени архива
    if (m_entries.empty() && Core::XdgUtils::isSingleStreamArchive(m_archivePath)) {
        Core::XdgUtils::ArchiveEntry e;
        e.path = QString::fromUtf8(Core::XdgUtils::archiveExtractDirName(m_archivePath).string().c_str());
        e.isDir = false;
        if (!e.path.isEmpty()) m_entries.push_back(std::move(e));
    }

    m_error.clear();
    m_fileCount = 0;
    m_dirCount = 0;
    m_totalSize = 0;
    for (const auto& e : m_entries) {
        if (e.isDir) {
            ++m_dirCount;
        } else {
            ++m_fileCount;
            m_totalSize += e.size;
        }
    }

    m_loaded = true;
    rebuildRows();
    emit loaded();
}

void ArchiveModel::rebuildRows() {
    struct ChildInfo {
        bool seen{false};
        bool isDir{false};
        quint64 size{0};
        QString modified;
        bool dotSlash{false};
    };
    std::map<QString, ChildInfo> children;

    const bool atRoot = m_prefix.isEmpty();
    for (const auto& e : m_entries) {
        const QString norm = normalizeEntryPath(e.path);
        if (norm.isEmpty()) continue;
        QString rem;
        if (atRoot) {
            rem = norm;
        } else if (norm.startsWith(m_prefix)) {
            rem = norm.mid(m_prefix.size());
        } else {
            continue;
        }
        if (rem.isEmpty()) continue;

        const int slash = rem.indexOf('/');
        const QString child = (slash >= 0) ? rem.left(slash) : rem;
        if (child.isEmpty()) continue;
        const bool childIsDir = (slash >= 0) || e.isDir;

        ChildInfo& ci = children[child];
        if (!ci.seen) {
            ci.seen = true;
            ci.isDir = childIsDir;
            ci.dotSlash = rawUsesDotSlash(e.path);
        } else {
            ci.isDir = ci.isDir || childIsDir;
        }

        if (slash < 0) { // запись сам ребёнок
            if (e.isDir) {
                if (!e.modified.isEmpty()) ci.modified = e.modified;
            } else {
                ci.size = e.size;
                ci.modified = e.modified;
            }
        }
        // запись внутри подкаталога не трогает метаданные ребёнка
    }

    std::vector<Row> dirs;
    std::vector<Row> files;
    dirs.reserve(children.size());
    files.reserve(children.size());
    for (auto& [name, ci] : children) {
        Row r;
        r.name = name;
        r.entryPath = m_prefix + name;
        r.memberPath = (ci.dotSlash ? QStringLiteral("./") : QString()) + r.entryPath;
        r.isDir = ci.isDir;
        r.size = ci.size;
        r.dateText = formatEntryDate(ci.modified);
        const auto icon = iconForName(name, r.isDir);
        r.glyph = icon.glyph;
        r.color = icon.color;
        (r.isDir ? dirs : files).push_back(std::move(r));
    }

    auto lessName = [](const Row& a, const Row& b) {
        const int c = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        return a.name < b.name;
    };
    std::sort(dirs.begin(), dirs.end(), lessName);
    std::sort(files.begin(), files.end(), lessName);

    std::vector<Row> newRows;
    newRows.reserve(dirs.size() + files.size() + 1);
    if (!atRoot) {
        Row up;
        up.name = QStringLiteral("..");
        up.isDir = true;
        up.parentRow = true;
        const auto icon = iconForName(up.name, true);
        up.glyph = icon.glyph;
        up.color = icon.color;
        newRows.push_back(std::move(up));
    }
    newRows.insert(newRows.end(), std::make_move_iterator(dirs.begin()),
                   std::make_move_iterator(dirs.end()));
    newRows.insert(newRows.end(), std::make_move_iterator(files.begin()),
                   std::make_move_iterator(files.end()));

    beginResetModel();
    m_rows = std::move(newRows);
    endResetModel();
    emit prefixChanged();
}

void ArchiveModel::navigateInto(const QString& entryDirPath) {
    const QString norm = normalizeEntryPath(entryDirPath);
    if (norm.isEmpty()) return;
    m_prefix = norm + QLatin1Char('/');
    rebuildRows();
}

bool ArchiveModel::navigateUp() {
    if (m_prefix.isEmpty()) return false;
    QString p = m_prefix;
    p.chop(1); // завершающий '/'
    const int slash = p.lastIndexOf('/');
    m_prefix = (slash < 0) ? QString() : p.left(slash + 1);
    rebuildRows();
    return true;
}

QString ArchiveModel::entryPathAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return QString();
    return m_rows[static_cast<size_t>(row)].entryPath;
}

QString ArchiveModel::memberPathAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return QString();
    return m_rows[static_cast<size_t>(row)].memberPath;
}

bool ArchiveModel::isParentRow(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return false;
    return m_rows[static_cast<size_t>(row)].parentRow;
}

bool ArchiveModel::isDirAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return false;
    return m_rows[static_cast<size_t>(row)].isDir;
}

int ArchiveModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ArchiveModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(m_rows.size())) {
        return QVariant();
    }
    const Row& r = m_rows[static_cast<size_t>(index.row())];

    switch (role) {
    case NameRole:
        return r.name;
    case SizeRole:
        return r.isDir ? QString() : Core::XdgUtils::formatSize(r.size);
    case DateRole:
        return r.dateText;
    case PermsRole:
        return QString();
    case IconGlyphRole:
        return r.glyph;
    case IconColorRole:
        return r.color;
    case PathRole: {
        if (r.parentRow || r.entryPath.isEmpty()) return QString();
        return QString::fromUtf8(m_archivePath.c_str()) + QChar(0x1F) + r.entryPath;
    }
    case IsDirRole:
        return r.isDir;
    case IsSymlinkRole:
        return false;
    case IsHiddenRole:
        return r.name.startsWith(QLatin1Char('.'));
    case IsSelectedRole:
        return false;
    case RawSizeRole:
        return static_cast<qulonglong>(r.size);
    case FuzzyScoreRole:
        return 0;
    case SymlinkTargetRole:
        return QString();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ArchiveModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[SizeRole] = "size";
    roles[DateRole] = "date";
    roles[PermsRole] = "perms";
    roles[IconGlyphRole] = "iconGlyph";
    roles[IconColorRole] = "iconColor";
    roles[PathRole] = "path";
    roles[IsDirRole] = "isDir";
    roles[IsSymlinkRole] = "isSymlink";
    roles[IsHiddenRole] = "isHidden";
    roles[IsSelectedRole] = "isSelected";
    roles[RawSizeRole] = "rawSize";
    roles[FuzzyScoreRole] = "fuzzyScore";
    roles[SymlinkTargetRole] = "symlinkTarget";
    return roles;
}

}
