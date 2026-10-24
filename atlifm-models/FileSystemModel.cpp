#include "FileSystemModel.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/Icons.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/SearchUtils.hpp"
#include "../atlifm-core/ExternalTools.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"
#include <QRegularExpression>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QThreadPool>
#include <QPointer>
#include <QCoreApplication>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>

namespace Models {

static std::mutex s_inProgressMutex;
static std::unordered_set<std::string> s_inProgressPaths;

void FileSystemModel::addInProgressPath(const std::filesystem::path& p) {
    std::lock_guard<std::mutex> lock(s_inProgressMutex);
    s_inProgressPaths.insert(p.lexically_normal().string());
}

void FileSystemModel::removeInProgressPath(const std::filesystem::path& p) {
    std::lock_guard<std::mutex> lock(s_inProgressMutex);
    s_inProgressPaths.erase(p.lexically_normal().string());
}

bool FileSystemModel::isInProgress(const std::filesystem::path& p) {
    std::lock_guard<std::mutex> lock(s_inProgressMutex);
    if (s_inProgressPaths.empty()) return false;
    return s_inProgressPaths.find(p.lexically_normal().string()) != s_inProgressPaths.end();
}

namespace {

QString globalDisplayPath(const std::filesystem::path& path) {
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(path, std::filesystem::path(home), ec);
        if (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) {
            return QStringLiteral("~/") + QString::fromUtf8(rel.c_str());
        }
    }
    return QString::fromUtf8(path.c_str());
}

FileItem globalSearchItem(const std::filesystem::path& path) {
    FileItem item;
    item.path = path;
    item.filename = globalDisplayPath(path);

    std::error_code ec;
    const auto linkStatus = std::filesystem::symlink_status(path, ec);
    item.isSymlink = std::filesystem::is_symlink(linkStatus);
    const auto status = item.isSymlink ? std::filesystem::status(path, ec) : linkStatus;
    item.isDirectory = std::filesystem::is_directory(status);
    const std::string name = path.filename().string();
    item.isHidden = !name.empty() && name.front() == '.';
    item.permissions = Core::XdgUtils::formatPermissions(status.permissions(), item.isDirectory);
    using perms = std::filesystem::perms;
    item.isExecutable = !item.isDirectory &&
        ((status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none);
    if (!item.isDirectory && std::filesystem::is_regular_file(status)) {
        item.sizeBytes = std::filesystem::file_size(path, ec);
    }
    item.sizeFormatted = item.isDirectory ? QString() : Core::XdgUtils::formatSize(item.sizeBytes);

    time_t timestamp = 0;
    struct statx stx;
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MTIME, &stx) == 0) {
        if ((stx.stx_mask & STATX_MTIME) && stx.stx_mtime.tv_sec != 0) {
            timestamp = stx.stx_mtime.tv_sec;
        }
    }
    if (timestamp == 0) {
        auto ftime = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            timestamp = std::chrono::system_clock::to_time_t(sctp);
        }
    }
    item.modifiedTimestamp = timestamp;
    if (timestamp != 0) {
        std::tm tmVal;
        localtime_r(&timestamp, &tmVal);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmVal);
        item.modifiedDate = QString::fromUtf8(buf);
    }

    if (item.isSymlink) {
        std::error_code linkEc;
        const auto target = std::filesystem::read_symlink(path, linkEc);
        if (!linkEc) item.symlinkTarget = QString::fromUtf8(target.c_str());
    }
    const auto icon = Icons::getIcon(path, item.isDirectory, item.isSymlink, false, item.isExecutable);
    item.iconGlyph = icon.glyph;
    item.iconColor = icon.color;
    return item;
}

std::vector<std::filesystem::path> globalSearchRoots() {
    auto roots = Core::Search::searchRoots();
    if (roots.empty()) roots.push_back(std::filesystem::path("/"));
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

std::vector<FileItem> scanNamesFallback(const QString& query,
                                        const std::vector<std::filesystem::path>& roots,
                                        bool showHidden,
                                        const std::shared_ptr<std::atomic_bool>& cancel,
                                        uint64_t scanId,
                                        const std::shared_ptr<std::atomic<uint64_t>>& scanCounter) {
    std::vector<std::pair<int, FileItem>> found;
    constexpr std::size_t kLimit = 1000;
    for (const auto& root : roots) {
        if (found.size() >= kLimit) break;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!ec && it != end) {
            if (cancel->load() || scanCounter->load() != scanId || found.size() >= kLimit) return {};
            if (it.depth() > 24) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }
            const auto& entry = *it;
            const std::string name = entry.path().filename().string();
            const bool hidden = !name.empty() && name.front() == '.';
            if (!showHidden && hidden) {
                if (entry.is_directory(ec)) it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }
            if (Core::Search::isMountPoint(entry.path()) && entry.path() != root) {
                it.disable_recursion_pending();
            }
            const QString display = globalDisplayPath(entry.path());
            const int score = Core::Search::fuzzyScore(query, display);
            if (score >= 0) {
                found.emplace_back(score, globalSearchItem(entry.path()));
            }
            it.increment(ec);
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return QString::compare(a.second.filename, b.second.filename, Qt::CaseInsensitive) < 0;
    });
    std::vector<FileItem> out;
    out.reserve(found.size());
    for (auto& entry : found) out.push_back(std::move(entry.second));
    return out;
}

} // namespace

FileSystemModel::FileSystemModel(QObject* parent)
    : QAbstractListModel(parent) {
    m_dirWatcher = new QFileSystemWatcher(this);
    m_watcherTimer = new QTimer(this);
    m_watcherTimer->setSingleShot(true);
    m_watcherTimer->setInterval(150);

    connect(m_watcherTimer, &QTimer::timeout, this, &FileSystemModel::refresh);

    connect(m_dirWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& /*path*/) {
        if (!m_currentDir.empty()) {
            m_watcherTimer->start();
        }
    });
}

FileSystemModel::~FileSystemModel() {
    // копия счётчика переживает модель, воркеры безопасно увидят протухание
    ++(*m_scanCounter);
}

int FileSystemModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_displayedItems.size());
}

QVariant FileSystemModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_displayedItems.size())) {
        return QVariant();
    }

    const auto& item = m_displayedItems[index.row()];

    switch (role) {
    case NameRole:
        return item.filename;
    case SizeRole:
        return item.sizeFormatted;
    case DateRole:
        return item.modifiedDate;
    case PermsRole:
        return item.permissions;
    case IconGlyphRole:
        return item.iconGlyph;
    case IconColorRole:
        return item.iconColor;
    case PathRole:
        return QString::fromUtf8(item.path.c_str());
    case IsDirRole:
        return item.isDirectory;
    case IsSymlinkRole:
        return item.isSymlink;
    case IsHiddenRole:
        return item.isHidden;
    case IsExecutableRole:
        return item.isExecutable;
    case IsSelectedRole:
        return item.isSelected;
    case RawSizeRole:
        return static_cast<qulonglong>(item.sizeBytes);
    case FuzzyScoreRole:
        return item.fuzzyScore;
    case SymlinkTargetRole:
        return item.symlinkTarget;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> FileSystemModel::roleNames() const {
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
    roles[IsExecutableRole] = "isExecutable";
    roles[IsSelectedRole] = "isSelected";
    roles[RawSizeRole] = "rawSize";
    roles[FuzzyScoreRole] = "fuzzyScore";
    roles[SymlinkTargetRole] = "symlinkTarget";
    return roles;
}

void FileSystemModel::setDirectory(const std::filesystem::path& dirPath) {
    const bool wasGlobal = m_globalSearch;
    m_globalSearch = false;
    stopGlobalProcess();
    if (m_searchCancel) m_searchCancel->store(true);
    if (m_currentDir == dirPath && !m_allItems.empty() && m_filter.isEmpty() && !wasGlobal) {
        return;
    }

    m_currentDir = dirPath;
    m_filter.clear();

    if (m_dirWatcher) {
        const auto dirs = m_dirWatcher->directories();
        if (!dirs.isEmpty()) {
            m_dirWatcher->removePaths(dirs);
        }
        std::filesystem::path watchTarget = dirPath;
        const char* homeEnv = std::getenv("HOME");
        if (homeEnv) {
            std::string trashDir = (std::filesystem::path(homeEnv) / ".local/share/Trash").lexically_normal().string();
            if (dirPath.lexically_normal().string() == trashDir) {
                watchTarget = dirPath / "files";
            }
        }
        std::error_code ec;
        if (std::filesystem::exists(watchTarget, ec) && std::filesystem::is_directory(watchTarget, ec)) {
            m_dirWatcher->addPath(QString::fromUtf8(watchTarget.c_str()));
        }
    }

    refresh();
}

void FileSystemModel::refresh() {
    m_globalSearch = false;
    stopGlobalProcess();
    if (m_searchCancel) m_searchCancel->store(true);
    uint64_t scanId = ++(*m_scanCounter);
    m_isLoading = true;
    m_dirScanPending = true;
    emit loadingStarted();

    // без join в потоке gui: старый скан сам выйдет по несовпадению scanid,
    // воркер получает разделяемое состояние и не трогает модель небезопасно
    QPointer<FileSystemModel> self(this);
    std::filesystem::path dir = m_currentDir;
    auto scanCounter = m_scanCounter;
    const bool lightweight = m_lightweightScan;
    QThreadPool* pool = Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("listing"));
    pool->start([self, dir, scanId, scanCounter, lightweight]() {
        // статический вызов: this в воркере не разыменовывается, разрушение модели посреди скана без uaf
        FileSystemModel::scanDirectoryAsync(dir, scanId, scanCounter, self, lightweight);
    });
}

void FileSystemModel::scanDirectoryAsync(std::filesystem::path dirPath, uint64_t scanId,
                                         std::shared_ptr<std::atomic<uint64_t>> scanCounter,
                                         QPointer<FileSystemModel> self, bool lightweight) {
    std::string dirStr = dirPath.lexically_normal().string();
    const char* homeEnv = std::getenv("HOME");
    std::string trashDir = homeEnv ? (std::filesystem::path(homeEnv) / ".local/share/Trash").lexically_normal().string() : "";
    bool isVirtualTrashRoot = (!trashDir.empty() && dirStr == trashDir);
    if (isVirtualTrashRoot) {
        std::filesystem::path actualFilesDir = dirPath / "files";
        std::error_code ecTrash;
        std::filesystem::create_directories(actualFilesDir, ecTrash);
        dirPath = actualFilesDir;
    }

    std::error_code ec;
    bool isDir = std::filesystem::exists(dirPath, ec) && std::filesystem::is_directory(dirPath, ec);
    // при eacces у root родителя stat не подтверждает каталог, идём
    // к кэшу sudo листинга или фолбэку через find ниже
    const bool statDenied = (ec == std::errc::permission_denied);
    if (!isDir && !statDenied) {
        QProcess checkProc;
        const QStringList checkArgv = Core::XdgUtils::rootArgs({"test", "-d", QString::fromUtf8(dirPath.c_str())});
        checkProc.start(checkArgv.first(), checkArgv.mid(1));
        if (checkProc.waitForFinished(500) && checkProc.exitCode() == 0) {
            isDir = true;
        }
    }
    if (!isDir && !statDenied) {
        QMetaObject::invokeMethod(qApp, [self, scanCounter, scanId]() {
            if (scanCounter->load() != scanId) return;
            if (!self) return;
            self->beginResetModel();
            self->m_allItems.clear();
            self->m_displayedItems.clear();
            self->endResetModel();
            self->m_isLoading = false;
            self->m_dirScanPending = false;
            emit self->loadingFinished(0);
        }, Qt::QueuedConnection);
        return;
    }

    std::vector<FileItem> gathered;
    gathered.reserve(256);

    // increment(ec) вместо range for: бросающий overload в пуле дал бы std::terminate
    std::error_code dirEc;
    std::filesystem::directory_iterator dirIt(dirPath,
        std::filesystem::directory_options::skip_permission_denied, dirEc);
    const std::filesystem::directory_iterator dirEnd;
    for (; !dirEc && dirIt != dirEnd; dirIt.increment(dirEc)) {
        if (scanCounter->load() != scanId) {
            return;
        }
        const auto& entry = *dirIt;

        FileItem item;
        item.path = entry.path();
        item.filename = QString::fromUtf8(entry.path().filename().c_str());

        std::error_code entryEc;
        auto status = entry.symlink_status(entryEc);
        item.isSymlink = std::filesystem::is_symlink(status);
        auto targetStatus = item.isSymlink ? entry.status(entryEc) : status;
        item.isDirectory = std::filesystem::is_directory(targetStatus);
        if (item.isSymlink) {
            std::error_code linkEc;
            auto target = std::filesystem::read_symlink(item.path, linkEc);
            if (!linkEc) item.symlinkTarget = QString::fromUtf8(target.c_str());
        }

        std::string fname = item.path.filename().string();
        item.isHidden = (!fname.empty() && fname.front() == '.');

        // панель превью рисует только иконку и имя, поэтому не делаем лишний
        // statx и не форматируем размер, права и дату. так листинг больших
        // папок заметно ускоряется.
        if (lightweight) {
            // бит исполнения уже есть в symlink_status, поэтому сохраняем
            // иконку без лишнего stat.
            using perms = std::filesystem::perms;
            item.isExecutable = !item.isDirectory &&
                ((status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none);
            auto iconInfo = Icons::getIcon(item.path, item.isDirectory, item.isSymlink, false, item.isExecutable);
            item.iconGlyph = iconInfo.glyph;
            item.iconColor = iconInfo.color;
            gathered.push_back(std::move(item));
            continue;
        }

        item.permissions = Core::XdgUtils::formatPermissions(status.permissions(), item.isDirectory);
        using perms = std::filesystem::perms;
        item.isExecutable = !item.isDirectory && ((status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none);

        if (!item.isDirectory) {
            item.sizeBytes = std::filesystem::is_regular_file(targetStatus) ? entry.file_size(entryEc) : 0;
            item.sizeFormatted = Core::XdgUtils::formatSize(item.sizeBytes);
        } else {
            item.sizeBytes = 0;
            item.sizeFormatted = "";
        }

        // дата изменения через statx с fallback на mtime, формат %d.%m %h:%m
        time_t timestamp = 0;
        struct statx stx;
        if (statx(AT_FDCWD, item.path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MTIME, &stx) == 0) {
            if ((stx.stx_mask & STATX_MTIME) && stx.stx_mtime.tv_sec != 0) {
                timestamp = stx.stx_mtime.tv_sec;
            }
        }

        if (timestamp == 0 && !entryEc) {
            auto ftime = entry.last_write_time(entryEc);
            if (!entryEc) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                timestamp = std::chrono::system_clock::to_time_t(sctp);
            }
        }

        item.modifiedTimestamp = timestamp;
        if (timestamp != 0) {
            std::tm tmVal;
            localtime_r(&timestamp, &tmVal);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmVal);
            item.modifiedDate = QString::fromUtf8(buf);
        }

        // иконка nerd font (передаём подсказку isexecutable, чтобы пропустить лишний вызов stat)
        auto iconInfo = Icons::getIcon(item.path, item.isDirectory, item.isSymlink, false, item.isExecutable);
        item.iconGlyph = iconInfo.glyph;
        item.iconColor = iconInfo.color;

        gathered.push_back(std::move(item));
    }

    if (gathered.empty()) {
        // листинг из общего кэша xdgutils, добытого одним вызовом root:
        // pkexec уже отработал, второй диалог пароля запрещён;
        // даже пустой вывод валиден, фолбэк не запускаем
        QString sudoOut;
        const bool haveSudoListing = Core::XdgUtils::sudoListing(dirPath, &sudoOut);
        if (haveSudoListing) {
            parseFindListing(sudoOut, dirPath, scanId, scanCounter, gathered);
        } else {
            // тихий фолбэк для каталогов закрытых для root через sudo без диалога
            // работает при живом тикете и не показывает диалогов
            QProcess proc;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("LC_ALL", "C.UTF-8");
            proc.setProcessEnvironment(env);
            // разделитель записей nul: имя файла может содержать '\n',
            // и с '\n' оно инжектировало бы поддельные записи, '\0' в имени невозможен
            const QStringList findArgv = Core::XdgUtils::rootArgs(
                {"find", QString::fromUtf8(dirPath.c_str()), "-mindepth", "1", "-maxdepth", "1", "-printf", findPrintfFormat()});
            proc.start(findArgv.first(), findArgv.mid(1));
            // exit != 0 не отменяет stdout: find может выдать листинг и упасть лишь на отдельных нечитаемых записях
            if (proc.waitForFinished(3000)) {
                const QByteArray sudoStdout = proc.readAllStandardOutput();
                parseFindListing(QString::fromUtf8(sudoStdout), dirPath, scanId, scanCounter, gathered);
                // чистый ответ (exit 0) кэшируем для последующих refresh'ов,
                // отказ не кэшируется: пустота при отказе не пустой каталог
                if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                    Core::XdgUtils::provideSudoListing(dirPath, QString::fromUtf8(sudoStdout));
                }
            } else if (proc.state() != QProcess::NotRunning) {
                proc.kill();
                proc.waitForFinished(300);
            }
        }
    }

    QMetaObject::invokeMethod(qApp, [self, scanCounter, scanId, dirPath, results = std::move(gathered)]() mutable {
        if (scanCounter->load() != scanId) return;
        if (!self) return;

        self->m_allItems = std::move(results);
        for (auto& item : self->m_allItems) {
            item.isSelected = (self->m_selectedPaths.find(item.path.string()) != self->m_selectedPaths.end());
        }

        self->applyFilterAndSort();
        self->m_isLoading = false;
        self->m_dirScanPending = false;
        emit self->directoryLoaded(dirPath);
        emit self->loadingFinished(static_cast<int>(self->m_displayedItems.size()));
    }, Qt::QueuedConnection);
}

// разбор вывода find с printf формат общий для обычного скана и root
// разделитель nul против переводов строк в именах
// false если скан устарел
bool FileSystemModel::parseFindListing(const QString& out, const std::filesystem::path& dirPath,
                                       uint64_t scanId, const std::shared_ptr<std::atomic<uint64_t>>& scanCounter,
                                       std::vector<FileItem>& gathered) {
    const QChar recordSep = out.contains(QChar(u'\0')) ? QChar(u'\0') : QChar(u'\n');
    const QStringList lines = out.split(recordSep, Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        if (scanCounter->load() != scanId) return false;
        auto parts = line.split('\t');
        if (parts.size() < 5) continue;

        QString type = parts[0];
        uintmax_t sizeBytes = parts[1].toULongLong();
        double tsSec = parts[2].toDouble();
        time_t timestamp = static_cast<time_t>(tsSec);
        bool modeOk = false;
        uint octalMode = parts[3].toUInt(&modeOk, 8);
        QString fname = parts.mid(4).join('\t');

        FileItem item;
        item.path = dirPath / fname.toUtf8().toStdString();
        item.filename = fname;
        item.isDirectory = (type == "d");
        item.isSymlink = (type == "l");
        item.isHidden = (!fname.isEmpty() && fname.front() == '.');
        item.sizeBytes = item.isDirectory ? 0 : sizeBytes;
        item.sizeFormatted = item.isDirectory ? "" : Core::XdgUtils::formatSize(item.sizeBytes);
        item.modifiedTimestamp = timestamp;
        if (timestamp != 0) {
            std::tm tmVal;
            localtime_r(&timestamp, &tmVal);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmVal);
            item.modifiedDate = QString::fromUtf8(buf);
        }
        if (modeOk) {
            auto p = static_cast<std::filesystem::perms>(octalMode);
            item.permissions = Core::XdgUtils::formatPermissions(p, item.isDirectory);
            item.isExecutable = !item.isDirectory && ((octalMode & 0111) != 0);
        } else {
            item.permissions = (item.isDirectory ? "d" : "-") + QString("rwxr-xr-x");
        }
        auto iconInfo = Icons::getIcon(item.path, item.isDirectory, item.isSymlink, false, item.isExecutable);
        item.iconGlyph = iconInfo.glyph;
        item.iconColor = iconInfo.color;
        gathered.push_back(std::move(item));
    }
    return true;
}

// сырой вывод авторизованного find кладём в общий кэш xdgutils,
// следующий скан каталога получит его без повторного вызова
void FileSystemModel::provideSudoListing(const std::filesystem::path& dir, const QString& findOutput) {
    Core::XdgUtils::provideSudoListing(dir, findOutput);
}

// \\0 в литерале это backslash+ноль, find превращает его в разделитель
// записей nul; реальный байт nul в argv недопустим, execve обрезает аргумент
QString FileSystemModel::findPrintfFormat() {
    return QStringLiteral("%y\t%s\t%T@\t%m\t%f\\0");
}

// разбор размера: "10m", "500k", "1.5g", "1024"
static uintmax_t parseSizeBytes(const QString& str) {
    QString s = str.trimmed().toUpper();
    uintmax_t multiplier = 1;
    if (s.endsWith("B")) s.chop(1);
    if (s.endsWith("K")) { multiplier = 1024ULL; s.chop(1); }
    else if (s.endsWith("M")) { multiplier = 1024ULL * 1024ULL; s.chop(1); }
    else if (s.endsWith("G")) { multiplier = 1024ULL * 1024ULL * 1024ULL; s.chop(1); }
    else if (s.endsWith("T")) { multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL; s.chop(1); }

    bool ok = false;
    double val = s.toDouble(&ok);
    if (ok && val >= 0) {
        return static_cast<uintmax_t>(val * multiplier);
    }
    return 0;
}

// подсчёт fuzzy очков по подстроке с бонусами за границы слов
static int calculateFuzzyScore(const QString& pattern, const QString& target) {
    if (pattern.isEmpty()) return 100;
    if (target.isEmpty()) return 0;

    QString p = pattern.toLower();
    QString t = target.toLower();

    int pIdx = 0;
    int pLen = p.length();
    int tLen = t.length();

    int score = 0;
    int consecutive = 0;

    for (int tIdx = 0; tIdx < tLen && pIdx < pLen; ++tIdx) {
        if (t[tIdx] == p[pIdx]) {
            pIdx++;
            consecutive++;
            score += 10 + (consecutive * 5);
            if (tIdx == 0 || t[tIdx - 1] == '_' || t[tIdx - 1] == '-' || t[tIdx - 1] == '.' || t[tIdx - 1] == '/') {
                score += 20; // бонус за границу слова
            }
        } else {
            consecutive = 0;
        }
    }

    if (pIdx == pLen) {
        score -= (tLen - pLen);
        return std::max(1, score);
    }

    return 0;
}

struct SearchQuery {
    enum TermType {
        MatchAll,      // "*"
        DirOnly,       // "dir:", "folder:", "/"
        FileOnly,      // "file:"
        Extension,     // "ext:png,jpg"
        SizeGreater,   // ">10m", "size:>10m"
        SizeLess,      // "<500k", "size:<500k"
        ExactPhrase,   // "\"exact phrase\""
        Prefix,        // "^prefix"
        Suffix,        // "suffix$"
        Regex,         // "/pattern/"
        Wildcard,      // "*.txt", "*test*"
        TextOrFuzzy    // обычный текст
    };

    struct Term {
        TermType type{TextOrFuzzy};
        bool isNegated{false};
        QString text;
        QRegularExpression regex;
        uintmax_t sizeThreshold{0};
        QStringList extensions;
    };

    struct OrGroup {
        std::vector<Term> terms;
    };

    std::vector<OrGroup> groups;
    bool isEmpty{true};

    static SearchQuery parse(const QString& raw) {
        SearchQuery q;
        QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) {
            q.isEmpty = true;
            return q;
        }
        q.isEmpty = false;

        // разбиение по " | " или " or "
        static const QRegularExpression orSplitRx(R"(\s+(?:\||OR)\s+)", QRegularExpression::CaseInsensitiveOption);
        QStringList groupStrings = trimmed.split(orSplitRx);

        for (const auto& gStr : groupStrings) {
            QString s = gStr.trimmed();
            if (s.isEmpty()) continue;

            OrGroup group;
            QStringList tokens;
            bool inQuote = false;
            QString curToken;

            for (int i = 0; i < s.length(); ++i) {
                QChar ch = s[i];
                if (ch == '"') {
                    inQuote = !inQuote;
                    curToken += ch;
                } else if (ch.isSpace() && !inQuote) {
                    if (!curToken.isEmpty()) {
                        tokens.append(curToken);
                        curToken.clear();
                    }
                } else {
                    curToken += ch;
                }
            }
            if (!curToken.isEmpty()) {
                tokens.append(curToken);
            }

            for (QString tok : tokens) {
                if (tok.isEmpty()) continue;
                Term term;
                if (tok.startsWith('!') || (tok.startsWith('-') && tok.length() > 1 && tok[1] != '-')) {
                    term.isNegated = true;
                    tok = tok.mid(1);
                }

                if (tok == "*") {
                    term.type = MatchAll;
                } else if (tok.startsWith('"') && tok.endsWith('"') && tok.length() >= 2) {
                    term.type = ExactPhrase;
                    term.text = tok.mid(1, tok.length() - 2);
                } else if (tok.startsWith('/') && tok.endsWith('/') && tok.length() >= 2) {
                    term.type = Regex;
                    term.regex = QRegularExpression(tok.mid(1, tok.length() - 2), QRegularExpression::CaseInsensitiveOption);
                } else if (tok.compare("dir:", Qt::CaseInsensitive) == 0 ||
                           tok.compare("folder:", Qt::CaseInsensitive) == 0 ||
                           tok == "/") {
                    term.type = DirOnly;
                } else if (tok.compare("file:", Qt::CaseInsensitive) == 0) {
                    term.type = FileOnly;
                } else if (tok.startsWith("ext:", Qt::CaseInsensitive)) {
                    term.type = Extension;
                    QString exts = tok.mid(4).toLower();
                    term.extensions = exts.split(',', Qt::SkipEmptyParts);
                    for (auto& e : term.extensions) {
                        if (e.startsWith('.')) e = e.mid(1);
                    }
                } else if (tok.startsWith("size:>", Qt::CaseInsensitive)) {
                    term.type = SizeGreater;
                    term.sizeThreshold = parseSizeBytes(tok.mid(6));
                } else if (tok.startsWith("size:<", Qt::CaseInsensitive)) {
                    term.type = SizeLess;
                    term.sizeThreshold = parseSizeBytes(tok.mid(6));
                } else if (tok.startsWith('>')) {
                    term.type = SizeGreater;
                    term.sizeThreshold = parseSizeBytes(tok.mid(1));
                } else if (tok.startsWith('<')) {
                    term.type = SizeLess;
                    term.sizeThreshold = parseSizeBytes(tok.mid(1));
                } else if (tok.startsWith('^') && tok.length() > 1) {
                    term.type = Prefix;
                    term.text = tok.mid(1);
                } else if (tok.endsWith('$') && tok.length() > 1) {
                    term.type = Suffix;
                    term.text = tok.left(tok.length() - 1);
                } else if (tok.contains('*') || tok.contains('?')) {
                    term.type = Wildcard;
                    term.regex = QRegularExpression::fromWildcard(tok, Qt::CaseInsensitive);
                } else {
                    term.type = TextOrFuzzy;
                    term.text = tok;
                }

                group.terms.push_back(term);
            }

            if (!group.terms.empty()) {
                q.groups.push_back(std::move(group));
            }
        }

        if (q.groups.empty()) {
            q.isEmpty = true;
        }

        return q;
    }

    bool matches(const FileItem& item, int& outScore) const {
        if (isEmpty) {
            outScore = 100;
            return true;
        }

        for (const auto& group : groups) {
            bool groupPassed = true;
            int groupScore = 0;
            int positiveTermCount = 0;

            for (const auto& term : group.terms) {
                bool termMatched = false;
                int currentScore = 0;

                switch (term.type) {
                    case MatchAll: {
                        termMatched = true;
                        currentScore = 100;
                        break;
                    }
                    case DirOnly: {
                        termMatched = item.isDirectory;
                        currentScore = 50;
                        break;
                    }
                    case FileOnly: {
                        termMatched = !item.isDirectory;
                        currentScore = 50;
                        break;
                    }
                    case Extension: {
                        if (!item.isDirectory) {
                            QString fileExt = QString::fromUtf8(item.path.extension().c_str()).toLower();
                            if (fileExt.startsWith('.')) fileExt = fileExt.mid(1);
                            termMatched = term.extensions.contains(fileExt);
                            currentScore = 120;
                        }
                        break;
                    }
                    case SizeGreater: {
                        termMatched = (!item.isDirectory && item.sizeBytes >= term.sizeThreshold);
                        currentScore = 50;
                        break;
                    }
                    case SizeLess: {
                        termMatched = (!item.isDirectory && item.sizeBytes <= term.sizeThreshold);
                        currentScore = 50;
                        break;
                    }
                    case ExactPhrase: {
                        termMatched = item.filename.contains(term.text, Qt::CaseInsensitive);
                        currentScore = 150;
                        break;
                    }
                    case Prefix: {
                        termMatched = item.filename.startsWith(term.text, Qt::CaseInsensitive);
                        currentScore = 130;
                        break;
                    }
                    case Suffix: {
                        termMatched = item.filename.endsWith(term.text, Qt::CaseInsensitive);
                        currentScore = 130;
                        break;
                    }
                    case Regex: {
                        termMatched = term.regex.match(item.filename).hasMatch();
                        currentScore = 150;
                        break;
                    }
                    case Wildcard: {
                        termMatched = term.regex.match(item.filename).hasMatch();
                        currentScore = 130;
                        break;
                    }
                    case TextOrFuzzy: {
                        if (item.filename.contains(term.text, Qt::CaseInsensitive)) {
                            termMatched = true;
                            currentScore = 100;
                            if (item.filename.startsWith(term.text, Qt::CaseInsensitive)) {
                                currentScore += 50;
                            }
                        } else {
                            int fz = calculateFuzzyScore(term.text, item.filename);
                            if (fz > 0) {
                                termMatched = true;
                                currentScore = fz;
                            }
                        }
                        break;
                    }
                }

                if (term.isNegated) {
                    if (termMatched) {
                        groupPassed = false;
                        break; // отрицание не прошло, элемент исключается
                    }
                } else {
                    if (!termMatched) {
                        groupPassed = false;
                        break; // положительное условие не прошло
                    }
                    groupScore += currentScore;
                    positiveTermCount++;
                }
            }

            if (groupPassed) {
                outScore = (positiveTermCount == 0) ? 100 : groupScore;
                return true;
            }
        }

        return false;
    }
};

void FileSystemModel::applyFilterAndSort() {
    beginResetModel();
    m_displayedItems.clear();

    struct ScoredItem {
        FileItem item;
        int score{0};
    };
    std::vector<ScoredItem> matches;
    matches.reserve(m_allItems.size());

    SearchQuery query = SearchQuery::parse(m_filter);

    for (const auto& item : m_allItems) {
        if (!m_showHidden && item.isHidden) {
            continue;
        }

        int score = 0;
        if (query.matches(item, score)) {
            FileItem matchedItem = item;
            matchedItem.fuzzyScore = score;
            matches.push_back({std::move(matchedItem), score});
        }
    }

    if (!query.isEmpty) {
        // при поиске сортируем по очкам совпадения по убыванию
        std::sort(matches.begin(), matches.end(), [](const ScoredItem& a, const ScoredItem& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.item.isDirectory != b.item.isDirectory) {
                return a.item.isDirectory > b.item.isDirectory;
            }
            return QString::compare(a.item.filename, b.item.filename, Qt::CaseInsensitive) < 0;
        });
    } else {
        // обычная сортировка каталога: сначала каталоги, затем по выбранному режиму
        std::sort(matches.begin(), matches.end(), [this](const ScoredItem& a, const ScoredItem& b) {
            if (a.item.isDirectory != b.item.isDirectory) {
                return a.item.isDirectory > b.item.isDirectory;
            }
            if (m_sortMode == 1) { // размер (у каталогов реальный вес из кэша du)
                const qulonglong sa = sortSizeOf(a.item);
                const qulonglong sb = sortSizeOf(b.item);
                if (sa != sb) {
                    return m_sortAscending ? (sa < sb) : (sa > sb);
                }
            } else if (m_sortMode == 2) { // дата
                if (a.item.modifiedTimestamp != b.item.modifiedTimestamp) {
                    return m_sortAscending ? (a.item.modifiedTimestamp < b.item.modifiedTimestamp)
                                           : (a.item.modifiedTimestamp > b.item.modifiedTimestamp);
                }
            } else if (m_sortMode == 3) { // type (расширение, регистронезависимо)
                const int tcmp = QString::compare(sortTypeOf(a.item), sortTypeOf(b.item),
                                                  Qt::CaseInsensitive);
                if (tcmp != 0) {
                    return m_sortAscending ? (tcmp < 0) : (tcmp > 0);
                }
            }
            // режим 0: имя (без учёта регистра)
            int cmp = QString::compare(a.item.filename, b.item.filename, Qt::CaseInsensitive);
            if (cmp != 0) {
                return m_sortAscending ? (cmp < 0) : (cmp > 0);
            }
            return a.item.filename < b.item.filename;
        });
    }

    m_displayedItems.reserve(matches.size());
    for (auto& sm : matches) {
        m_displayedItems.push_back(std::move(sm.item));
    }

    endResetModel();

    // досчитываем веса каталогов (du) и пересортируем, когда придут
    if (m_sortMode == 1) ensureDirSizesForSort();
}

qulonglong FileSystemModel::sortSizeOf(const FileItem& item) const {
    if (!item.isDirectory) return item.sizeBytes;
    const auto it = m_dirSizeCache.constFind(QString::fromUtf8(item.path.c_str()));
    return it == m_dirSizeCache.constEnd() ? 0 : *it;
}

// расширение без точки в нижнем регистре,
// у «.bashrc» расширения нет, каталоги выше файлов и так
QString FileSystemModel::sortTypeOf(const FileItem& item) const {
    if (item.isDirectory) return QString();
    const std::filesystem::path p(item.path);
    return QString::fromUtf8(p.extension().string().c_str()).toLower();
}

// du по виртуальным фс ядра бессмыслен (/proc/kcore ~128 тиб), сортируем как 0;
// паритет с propertiesdialog::isvirtualkernelfs
static bool isVirtualKernelFsPath(const QString& path) {
    const QStorageInfo si(path);
    if (!si.isValid()) return false;
    const QByteArray fs = si.fileSystemType();
    return fs == "proc" || fs == "sysfs" || fs == "devtmpfs" || fs == "devpts"
        || fs == "efivarfs" || fs == "cgroup" || fs == "cgroup2" || fs == "pstore"
        || fs == "securityfs" || fs == "debugfs" || fs == "tracefs"
        || fs == "configfs" || fs == "fusectl" || fs == "mqueue"
        || fs == "hugetlbfs";
}

void FileSystemModel::ensureDirSizesForSort() {
    QStringList toScan;
    for (const auto& item : m_displayedItems) {
        if (!item.isDirectory) continue;
        const QString key = QString::fromUtf8(item.path.c_str());
        if (m_dirSizeCache.contains(key) || m_dirSizePending.contains(key)) continue;
        m_dirSizePending.insert(key);
        toScan << key;
    }
    if (toScan.isEmpty()) return;

    QPointer<FileSystemModel> self(this);
    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("io"))->start([self, toScan]() {
        QHash<QString, qulonglong> results;
        // один du на пачку каталогов чтобы не плодить процессы
        // ключ x не переходит на другие файловые системы
        constexpr int kChunk = 256;
        for (int start = 0; start < toScan.size(); start += kChunk) {
            QStringList args{QStringLiteral("-s"), QStringLiteral("-b"),
                             QStringLiteral("-x"), QStringLiteral("--")};
            for (int i = start; i < start + kChunk && i < toScan.size(); ++i) {
                // du b дает кажущийся размер псевдофайлов и это ломает сортировку по размеру
                if (isVirtualKernelFsPath(toScan[i])) continue;
                args << toScan[i];
            }
            if (args.size() == 4) continue; // в чанке только виртуальные фс
            QProcess proc;
            proc.start(QStringLiteral("du"), args);
            if (!proc.waitForStarted(2000)) continue;
            if (!proc.waitForFinished(120000)) {
                proc.kill();
                proc.waitForFinished(500);
            }
            // exit != 0 не отменяет stdout, часть каталогов нечитаема
            const QString out = QString::fromUtf8(proc.readAllStandardOutput());
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const int tab = line.indexOf('\t');
                if (tab <= 0) continue;
                bool ok = false;
                const qulonglong size = line.left(tab).toULongLong(&ok);
                if (!ok) continue;
                results.insert(line.mid(tab + 1), size);
            }
        }
        // нечитаемые каталоги кешируем как 0, чтобы не гонять du повторно
        for (const QString& key : toScan) {
            if (!results.contains(key)) results.insert(key, 0);
        }
        QMetaObject::invokeMethod(qApp, [self, results]() {
            if (!self) return;
            self->applyDirSizes(results);
        }, Qt::QueuedConnection);
    });
}

void FileSystemModel::applyDirSizes(const QHash<QString, qulonglong>& sizes) {
    for (auto it = sizes.constBegin(); it != sizes.constEnd(); ++it) {
        m_dirSizeCache.insert(it.key(), it.value());
        m_dirSizePending.remove(it.key());
    }
    if (m_sortMode == 1 && !m_displayedItems.empty()) {
        applyFilterAndSort();
    }
}

void FileSystemModel::scanRecursiveAsync(std::filesystem::path dirPath, QString pattern, uint64_t scanId,
                                         std::shared_ptr<std::atomic<uint64_t>> scanCounter, bool showHidden,
                                         QPointer<FileSystemModel> self) {
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec)) {
        return;
    }

    struct ScoredMatch {
        FileItem item;
        int score{0};
    };
    std::vector<ScoredMatch> matches;

    // ограничения для быстрой отзывчивой индексации:
    constexpr int kMaxDepth = 5;
    constexpr int kMaxVisited = 25000;
    constexpr size_t kMaxMatches = 600;

    SearchQuery query = SearchQuery::parse(pattern);
    int visitedCount = 0;
    auto it = std::filesystem::recursive_directory_iterator(
        dirPath, std::filesystem::directory_options::skip_permission_denied, ec);
    auto endIt = std::filesystem::recursive_directory_iterator();

    while (!ec && it != endIt) {
        if (scanCounter->load() != scanId) {
            return;
        }

        if (++visitedCount > kMaxVisited || matches.size() >= kMaxMatches) {
            break;
        }

        if (it.depth() > kMaxDepth) {
            it.pop(ec);
            continue;
        }

        const auto& entry = *it;
        std::string fname = entry.path().filename().string();
        bool isHidden = (!fname.empty() && fname.front() == '.');

        // пропускаем рекурсию в скрытые каталоги, не выталкивая родителя
        if (!showHidden && isHidden) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }

        if (it.depth() == kMaxDepth && entry.is_directory(ec)) {
            it.disable_recursion_pending();
        }

        // относительный путь к текущему каталогу
        std::error_code relEc;
        auto rel = std::filesystem::relative(entry.path(), dirPath, relEc);
        QString fnameQ = QString::fromUtf8(fname.c_str());
        QString relStr = relEc ? fnameQ : QString::fromUtf8(rel.c_str());

        std::error_code entryEc;
        auto status = entry.symlink_status(entryEc);
        bool isSymlink = std::filesystem::is_symlink(status);
        auto targetStatus = isSymlink ? entry.status(entryEc) : status;
        bool isDir = std::filesystem::is_directory(targetStatus);
        uintmax_t sizeBytes = (!isDir && std::filesystem::is_regular_file(targetStatus)) ? entry.file_size(entryEc) : 0;

        FileItem tempItem;
        tempItem.path = entry.path();
        tempItem.filename = fnameQ;
        tempItem.isSymlink = isSymlink;
        tempItem.isDirectory = isDir;
        tempItem.isHidden = isHidden;
        tempItem.sizeBytes = sizeBytes;

        int score = 0;
        bool isMatch = query.matches(tempItem, score);
        if (!isMatch && relStr != fnameQ) {
            tempItem.filename = relStr;
            isMatch = query.matches(tempItem, score);
        }

        if (isMatch) {
            FileItem item;
            item.path = entry.path();
            item.filename = relStr;
            item.isSymlink = isSymlink;
            item.isDirectory = isDir;
            item.isHidden = isHidden;
            item.fuzzyScore = score;
            if (item.isSymlink) {
                std::error_code linkEc;
                auto target = std::filesystem::read_symlink(item.path, linkEc);
                if (!linkEc) item.symlinkTarget = QString::fromUtf8(target.c_str());
            }

            item.permissions = Core::XdgUtils::formatPermissions(status.permissions(), item.isDirectory);
            using perms = std::filesystem::perms;
            item.isExecutable = !item.isDirectory && ((status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none);

            item.sizeBytes = sizeBytes;
            item.sizeFormatted = item.isDirectory ? "" : Core::XdgUtils::formatSize(item.sizeBytes);

            // метка времени через statx с fallback на mtime
            time_t timestamp = 0;
            struct statx stx;
            if (statx(AT_FDCWD, item.path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MTIME, &stx) == 0) {
                if ((stx.stx_mask & STATX_MTIME) && stx.stx_mtime.tv_sec != 0) {
                    timestamp = stx.stx_mtime.tv_sec;
                }
            }

            if (timestamp == 0 && !entryEc) {
                auto ftime = entry.last_write_time(entryEc);
                if (!entryEc) {
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                    timestamp = std::chrono::system_clock::to_time_t(sctp);
                }
            }

            item.modifiedTimestamp = timestamp;
            if (timestamp != 0) {
                std::tm tmVal;
                localtime_r(&timestamp, &tmVal);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmVal);
                item.modifiedDate = QString::fromUtf8(buf);
            }

            auto iconInfo = Icons::getIcon(item.path, item.isDirectory, item.isSymlink, false, item.isExecutable);
            item.iconGlyph = iconInfo.glyph;
            item.iconColor = iconInfo.color;

            matches.push_back({std::move(item), score});
        }

        it.increment(ec);
    }

    if (scanCounter->load() != scanId) {
        return;
    }

    // сортировка по fuzzy очкам по убыванию
    std::sort(matches.begin(), matches.end(), [](const ScoredMatch& a, const ScoredMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.item.isDirectory != b.item.isDirectory) return a.item.isDirectory > b.item.isDirectory;
        return QString::compare(a.item.filename, b.item.filename, Qt::CaseInsensitive) < 0;
    });

    std::vector<FileItem> finalItems;
    finalItems.reserve(matches.size());
    for (auto& m : matches) {
        finalItems.push_back(std::move(m.item));
    }

    QMetaObject::invokeMethod(qApp, [self, scanCounter, scanId, results = std::move(finalItems)]() mutable {
        if (scanCounter->load() != scanId) return;
        if (!self) return;
        for (auto& item : results) {
            item.isSelected = (self->m_selectedPaths.find(item.path.string()) != self->m_selectedPaths.end());
        }
        self->beginResetModel();
        self->m_displayedItems = std::move(results);
        self->endResetModel();
        self->m_isLoading = false;
        emit self->loadingFinished(static_cast<int>(self->m_displayedItems.size()));
    }, Qt::QueuedConnection);
}

// принудительная перефильтрация даже при том же тексте фильтра
void FileSystemModel::setShowHidden(bool show) {
    if (m_showHidden != show) {
        m_showHidden = show;
        if (m_filter.trimmed().isEmpty()) {
            applyFilterAndSort();
        } else {
            QString curFilter = m_filter;
            m_filter.clear();
            setFilter(curFilter);
        }
    }
}

void FileSystemModel::stopGlobalProcess() {
    if (!m_globalProcess) return;
    m_globalProcess->cancel();
    m_globalProcess->deleteLater();
    m_globalProcess = nullptr;
}

void FileSystemModel::applyGlobalResults(std::vector<FileItem> items) {
    beginResetModel();
    m_displayedItems = std::move(items);
    for (auto& item : m_displayedItems) {
        item.isSelected = m_selectedPaths.find(item.path.string()) != m_selectedPaths.end();
    }
    endResetModel();
    m_isLoading = false;
    const int count = static_cast<int>(m_displayedItems.size());
    emit loadingFinished(count);
    emit globalSearchFinished(count);
}

void FileSystemModel::clearGlobalSearch() {
    stopGlobalProcess();
    if (!m_globalSearch) return;
    m_globalSearch = false;
    if (m_searchCancel) m_searchCancel->store(true);
    ++(*m_scanCounter);
    applyFilterAndSort();
    if (m_isLoading.exchange(false)) {
        emit loadingFinished(static_cast<int>(m_displayedItems.size()));
    }
}

void FileSystemModel::searchEverywhere(const QString& query) {
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        clearGlobalSearch();
        return;
    }

    stopGlobalProcess();
    m_globalSearch = true;
    m_filter.clear();
    ++(*m_scanCounter);
    if (m_searchCancel) m_searchCancel->store(true);
    m_searchCancel = std::make_shared<std::atomic_bool>(false);
    const uint64_t scanId = *m_scanCounter;
    auto cancel = m_searchCancel;
    auto scanCounter = m_scanCounter;
    QPointer<FileSystemModel> self(this);
    const bool showHidden = m_showHidden;
    const auto roots = globalSearchRoots();
    beginResetModel();
    m_displayedItems.clear();
    endResetModel();
    m_globalSeen.clear();
    m_isLoading = true;
    emit loadingStarted();

    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("search"))->start(
        [self, scanId, scanCounter, cancel, needle, showHidden, roots]() {
            std::vector<FileItem> items;
            const QString fd = Core::Tools::findTool(QStringLiteral("fd"));
            const QString fdfind = fd.isEmpty() ? Core::Tools::findTool(QStringLiteral("fdfind")) : QString();
            const QString tool = fd.isEmpty() ? fdfind : fd;
            if (!tool.isEmpty()) {
                QStringList args{QStringLiteral("--color"), QStringLiteral("never"),
                                 QStringLiteral("--absolute-path"),
                                 QStringLiteral("--fixed-strings"),
                                 QStringLiteral("--max-results"), QStringLiteral("1000"),
                                 QStringLiteral("--max-depth"), QStringLiteral("24"),
                                 QStringLiteral("--one-file-system")};
                if (showHidden) args << QStringLiteral("--hidden");
                args << QStringLiteral("--") << needle;
                for (const auto& root : roots) args << QString::fromUtf8(root.c_str());
                const auto res = Core::Tools::runSync(tool, args, 20000, 16 * 1024 * 1024, cancel);
                if (res.cancelled || cancel->load() || scanCounter->load() != scanId) return;
                if (res.started && !res.timedOut && res.exitCode >= 0 && res.exitCode <= 1) {
                    QSet<QString> seen;
                    const QList<QByteArray> lines = res.output.split('\n');
                    items.reserve(lines.size());
                    for (const QByteArray& raw : lines) {
                        if (raw.isEmpty()) continue;
                        const QString pathStr = QString::fromUtf8(raw);
                        if (seen.contains(pathStr)) continue;
                        seen.insert(pathStr);
                        const std::filesystem::path path(pathStr.toUtf8().toStdString());
                        std::error_code ec;
                        if (!std::filesystem::exists(path, ec)) continue;
                        FileItem item = globalSearchItem(path);
                        item.fuzzyScore = Core::Search::fuzzyScore(needle, item.filename);
                        items.push_back(std::move(item));
                    }
                }
            }
            if (items.empty()) {
                items = scanNamesFallback(needle, roots, showHidden, cancel, scanId, scanCounter);
            }
            if (cancel->load() || scanCounter->load() != scanId) return;
            std::sort(items.begin(), items.end(), [](const FileItem& a, const FileItem& b) {
                if (a.fuzzyScore != b.fuzzyScore) return a.fuzzyScore > b.fuzzyScore;
                if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
                return QString::compare(a.filename, b.filename, Qt::CaseInsensitive) < 0;
            });
            QMetaObject::invokeMethod(qApp, [self, scanCounter, scanId, items = std::move(items)]() mutable {
                if (!self || scanCounter->load() != scanId) return;
                self->applyGlobalResults(std::move(items));
            }, Qt::QueuedConnection);
        });
}

void FileSystemModel::searchContents(const QString& query) {
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        clearGlobalSearch();
        return;
    }

    stopGlobalProcess();
    m_globalSearch = true;
    m_filter.clear();
    ++(*m_scanCounter);
    if (m_searchCancel) m_searchCancel->store(true);
    m_searchCancel = std::make_shared<std::atomic_bool>(false);
    const uint64_t scanId = *m_scanCounter;

    beginResetModel();
    m_displayedItems.clear();
    endResetModel();
    m_globalSeen.clear();
    m_isLoading = true;
    emit loadingStarted();

    const bool showHidden = m_showHidden;
    const auto roots = globalSearchRoots();
    const QString rg = Core::Tools::findTool(QStringLiteral("rg"));
    QString program;
    QStringList args;
    if (!rg.isEmpty()) {
        program = rg;
        args = {QStringLiteral("--files-with-matches"), QStringLiteral("--no-messages"),
                QStringLiteral("--color"), QStringLiteral("never"),
                QStringLiteral("--line-buffered"),
                QStringLiteral("--max-filesize"), QStringLiteral("8M"),
                QStringLiteral("--one-file-system")};
        if (showHidden) args << QStringLiteral("--hidden");
        args << QStringLiteral("-e") << needle;
    } else {
        program = Core::Tools::findTool(QStringLiteral("grep"));
        args = {QStringLiteral("-rIl"), QStringLiteral("-s"), QStringLiteral("--line-buffered"),
                QStringLiteral("-e"), needle};
    }
    if (program.isEmpty()) {
        applyGlobalResults({});
        return;
    }
    for (const auto& root : roots) args << QString::fromUtf8(root.c_str());

    auto* runner = new Core::Tools::ProcessRunner(this);
    m_globalProcess = runner;
    runner->setProgram(program);
    runner->setArguments(args);
    runner->setRecordSeparator('\n');
    runner->setMaxOutputBytes(4 * 1024 * 1024);
    runner->setTimeoutMs(20000);

    connect(runner, &Core::Tools::ProcessRunner::recordReady, this,
            [this, runner, scanId](const QByteArray& record) {
        if (m_globalProcess != runner || *m_scanCounter != scanId || record.isEmpty()) return;
        const QString pathStr = QString::fromUtf8(record);
        if (m_globalSeen.contains(pathStr)) return;
        if (m_displayedItems.size() >= 1000) {
            runner->cancel();
            return;
        }
        const std::filesystem::path path(pathStr.toUtf8().toStdString());
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return;
        m_globalSeen.insert(pathStr);
        FileItem item = globalSearchItem(path);
        item.fuzzyScore = 100;
        const int row = static_cast<int>(m_displayedItems.size());
        beginInsertRows(QModelIndex(), row, row);
        m_displayedItems.push_back(std::move(item));
        endInsertRows();
    });
    connect(runner, &Core::Tools::ProcessRunner::finished, this,
            [this, runner, scanId](const Core::Tools::ProcessResult&) {
        if (m_globalProcess != runner || *m_scanCounter != scanId) return;
        m_globalProcess = nullptr;
        m_isLoading = false;
        const int count = static_cast<int>(m_displayedItems.size());
        emit loadingFinished(count);
        emit globalSearchFinished(count);
        runner->deleteLater();
    });
    runner->start();
}

void FileSystemModel::setFilter(const QString& filter) {
    if (m_globalSearch) {
        m_globalSearch = false;
        stopGlobalProcess();
        if (m_searchCancel) m_searchCancel->store(true);
        ++(*m_scanCounter);
    }
    if (m_filter == filter) return;
    m_filter = filter;

    if (filter.trimmed().isEmpty()) {
        // старый фоновый скан сам отвалится по scanid, join не нужен
        ++(*m_scanCounter);
        applyFilterAndSort();
        // отменённый скан не доставит результат, иначе ui навсегда оставался бы в loading
        if (m_isLoading.exchange(false)) {
            emit loadingFinished(static_cast<int>(m_displayedItems.size()));
        }
        // m_allitems мог остаться от прошлого каталога, перечитываем текущий
        if (m_dirScanPending) {
            refresh();
        }
        return;
    }

    // сразу фильтруем элементы текущего каталога для мгновенного отклика
    applyFilterAndSort();

    // параллельно запускаем рекурсивный скан подкаталогов
    uint64_t scanId = ++(*m_scanCounter);
    m_isLoading = true;
    emit loadingStarted();

    QPointer<FileSystemModel> self(this);
    std::filesystem::path dir = m_currentDir;
    QString pattern = m_filter;
    auto scanCounter = m_scanCounter;
    const bool showHidden = m_showHidden;
    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("search"))->start([self, dir, pattern, scanId, scanCounter, showHidden]() {
        // статический вызов: воркер не разыменовывает this
        FileSystemModel::scanRecursiveAsync(dir, pattern, scanId, scanCounter, showHidden, self);
    });
}

void FileSystemModel::setSortMode(int mode) {
    // переключение режима применяет дефолтное направление,
    // ручная инверсия живёт до смены режима
    const bool asc = defaultSortAscending(mode);
    if (m_sortMode == mode && m_sortAscending == asc) return;
    m_sortMode = mode;
    m_sortAscending = asc;
    applyFilterAndSort();
}

void FileSystemModel::setSortMode(int mode, bool ascending) {
    if (m_sortMode == mode && m_sortAscending == ascending) return;
    m_sortMode = mode;
    m_sortAscending = ascending;
    applyFilterAndSort();
}

void FileSystemModel::setSortAscending(bool ascending) {
    if (m_sortAscending == ascending) return;
    m_sortAscending = ascending;
    applyFilterAndSort();
}

void FileSystemModel::toggleSortDirection() {
    m_sortAscending = !m_sortAscending;
    applyFilterAndSort();
}

int FileSystemModel::findRowByPath(const std::filesystem::path& path) const {
    if (path.empty()) return -1;

    // 1. быстрое точное совпадение без нормализации строк
    for (size_t i = 0; i < m_displayedItems.size(); ++i) {
        if (m_displayedItems[i].path == path) {
            return static_cast<int>(i);
        }
    }

    std::string targetFilename = path.filename().string();
    std::string targetStr = path.lexically_normal().string();

    // 2. совпадение по нормализованному пути
    for (size_t i = 0; i < m_displayedItems.size(); ++i) {
        if (m_displayedItems[i].path.lexically_normal().string() == targetStr) {
            return static_cast<int>(i);
        }
    }

    // 3. совпадение по имени файла
    for (size_t i = 0; i < m_displayedItems.size(); ++i) {
        if (m_displayedItems[i].path.filename().string() == targetFilename ||
            m_displayedItems[i].path.filename().string() == path.string()) {
            return static_cast<int>(i);
        }
    }

    // 4. совпадение по имени файла без учёта регистра
    QString targetFilenameQ = QString::fromUtf8(targetFilename.c_str());
    for (size_t i = 0; i < m_displayedItems.size(); ++i) {
        if (QString::compare(m_displayedItems[i].filename, targetFilenameQ, Qt::CaseInsensitive) == 0) {
            return static_cast<int>(i);
        }
    }

    // 5. проверка эквивалентности средствами фс (только если цель есть на диске, чтобы не делать o(n) падающих вызовов stat)
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        for (size_t i = 0; i < m_displayedItems.size(); ++i) {
            if (std::filesystem::equivalent(m_displayedItems[i].path, path, ec)) {
                return static_cast<int>(i);
            }
        }
    }

    return -1;
}

void FileSystemModel::toggleSelection(int row) {
    if (row < 0 || row >= static_cast<int>(m_displayedItems.size())) return;
    auto& item = m_displayedItems[row];
    item.isSelected = !item.isSelected;

    if (item.isSelected) {
        m_selectedPaths.insert(item.path.string());
    } else {
        m_selectedPaths.erase(item.path.string());
    }

    QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {IsSelectedRole});
}

void FileSystemModel::setItemSelected(int row, bool selected) {
    if (row < 0 || row >= static_cast<int>(m_displayedItems.size())) return;
    auto& item = m_displayedItems[row];
    if (item.isSelected != selected) {
        item.isSelected = selected;
        if (selected) {
            m_selectedPaths.insert(item.path.string());
        } else {
            m_selectedPaths.erase(item.path.string());
        }
        QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {IsSelectedRole});
    }
}

void FileSystemModel::setSelectedPaths(const std::unordered_set<std::string>& paths) {
    m_selectedPaths = paths;
    for (auto& item : m_allItems) {
        item.isSelected = (m_selectedPaths.find(item.path.string()) != m_selectedPaths.end());
    }
    for (auto& item : m_displayedItems) {
        item.isSelected = (m_selectedPaths.find(item.path.string()) != m_selectedPaths.end());
    }
    if (!m_displayedItems.empty()) {
        emit dataChanged(index(0, 0), index(static_cast<int>(m_displayedItems.size()) - 1, 0), {IsSelectedRole});
    }
}

void FileSystemModel::clearSelection() {
    m_selectedPaths.clear();
    for (size_t i = 0; i < m_displayedItems.size(); ++i) {
        if (m_displayedItems[i].isSelected) {
            m_displayedItems[i].isSelected = false;
            QModelIndex idx = index(static_cast<int>(i), 0);
            emit dataChanged(idx, idx, {IsSelectedRole});
        }
    }
}

std::vector<std::filesystem::path> FileSystemModel::selectedPaths() const {
    std::vector<std::filesystem::path> paths;
    for (const auto& item : m_displayedItems) {
        if (item.isSelected) {
            paths.push_back(item.path);
        }
    }
    return paths;
}

}
