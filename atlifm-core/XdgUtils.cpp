#include <QRegularExpression>
#include "../atlifm-widgets/TuiInputPopup.hpp"
#include <unistd.h>
#include "XdgUtils.hpp"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QMimeDatabase>
#include <QMimeType>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <fstream>
#include <optional>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>
#include <cmath>
#include <cstdlib>

namespace Core {

QString XdgUtils::formatSize(uintmax_t bytes) {
    if (bytes == 0) return "0 B";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 5) {
        size /= 1024.0;
        unitIndex++;
    }

    if (unitIndex == 0) {
        return QString("%1 %2").arg(static_cast<int>(size)).arg(units[unitIndex]);
    }
    return QString("%1 %2").arg(size, 0, 'f', 1).arg(units[unitIndex]);
}

QString XdgUtils::formatPermissions(std::filesystem::perms p, bool isDirectory) {
    using perms = std::filesystem::perms;
    QString result;
    result.reserve(10);
    result.append(isDirectory ? 'd' : '-');

    result.append((p & perms::owner_read) != perms::none ? 'r' : '-');
    result.append((p & perms::owner_write) != perms::none ? 'w' : '-');
    result.append((p & perms::owner_exec) != perms::none ? 'x' : '-');

    result.append((p & perms::group_read) != perms::none ? 'r' : '-');
    result.append((p & perms::group_write) != perms::none ? 'w' : '-');
    result.append((p & perms::group_exec) != perms::none ? 'x' : '-');

    result.append((p & perms::others_read) != perms::none ? 'r' : '-');
    result.append((p & perms::others_write) != perms::none ? 'w' : '-');
    result.append((p & perms::others_exec) != perms::none ? 'x' : '-');

    return result;
}

QString XdgUtils::getMimeType(const std::filesystem::path& path) {
    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForFile(QString::fromUtf8(path.c_str()), QMimeDatabase::MatchDefault);
    if (mime.isValid()) {
        return mime.name();
    }
    return "application/octet-stream";
}

namespace {
std::optional<DesktopApp> findDesktopAppById(const QString& desktopId);
}

std::vector<DesktopApp> XdgUtils::getAppsForFile(const std::filesystem::path& path) {
    std::vector<DesktopApp> apps;
    QString mime = getMimeType(path);

    QStringList searchDirs = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        QDir::homePath() + "/.local/share/applications",
        QDir::homePath() + "/.local/share/flatpak/exports/share/applications",
        "/var/lib/flatpak/exports/share/applications"
    };

    std::unordered_map<std::string, DesktopApp> found;

    for (const auto& dirPath : searchDirs) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;

        std::error_code dirEc;
        // increment ec потому что range for использует бросающий overload
        std::filesystem::directory_iterator it(
            std::filesystem::path(dirPath.toUtf8().toStdString()),
            std::filesystem::directory_options::skip_permission_denied, dirEc);
        const std::filesystem::directory_iterator dirEnd;
        for (; !dirEc && it != dirEnd; it.increment(dirEc)) {
            const auto& entry = *it;
            if (entry.path().extension() == ".desktop") {
                QFile f(QString::fromUtf8(entry.path().c_str()));
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

                QTextStream in(&f);
                QString name;
                QString exec;
                QString icon;
                QStringList mimes;
                bool isApp = false;
                bool noDisplay = false;
                bool inDesktopEntry = false;

                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.isEmpty() || line.startsWith('#')) continue;
                    if (line.startsWith('[') && line.endsWith(']')) {
                        inDesktopEntry = (line == "[Desktop Entry]");
                        continue;
                    }
                    if (!inDesktopEntry) continue;

                    if (line.startsWith("Name=") && name.isEmpty()) {
                        name = line.mid(5);
                    } else if (line.startsWith("Exec=") && exec.isEmpty()) {
                        exec = line.mid(5);
                    } else if (line.startsWith("Icon=") && icon.isEmpty()) {
                        icon = line.mid(5);
                    } else if (line.startsWith("Type=Application")) {
                        isApp = true;
                    } else if (line.startsWith("NoDisplay=true")) {
                        noDisplay = true;
                    } else if (line.startsWith("MimeType=")) {
                        mimes = line.mid(9).split(';', Qt::SkipEmptyParts);
                    }
                }

                if (!isApp || noDisplay || name.isEmpty() || exec.isEmpty()) continue;

                bool matches = false;
                for (const auto& m : mimes) {
                    if (m.trimmed().compare(mime, Qt::CaseInsensitive) == 0) {
                        matches = true;
                        break;
                    }
                }

                if (matches) {
                    DesktopApp app;
                    app.id = QString::fromUtf8(entry.path().filename().c_str());
                    app.name = name;
                    app.execCommand = exec;
                    app.iconName = icon;
                    found[app.name.toUtf8().toStdString()] = std::move(app);
                }
            }
        }
    }

    // GIO знает ассоциации из mimeapps.list, рекомендованные и добавленные вручную
    // приложения, которых нет в MimeType=.desktop (например nvim/zed для .ts).
    const QString gio = QStandardPaths::findExecutable("gio");
    if (!gio.isEmpty()) {
        QProcess proc;
        proc.start(gio, {QStringLiteral("mime"), mime});
        if (proc.waitForFinished(1500) && proc.exitCode() == 0) {
            const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput())
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList tokens = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                                       Qt::SkipEmptyParts);
                for (const QString& token : tokens) {
                    if (!token.endsWith(QLatin1String(".desktop"))) continue;
                    if (auto app = findDesktopAppById(token)) {
                        found[app->name.toUtf8().toStdString()] = std::move(*app);
                    }
                }
            }
        }
    }

    apps.reserve(found.size());
    for (auto& [_, app] : found) {
        apps.push_back(std::move(app));
    }

    std::sort(apps.begin(), apps.end(), [](const DesktopApp& a, const DesktopApp& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    return apps;
}

namespace {

// экранирование строки для подстановки внутрь скрипта sh (одинарные кавычки)
QString shellQuote(const QString& s) {
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// каталоги .desktop: сначала пользовательский, потом системный
const QStringList& desktopAppDirs() {
    static const QStringList dirs = {
        QDir::homePath() + "/.local/share/applications",
        "/usr/local/share/applications",
        "/usr/share/applications",
        QDir::homePath() + "/.local/share/flatpak/exports/share/applications",
        "/var/lib/flatpak/exports/share/applications"
    };
    return dirs;
}

// разбор одного .desktop: Name/Exec/Icon/Type из [Desktop Entry]
std::optional<DesktopApp> parseDesktopAppFile(const std::filesystem::path& filePath) {
    QFile f(QString::fromUtf8(filePath.c_str()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;

    QTextStream in(&f);
    DesktopApp app;
    bool isApp = false;
    bool noDisplay = false;
    bool hidden = false;
    bool inDesktopEntry = false;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            inDesktopEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry) continue;
        if (line.startsWith("Name=") && app.name.isEmpty()) app.name = line.mid(5);
        else if (line.startsWith("Exec=") && app.execCommand.isEmpty()) app.execCommand = line.mid(5);
        else if (line.startsWith("Icon=") && app.iconName.isEmpty()) app.iconName = line.mid(5);
        else if (line.startsWith("Type=Application")) isApp = true;
        else if (line.startsWith("NoDisplay=true")) noDisplay = true;
        else if (line.startsWith("Hidden=true")) hidden = true;
    }

    if (!isApp || noDisplay || hidden || app.name.isEmpty() || app.execCommand.isEmpty()) return std::nullopt;
    app.id = QString::fromUtf8(filePath.filename().c_str());
    return app;
}

std::optional<DesktopApp> findDesktopAppById(const QString& desktopId) {
    if (desktopId.isEmpty()) return std::nullopt;
    // id приходит из внешних файлов xdg mime и mimeapps list и мы проверяем путь
    // за каталоги приложений через '/' или '..'
    if (desktopId.contains('/') || desktopId.contains('\\') ||
        desktopId == "." || desktopId == "..") {
        return std::nullopt;
    }
    for (const QString& dir : desktopAppDirs()) {
        const std::filesystem::path candidate((dir + "/" + desktopId).toStdString());
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        if (auto app = parseDesktopAppFile(candidate)) return app;
    }
    return std::nullopt;
}

}

std::vector<DesktopApp> XdgUtils::getAllApps() {
    std::vector<DesktopApp> apps;
    QSet<QString> seen;
    for (const QString& dirPath : desktopAppDirs()) {
        const QStringList files = QDir(dirPath).entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString& fileName : files) {
            // пользовательский каталог идёт первым и перекрывает системный файл с тем же id
            if (seen.contains(fileName)) continue;
            seen.insert(fileName);
            const std::filesystem::path file((dirPath + QLatin1Char('/') + fileName).toStdString());
            if (auto app = parseDesktopAppFile(file)) apps.push_back(std::move(*app));
        }
    }
    std::sort(apps.begin(), apps.end(), [](const DesktopApp& a, const DesktopApp& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return apps;
}

DesktopApp XdgUtils::getDefaultAppForFile(const std::filesystem::path& path) {
    const QString mime = getMimeType(path);

    // xdg mime авторитетно и учитывает все базы mimeapps и профили
    const QString xdgMime = QStandardPaths::findExecutable("xdg-mime");
    if (!xdgMime.isEmpty()) {
        QProcess proc;
        proc.start(xdgMime, {"query", "default", mime});
        if (proc.waitForFinished(2000) && proc.exitCode() == 0) {
            const QString id = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            if (auto app = findDesktopAppById(id)) return *app;
        }
    }

    // 2) [Default Applications] из mimeapps.list: пользовательские, затем системные
    const QStringList listPaths = {
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/mimeapps.list",
        QDir::homePath() + "/.local/share/applications/mimeapps.list",
        "/usr/share/applications/mimeapps.list"
    };
    for (const QString& listPath : listPaths) {
        QFile f(listPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream in(&f);
        bool inDefaults = false;
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (line.startsWith('[') && line.endsWith(']')) {
                inDefaults = (line == "[Default Applications]");
                continue;
            }
            if (!inDefaults) continue;
            const qsizetype eq = line.indexOf('=');
            if (eq <= 0) continue;
            if (line.left(eq).trimmed().compare(mime, Qt::CaseInsensitive) != 0) continue;
            const QStringList ids = line.mid(eq + 1).split(';', Qt::SkipEmptyParts);
            for (const QString& rawId : ids) {
                if (auto app = findDesktopAppById(rawId.trimmed())) return *app;
            }
            break;
        }
    }

    return DesktopApp{};
}

bool XdgUtils::setDefaultAppForFile(const std::filesystem::path& path,
                                    const DesktopApp& app, QString& outError) {
    outError.clear();
    const QString mime = getMimeType(path);
    if (mime.isEmpty() || app.id.isEmpty()) {
        outError = "cannot determine MIME type or application id";
        return false;
    }

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configDir.isEmpty()) {
        outError = "no config directory";
        return false;
    }
    QDir().mkpath(configDir);
    const QString listPath = configDir + "/mimeapps.list";

    // точечная правка: заменяем или добавляем mime=id в [Default Applications],
    // остальное содержимое не трогаем
    QStringList lines;
    {
        QFile in(listPath);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&in);
            while (!ts.atEnd()) lines << ts.readLine();
        }
    }

    int secStart = -1;
    int secEnd = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (!t.startsWith('[') || !t.endsWith(']')) continue;
        if (secStart >= 0) { secEnd = i; break; }
        if (t == "[Default Applications]") secStart = i;
    }

    const QString entry = mime + "=" + app.id;
    if (secStart < 0) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty()) lines << QString();
        lines << "[Default Applications]" << entry;
    } else {
        bool replaced = false;
        for (int i = secStart + 1; i < secEnd; ++i) {
            const QString t = lines[i].trimmed();
            if (t.startsWith(mime + "=") || t.startsWith(mime + ";")) {
                lines[i] = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) lines.insert(secStart + 1, entry);
    }

    QFile out(listPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        outError = out.errorString();
        return false;
    }
    QTextStream os(&out);
    for (const QString& l : lines) os << l << '\n';
    return true;
}

bool XdgUtils::openWithDefault(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    QString qpath = QString::fromUtf8(path.c_str());
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

    // файл desktop: запуск через gio launch или gtk launch
    if (ext == ".desktop") {
        if (QProcess::startDetached("gio", {"launch", qpath})) return true;
        if (QProcess::startDetached("gtk-launch", {QString::fromUtf8(path.filename().c_str())})) return true;
    }

    // 2. исполняемые бинарники, скрипты, AppImage
    QFileInfo fi(qpath);
    if (fi.isExecutable() && !fi.isDir()) {
        if (ext == ".appimage" || ext == ".sh" || ext == ".bin" || ext == ".run" || ext.empty()) {
            if (QProcess::startDetached(qpath, {}, QString::fromUtf8(path.parent_path().c_str()))) {
                return true;
            }
        }
    }

    // 3. стандартный запускатель приложений FreeDesktop в Linux: gio open
    if (QProcess::startDetached("gio", {"open", qpath})) {
        return true;
    }

    // фолбэк к xdg open
    if (QProcess::startDetached("xdg-open", {qpath})) {
        return true;
    }

    // 5. откат к Qt DesktopServices
    return QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

// выполнение через splitCommand без sh c и без инъекции команд
bool XdgUtils::openWithApp(const DesktopApp& app, const std::filesystem::path& path) {
    QString cmd = app.execCommand;
    QString filePath = QString::fromUtf8(path.c_str());

    QStringList tokens = QProcess::splitCommand(cmd);
    if (tokens.isEmpty()) return false;

    QString program = tokens.takeFirst();
    QStringList args;
    bool replacedPlaceholder = false;

    for (const auto& token : tokens) {
        if (token == "%f" || token == "%u" || token == "%F" || token == "%U") {
            args << filePath;
            replacedPlaceholder = true;
        } else if (token.contains("%f") || token.contains("%u") || token.contains("%F") || token.contains("%U")) {
            QString modToken = token;
            modToken.replace("%f", filePath);
            modToken.replace("%u", filePath);
            modToken.replace("%F", filePath);
            modToken.replace("%U", filePath);
            args << modToken;
            replacedPlaceholder = true;
        } else if (token.startsWith('%')) {
            // отбрасываем устаревшие коды полей desktop entry (%i, %c, %k и т.п.)
            continue;
        } else {
            args << token;
        }
    }

    if (!replacedPlaceholder) {
        args << filePath;
    }

    return QProcess::startDetached(program, args);
}

DesktopApp XdgUtils::appFromFile(const std::filesystem::path& filePath) {
    const QString qpath = QString::fromUtf8(filePath.c_str());
    if (qpath.endsWith(QStringLiteral(".desktop"), Qt::CaseInsensitive)) {
        if (auto app = parseDesktopAppFile(filePath)) return *app;
        return DesktopApp{};
    }
    DesktopApp app;
    app.name = QString::fromUtf8(filePath.filename().c_str());
    app.execCommand = QStringLiteral("\"%1\" %U").arg(qpath);
    app.iconName = QStringLiteral("application-x-executable");
    return app;
}

namespace {

// терминалы, в которых sudo сможет спросить пароль: без tty sudo падает
// или висит («a terminal is required»)
QStringList terminalCandidates() {
    QStringList terms = {"ghostty", "kitty", "alacritty", "foot", "wezterm",
                         "konsole", "gnome-terminal", "xfce4-terminal",
                         "x-terminal-emulator", "xterm"};
    const char* envTerm = std::getenv("TERMINAL");
    if (envTerm && *envTerm) {
        terms.prepend(QString::fromUtf8(envTerm));
    }
    return terms;
}

QStringList wrapForTerminal(const QString& term, const QStringList& cmd) {
    if (term == "foot" || term == "kitty") {
        return cmd;
    }
    if (term == "wezterm") {
        return QStringList{"start", "--"} << cmd;
    }
    if (term == "gnome-terminal" || term == "xfce4-terminal") {
        return QStringList{"--"} << cmd;
    }
    return QStringList{"-e"} << cmd;
}

// неблокирующее ожидание процесса: sudo или запрос пароля могут держать
// процесс десятки секунд, а GUI обязан перерисовываться, поэтому не waitForFinished()
bool waitForProcResponsive(QProcess& proc, int timeoutMs) {
    if (proc.state() == QProcess::NotRunning) return true;
    QEventLoop loop;
    bool timedOut = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError) { loop.quit(); });
    timer.start(timeoutMs);
    loop.exec();
    return !timedOut && proc.exitStatus() == QProcess::NormalExit;
}

// sudo доступен без пароля (NOPASSWD или живой кэш тикета)? ожидание неблокирующее
bool sudoProbe(int timeoutMs, QString* outErr = nullptr) {
    if (outErr) outErr->clear();
    if (::geteuid() == 0) return true;
    if (QStandardPaths::findExecutable("sudo").isEmpty()) {
        if (outErr) *outErr = QStringLiteral("sudo not found");
        return false;
    }
    QProcess probe;
    probe.start("sudo", {"-n", "true"});
    if (!probe.waitForStarted(1500)) {
        if (outErr) *outErr = QStringLiteral("failed to start sudo");
        return false;
    }
    const bool finished = waitForProcResponsive(probe, timeoutMs);
    if (outErr) *outErr = QString::fromUtf8(probe.readAllStandardError()).trimmed();
    return finished && probe.exitCode() == 0;
}

// имя текущего пользователя, очищенное для использования в путях/правилах
QString currentUserName() {
    QString user = QString::fromUtf8(std::getenv("USER") ? std::getenv("USER") : "");
    if (user.isEmpty()) user = qgetenv("USER");
    if (user.isEmpty()) user = QString::fromUtf8(std::getenv("LOGNAME"));
    user.remove(QRegularExpression("[^a-zA-Z0-9_.-]"));
    return user;
}

// побайтовое копирование потока с отменой и прогрессом; false при отмене
// или ошибке записи, текст в errorMsg
bool copyStream(std::istream& in, std::ostream& out,
                const std::function<bool()>& isCancelled,
                const std::function<void(uintmax_t)>& onProgress,
                QString& errorMsg) {
    constexpr size_t kBuf = 1024 * 1024;
    std::vector<char> buf(kBuf);
    uintmax_t copied = 0;
    while (in.read(buf.data(), kBuf) || in.gcount() > 0) {
        if (isCancelled()) {
            errorMsg = "Cancelled";
            return false;
        }
        auto n = in.gcount();
        out.write(buf.data(), n);
        if (!out) {
            errorMsg = "Write error (disk full or I/O failure)";
            return false;
        }
        copied += static_cast<uintmax_t>(n);
        if (onProgress) onProgress(copied);
    }
    return true;
}

}

// строка диагностики авторизации sudo в лог файл
// файл усечётся при росте > 64 КБ, пароли сюда не попадают
void XdgUtils::sudoDebugLog(const QString& line) {
    const QString dir = QDir::homePath() + QStringLiteral("/.cache/atlifm");
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/sudo-debug.log");
    QFile f(path);
    if (f.size() > 64 * 1024) f.remove();
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(QStringLiteral("[dd.MM HH:mm:ss.zzz] "))
           << line << '\n';
    }
}

bool XdgUtils::sudoTicketAlive(int timeoutMs, QString* outError) {
    return sudoProbe(timeoutMs, outError);
}

bool XdgUtils::polkitAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty();
}

QStringList XdgUtils::rootArgs(const QStringList& cmd) {
    if (cmd.isEmpty()) return cmd;
    QStringList args{QStringLiteral("sudo"), QStringLiteral("-n")};
    args += cmd;
    return args;
}

QString XdgUtils::resolveTrustedExe(const QString& bare) {
    static const char* kTrustedDirs[] = {"/usr/bin", "/bin", "/usr/sbin", "/sbin",
                                         "/usr/local/bin", "/usr/local/sbin"};
    for (const char* dir : kTrustedDirs) {
        const QString candidate = QString::fromLatin1(dir) + QLatin1Char('/') + bare;
        QFileInfo fi(candidate);
        if (fi.isFile() && fi.isExecutable()) return candidate;
    }
    return QString();
}

QStringList XdgUtils::pkexecArgs(const QStringList& cmd) {
    if (cmd.isEmpty()) return cmd;
    // pkexec требует полный путь к программе: резолвим из доверенных
    // системных каталогов, а не из PATH пользователя, т.к. подмена бинарника
    // в пользовательском каталоге даёт запуск от root
    QString exe = XdgUtils::resolveTrustedExe(cmd.first());
    if (exe.isEmpty()) exe = cmd.first();
    QStringList args{QStringLiteral("pkexec"), exe};
    args += cmd.mid(1);
    return args;
}

// разовый запуск готовой команды (argv целиком) с ожиданием ответа, не
// блокирующим GUI; true при нормальном завершении процесса
bool XdgUtils::runCommand(const QStringList& argv, int timeoutMs, QByteArray* out,
                          QString* outErr, int* outExitCode) {
    if (argv.isEmpty()) return false;
    QProcess proc;
    proc.start(argv.first(), argv.mid(1));
    if (!proc.waitForStarted(1500)) {
        if (outErr) *outErr = QStringLiteral("failed to start %1").arg(argv.first());
        if (outExitCode) *outExitCode = -1;
        return false;
    }
    const bool finished = waitForProcResponsive(proc, timeoutMs);
    if (proc.state() != QProcess::NotRunning) {
        proc.kill();
        proc.waitForFinished(300);
    }
    if (out) *out = proc.readAllStandardOutput();
    if (outErr) *outErr = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    if (outExitCode) {
        *outExitCode = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
    }
    return finished && proc.exitStatus() == QProcess::NormalExit;
}

bool XdgUtils::sudoApplyPassword(const QString& password, QString& outError) {
    outError.clear();
    if (::geteuid() == 0) return true;

    QString pass = password; // копия, затираем после отправки

    // сброс кэша вызовом sudo k без пароля иначе sudo v
    // молча подтвердит живой тикет и не проверит пароль а сброс кэша гарантирует проверку
    {
        QProcess invalidate;
        invalidate.start("sudo", {"-k"});
        const bool started = invalidate.waitForStarted(1500);
        const bool done = started ? waitForProcResponsive(invalidate, 2500) : false;
        const QString invErr = QString::fromUtf8(invalidate.readAllStandardError()).trimmed();
        if (invalidate.state() != QProcess::NotRunning) {
            invalidate.kill();
            invalidate.waitForFinished(300);
        }
        sudoDebugLog(QStringLiteral("apply: sudo -k started=%1 done=%2 exit=%3 err=[%4]")
                         .arg(started ? 1 : 0).arg(done ? 1 : 0)
                         .arg(invalidate.exitCode()).arg(invErr));
    }

    // после сброса кэша sudo S v реально проверяет пароль а успех
    // создает свежий тикет для всех sudo операций приложения
    QProcess sudoProc;
    sudoProc.start("sudo", {"-S", "-v"});
    if (!sudoProc.waitForStarted(1500)) {
        sudoDebugLog(QStringLiteral("apply: sudo -S -v failed to start"));
        outError = QString("failed to execute sudo");
        return false;
    }
    sudoProc.write(pass.toUtf8()); sudoProc.putChar('\n');
    pass.fill(QChar(0)); pass.clear();
    sudoProc.closeWriteChannel();

    const bool finished = waitForProcResponsive(sudoProc, 12000);
    if (sudoProc.state() != QProcess::NotRunning) {
        sudoProc.kill();
        sudoProc.waitForFinished(300);
    }
    const QString vErr = QString::fromUtf8(sudoProc.readAllStandardError()).trimmed();
    sudoDebugLog(QStringLiteral("apply: sudo -S -v done=%1 status=%2 exit=%3 err=[%4]")
                     .arg(finished ? 1 : 0)
                     .arg(sudoProc.exitStatus() == QProcess::NormalExit ? QStringLiteral("normal")
                                                                        : QStringLiteral("crash"))
                     .arg(sudoProc.exitCode())
                     .arg(vErr));
    if (!finished) {
        outError = QString("sudo: no response (timeout)");
        return false;
    }
    if (sudoProc.exitStatus() != QProcess::NormalExit || sudoProc.exitCode() != 0) {
        outError = QString("sudo: incorrect password");
        return false;
    }

    // 3) контроль: тикет реально создан, иначе папка откроется пустой
    QString verifyErr;
    if (!sudoProbe(2500, &verifyErr)) {
        sudoDebugLog(QStringLiteral("apply: verify FAILED err=[%1]").arg(verifyErr));
        outError = QString("sudo: ticket was not created (%1)")
                       .arg(verifyErr.isEmpty() ? QStringLiteral("no details") : verifyErr);
        return false;
    }
    sudoDebugLog(QStringLiteral("apply: OK"));

    // тикет создан: все привилегированные вызовы (rootArgs) идут через
    // sudo без повторных вопросов пока тикет жив
    return true;
}

// инлайн авторизация привилегированного действия (попап пароля):
// живой тикет молча true, иначе пароль из TuiInputPopup → sudoApplyPassword
// правила NOPASSWD не пишутся (антипаттерн: правило подавляет пароль и
// ломает путь полкита), системный диалог полкита здесь не показывается,
// для действия с одним системным диалогом есть pkexecArgs()
bool XdgUtils::ensureSudoAuthenticated(QWidget* parentWidget, QString& outError) {
    outError.clear();
    if (::geteuid() == 0) return true;

    // пока активен один запрос пароля, второй не показываем
    // (защита от двойного триггера действия)
    static bool authPromptInFlight = false;
    if (authPromptInFlight) {
        outError = QString("sudo: authentication already in progress");
        return false;
    }

    sudoDebugLog(QStringLiteral("ensure: euid=%1").arg(::geteuid()));

    // тикет уже жив, диалог не нужен; таймаут 2.5 с с запасом для
    // холодного старта sudo на ARM
    const bool ticketAlive = sudoProbe(2500);
    sudoDebugLog(QStringLiteral("ensure: probe ticket_alive=%1").arg(ticketAlive ? 1 : 0));
    if (ticketAlive) return true;

    if (QStandardPaths::findExecutable("sudo").isEmpty()) {
        outError = QString("sudo is not installed on this system");
        return false;
    }

    // внутренний ввод пароля: резерв на случай pkexec недоступен или
    // действие уже выбрало парольный путь
    bool ok = false;
    authPromptInFlight = true;
    QString pass = Widgets::TuiInputPopup::getText(
        parentWidget, "Sudo Authentication", "Password for sudo:", "", &ok, QPoint(), false, true);
    authPromptInFlight = false;
    sudoDebugLog(QStringLiteral("ensure: prompt ok=%1 pass_len=%2").arg(ok ? 1 : 0).arg(pass.size()));
    if (!ok) {
        outError = QString("Authentication cancelled");
        return false;
    }

    // проверка пароля и тикет: общий код с инлайн полем Disk Usage
    // (sudoApplyPassword)
    const bool applied = sudoApplyPassword(pass, outError);
    pass.fill(QChar(0)); pass.clear();
    return applied;
}

// при выходе чистим остатки sudoers через живой тикет, без тикета запись
// уйдёт сама по таймауту sleep 900
void XdgUtils::cleanupSudoSession() {
    const QString user = currentUserName();
    if (!user.isEmpty()) {
        QStringList paths{QString("/etc/sudoers.d/99-atlifm-%1").arg(user)};
        QProcess check;
        check.start("sudo", {"-n", "true"});
        if (check.waitForFinished(200) && check.exitCode() == 0) {
            for (const QString& path : paths) {
                QProcess::startDetached("sudo", {"-n", "rm", "-f", path});
            }
        }
    }
}

bool XdgUtils::openFileWithSudo(const std::filesystem::path& path, QString& outError) {
    outError.clear();
    QString qpath = QString::fromUtf8(path.c_str());

    // $EDITOR/$VISUAL запускаются от root, берём их только если такой
    // бинарник реально существует
    auto isValidEditor = [](const QString& e) {
        if (e.trimmed().isEmpty()) return false;
        if (e.contains('/')) {
            QFileInfo fi(e);
            return fi.isFile() && fi.isExecutable();
        }
        return !QStandardPaths::findExecutable(e).isEmpty();
    };
    QString editor = "nano";
    const char* envEditor = std::getenv("EDITOR");
    const char* envVisual = std::getenv("VISUAL");
    if (envEditor && *envEditor && isValidEditor(QString::fromUtf8(envEditor))) {
        editor = QString::fromUtf8(envEditor);
    } else if (envVisual && *envVisual && isValidEditor(QString::fromUtf8(envVisual))) {
        editor = QString::fromUtf8(envVisual);
    } else {
        for (const char* ed : {"nano", "nvim", "vim", "vi", "micro"}) {
            if (!QStandardPaths::findExecutable(ed).isEmpty()) {
                editor = ed;
                break;
            }
        }
    }

    QString workingDir = QString::fromUtf8(path.parent_path().c_str());
    if (workingDir.isEmpty()) workingDir = QDir::homePath();

    // терминальный sudo сам спросит пароль, если тикета нет
    QStringList cmdArgs;
    if (::geteuid() == 0) {
        cmdArgs = {editor, "--", qpath};
    } else {
        cmdArgs = {"sudo", editor, "--", qpath};
    }

    for (const auto& term : terminalCandidates()) {
        if (term != "x-terminal-emulator" && QStandardPaths::findExecutable(term).isEmpty()) continue;
        if (QProcess::startDetached(term, wrapForTerminal(term, cmdArgs), workingDir)) return true;
    }

    outError = QString("cannot open file with sudo: no terminal emulator found");
    return false;
}

bool XdgUtils::openTerminalAt(const std::filesystem::path& dirPath) {
    QString targetDir = QString::fromUtf8(dirPath.c_str());

    // сначала проверяем переменную окружения $TERMINAL
    const char* envTerm = std::getenv("TERMINAL");
    if (envTerm && *envTerm != '\0') {
        QString termCmd = QString::fromUtf8(envTerm);
        if (!QStandardPaths::findExecutable(termCmd).isEmpty()) {
            if (QProcess::startDetached(termCmd, {}, targetDir)) {
                return true;
            }
        }
    }

    for (const auto& term : terminalCandidates()) {
        if (term != "x-terminal-emulator" && QStandardPaths::findExecutable(term).isEmpty()) continue;
        if (QProcess::startDetached(term, {}, targetDir)) {
            return true;
        }
    }

    return false;
}

// корзина: QFile::moveToTrash, затем gio trash, затем ручная FreeDesktop корзина
namespace {
// кэш листингов root: пишут авторизованный find (Sudo Open) и мутации
// (sudoFileOp), читает фоновый скан FileSystemModel из QThreadPool, поэтому QMutex
struct SudoListingCache {
    QMutex mutex;
    QHash<QString, QString> listings;
};

SudoListingCache& sudoListingCache() {
    static SudoListingCache cache;
    return cache;
}

QString sudoListingKey(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path norm = std::filesystem::weakly_canonical(dir, ec);
    if (ec) norm = dir;
    return QString::fromUtf8(norm.c_str());
}

// полный путь для скрипта sh c исполняемого как root
// резолвим только из доверенных системных каталогов и экранируем
QString privilegedExe(const char* name) {
    const QString bare = QString::fromLatin1(name);
    const QString path = XdgUtils::resolveTrustedExe(bare);
    return shellQuote(path.isEmpty() ? bare : path);
}

// привилегированный запуск через живой тикет sudo или pkexec
// без полкита инлайн пароль только из gui потока
bool runPrivileged(const QStringList& argv, int timeoutMs,
                   QByteArray* out, QString* outErr, int* outExitCode,
                   QWidget* parentWidget) {
    if (::geteuid() == 0) {
        return XdgUtils::runCommand(argv, timeoutMs, out, outErr, outExitCode);
    }
    if (XdgUtils::sudoTicketAlive()) {
        return XdgUtils::runCommand(XdgUtils::rootArgs(argv), timeoutMs, out, outErr, outExitCode);
    }
    if (XdgUtils::polkitAvailable()) {
        return XdgUtils::runCommand(XdgUtils::pkexecArgs(argv), timeoutMs, out, outErr, outExitCode);
    }
    if (QThread::currentThread() != qApp->thread()) {
        if (outErr) *outErr = QStringLiteral("Authentication required");
        if (outExitCode) *outExitCode = -1;
        return false;
    }
    QString authErr;
    if (!XdgUtils::ensureSudoAuthenticated(parentWidget, authErr)) {
        if (outErr) *outErr = authErr;
        if (outExitCode) *outExitCode = -1;
        return false;
    }
    return XdgUtils::runCommand(XdgUtils::rootArgs(argv), timeoutMs, out, outErr, outExitCode);
}
}

void XdgUtils::provideSudoListing(const std::filesystem::path& dir, const QString& findOutput) {
    auto& cache = sudoListingCache();
    QMutexLocker lock(&cache.mutex);
    cache.listings.insert(sudoListingKey(dir), findOutput);
}

bool XdgUtils::sudoListing(const std::filesystem::path& dir, QString* out) {
    auto& cache = sudoListingCache();
    QMutexLocker lock(&cache.mutex);
    const auto it = cache.listings.constFind(sudoListingKey(dir));
    if (it == cache.listings.constEnd()) return false;
    if (out) *out = it.value();
    return true;
}

void XdgUtils::invalidateSudoListingsUnder(const std::filesystem::path& dir) {
    auto& cache = sudoListingCache();
    QMutexLocker lock(&cache.mutex);
    const QString prefix = sudoListingKey(dir);
    if (prefix.isEmpty()) return;
    const QString sub = prefix + QLatin1Char('/');
    for (auto it = cache.listings.begin(); it != cache.listings.end();) {
        if (it.key() == prefix || it.key().startsWith(sub)) {
            it = cache.listings.erase(it);
        } else {
            ++it;
        }
    }
}

// разбор рекурсивного вывода find ("%y\t%s\t%T@\t%m\t%p\0"): пересборка
// записей в формат одиночного листинга (%f) и группировка по родителям
// имя может содержать табы, поэтому путь это всё после пятого \t; обрыв
// вывода без финального NUL отбрасывается, недозаписанный путь в кэш не
// должен попасть
void XdgUtils::provideSudoListingTree(const std::filesystem::path& root, const QString& recursiveOutput) {
    const QChar nul(QLatin1Char('\0'));
    QHash<QString, QString> groups; // sudoListingKey(parent) → записи этого каталога
    QSet<QString> dirKeys;          // все встреченные каталоги (включая пустые)
    int pos = 0;
    while (pos < recursiveOutput.size()) {
        const int end = recursiveOutput.indexOf(nul, pos);
        if (end < 0) break; // обрезанный хвост не принимаем
        const QString rec = recursiveOutput.mid(pos, end - pos);
        pos = end + 1;
        if (rec.isEmpty()) continue;
        const int t1 = rec.indexOf(QLatin1Char('\t'));
        const int t2 = t1 < 0 ? -1 : rec.indexOf(QLatin1Char('\t'), t1 + 1);
        const int t3 = t2 < 0 ? -1 : rec.indexOf(QLatin1Char('\t'), t2 + 1);
        const int t4 = t3 < 0 ? -1 : rec.indexOf(QLatin1Char('\t'), t3 + 1);
        if (t4 < 0) continue;
        const QString type = rec.left(t1);
        const QString fullPath = rec.mid(t4 + 1);
        if (fullPath.isEmpty()) continue;
        const std::filesystem::path full(fullPath.toStdString());
        const std::filesystem::path parent = full.parent_path();
        if (parent.empty()) continue;
        const QString key = sudoListingKey(parent);
        groups[key] += type + QLatin1Char('\t') + rec.mid(t1 + 1, t2 - t1 - 1)
            + QLatin1Char('\t') + rec.mid(t2 + 1, t3 - t2 - 1)
            + QLatin1Char('\t') + rec.mid(t3 + 1, t4 - t3 - 1)
            + QLatin1Char('\t') + QString::fromStdString(full.filename().string()) + nul;
        if (type == QStringLiteral("d")) dirKeys.insert(sudoListingKey(full));
    }
    // корень и все пройденные каталоги без записей валидные пустые ответы
    const QString rootKey = sudoListingKey(root);
    if (!rootKey.isEmpty() && !groups.contains(rootKey)) groups.insert(rootKey, QString());
    for (const QString& dk : dirKeys) {
        if (!groups.contains(dk)) groups.insert(dk, QString());
    }
    auto& cache = sudoListingCache();
    QMutexLocker lock(&cache.mutex);
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        cache.listings.insert(it.key(), it.value());
    }
}

    // рекурсивный листинг одним вызовом: find обходит maxdepth уровней,
    // после одной авторизации кэш работает на любую глубину; пути
    // позиционный аргумент и ведущий минус безопасен
    // ограничение размера через head c для больших деревьев
XdgUtils::SudoListStatus XdgUtils::sudoListTree(const std::filesystem::path& dir, int maxDepth,
                                                QString& outList, QString& outErr,
                                                QWidget* parentWidget) {
    outList.clear();
    outErr.clear();
    if (dir.empty()) return SudoListStatus::Failed;
    const int depth = maxDepth > 0 ? maxDepth : 5;
    const QString findCmd = privilegedExe("find") +
        QStringLiteral(" -- \"$1\" -mindepth 1 -maxdepth %1 -printf '%y\\t%s\\t%T@\\t%m\\t%p\\0'")
            .arg(depth) +
        QStringLiteral(" 2>/dev/null | ") + privilegedExe("head") + QStringLiteral(" -c 8000000");
    QStringList argv{QStringLiteral("/bin/sh"), QStringLiteral("-c"), findCmd, QStringLiteral("_"),
                     QString::fromUtf8(dir.c_str())};

    QByteArray out;
    QString err;
    int exitCode = -1;
    const bool finished = runPrivileged(argv, 30000, &out, &err, &exitCode, parentWidget);
    outList = QString::fromUtf8(out);
    outErr = err.left(160).replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (!finished) return SudoListStatus::Failed;
    if (exitCode == 126 || exitCode == 127) return SudoListStatus::Dismissed;
    if (exitCode == 0 || !out.isEmpty()) return SudoListStatus::Ok;
    // пустой вывод и ненулевой код: консервативно Failed
    return SudoListStatus::Failed;
}

// привилегированная мутация: мутация и свежий листинг find одним вызовом,
// второй pkexec стал бы вторым системным диалогом (политика auth_admin без
// пути позиционные через sh c
bool XdgUtils::sudoFileOp(PrivOp op, const std::vector<std::filesystem::path>& targets,
                          const std::filesystem::path& listingDir,
                          QString& outError, QWidget* parentWidget) {
    outError.clear();
    if (targets.empty()) return true;
    if (listingDir.empty()) {
        outError = QStringLiteral("No listing directory");
        return false;
    }

    QString mutation;
    switch (op) {
    case PrivOp::MakeDir:
        mutation = privilegedExe("mkdir") + QStringLiteral(" -p -- \"$1\"");
        break;
    case PrivOp::MakeFile:
        mutation = privilegedExe("touch") + QStringLiteral(" -- \"$1\"");
        break;
    case PrivOp::Rename:
    case PrivOp::Move:
        mutation = privilegedExe("mv") + QStringLiteral(" -f -- \"$1\" \"$2\"");
        break;
    case PrivOp::Copy:
        mutation = privilegedExe("cp") + QStringLiteral(" -a -- \"$1\" \"$2\"");
        break;
    case PrivOp::Remove:
        mutation = privilegedExe("rm") + QStringLiteral(" -rf -- \"$@\"");
        break;
    }
    const QString findCmd = privilegedExe("find") +
        QStringLiteral(" -- \"$d\" -mindepth 1 -maxdepth 1 -printf '%y\\t%s\\t%T@\\t%m\\t%f\\0'");
    const QString script = QStringLiteral(
        "d=\"$1\"; shift; if %1; then %2; true; else exit $?; fi").arg(mutation, findCmd);

    QStringList argv{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script, QStringLiteral("_")};
    argv << QString::fromUtf8(listingDir.c_str());
    for (const auto& t : targets) argv << QString::fromUtf8(t.c_str());

    // тяжёлые операции под root идут минуты, таймаут больше чем у
    // мгновенных mkdir/touch/mv
    const int timeoutMs = (op == PrivOp::Copy || op == PrivOp::Move || op == PrivOp::Remove)
                              ? 300000 : 120000;
    QByteArray out;
    QString err;
    int exitCode = -1;
    const bool finished = runPrivileged(argv, timeoutMs, &out, &err, &exitCode, parentWidget);
    sudoDebugLog(QStringLiteral("sudoFileOp: op=%1 targets=%2 listing=[%3] finished=%4 exit=%5 err=[%6]")
                     .arg(static_cast<int>(op)).arg(targets.size())
                     .arg(QString::fromUtf8(listingDir.c_str()))
                     .arg(finished ? 1 : 0).arg(exitCode).arg(err.left(160)));
    if (!finished || exitCode != 0) {
        if (exitCode == 126) outError = QStringLiteral("Authentication denied");
        else if (exitCode == 127) outError = QStringLiteral("Not authorized");
        else if (!err.isEmpty()) outError = err;
        else outError = QStringLiteral("Operation failed");
        return false;
    }

    // после мутации поддерево кэша могло устареть
    invalidateSudoListingsUnder(listingDir);
    provideSudoListing(listingDir, QString::fromUtf8(out));
    return true;
}

bool XdgUtils::moveToTrash(const std::vector<std::filesystem::path>& paths, QString& errorMsg,
                           std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* outMoved) {
    if (paths.empty()) return true;

    for (const auto& p : paths) {
        QString qpath = QString::fromUtf8(p.c_str());
        // 1. стандартный механизм корзины Qt
        QString inTrash;
        if (QFile::moveToTrash(qpath, &inTrash)) {
            if (outMoved && !inTrash.isEmpty()) {
                outMoved->push_back({p, std::filesystem::path(inTrash.toUtf8().toStdString())});
            }
            continue;
        }

        // 2. gio trash
        QProcess proc;
        proc.start("gio", {"trash", "--", qpath});
        if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
            const char* home = std::getenv("HOME");
            if (outMoved && home) {
                std::filesystem::path dest = std::filesystem::path(home) / ".local/share/Trash/files" / p.filename();
                outMoved->push_back({p, dest});
            }
            continue;
        }

        // 3. откат: ручная корзина по спецификации FreeDesktop Trash
        const char* home = std::getenv("HOME");
        if (!home) {
            errorMsg = "HOME directory not found.";
            return false;
        }

        std::filesystem::path trashBase = std::filesystem::path(home) / ".local/share/Trash";
        std::filesystem::path trashFilesDir = trashBase / "files";
        std::filesystem::path trashInfoDir = trashBase / "info";
        std::error_code ec;
        std::filesystem::create_directories(trashFilesDir, ec);
        std::filesystem::create_directories(trashInfoDir, ec);

        std::string baseStem = p.stem().string();
        std::string baseExt = p.extension().string();
        std::string finalName = p.filename().string();
        std::filesystem::path dest = trashFilesDir / finalName;

        int suffix = 1;
        std::error_code existEc;
        while (std::filesystem::exists(dest, existEc)) {
            finalName = baseStem + "." + std::to_string(suffix++) + baseExt;
            dest = trashFilesDir / finalName;
        }

        std::filesystem::rename(p, dest, ec);
        if (ec) {
            errorMsg = QString("Failed to trash %1: %2").arg(QString::fromUtf8(p.c_str()), QString::fromStdString(ec.message()));
            return false;
        }
        if (outMoved) {
            outMoved->push_back({p, dest});
        }

        // пишем файл .trashinfo, чтобы другие файловые менеджеры могли восстановить элемент
        std::filesystem::path infoPath = trashInfoDir / (finalName + ".trashinfo");
        QFile infoFile(QString::fromUtf8(infoPath.c_str()));
        if (infoFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&infoFile);
            out << "[Trash Info]\n";
            out << "Path=" << QUrl::toPercentEncoding(QString::fromUtf8(p.c_str()), "/") << "\n";
            out << "DeletionDate=" << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
            infoFile.close();
        }
    }
    return true;
}

// ec в remove_all/create_directories, чтобы не падать на EACCES
bool XdgUtils::clearTrash() {
    const char* home = std::getenv("HOME");
    if (!home) return false;
    std::filesystem::path trashBase = std::filesystem::path(home) / ".local/share/Trash";
    std::error_code ec;
    std::filesystem::remove_all(trashBase / "files", ec);
    if (!ec) std::filesystem::remove_all(trashBase / "info", ec);
    if (!ec) std::filesystem::create_directories(trashBase / "files", ec);
    if (!ec) std::filesystem::create_directories(trashBase / "info", ec);
    if (ec) return false;

    QProcess proc;
    proc.start("gio", {"trash", "--empty"});
    proc.waitForFinished(1500);
    // gio может отсутствовать, очистка каталогов уже сработала; успех =
    // процесс завершился сам, «всё ещё бежит» после таймаута не успех
    if (proc.error() == QProcess::FailedToStart) return true;
    if (proc.state() != QProcess::NotRunning) {
        proc.kill();
        proc.waitForFinished(300);
        return false;
    }
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

bool XdgUtils::deletePermanently(const std::vector<std::filesystem::path>& paths, QString& errorMsg,
                                 std::shared_ptr<std::atomic<bool>> cancelToken) {
    const char* home = std::getenv("HOME");
    std::filesystem::path trashFiles;
    std::filesystem::path trashInfo;
    if (home) {
        trashFiles = std::filesystem::path(home) / ".local/share/Trash/files";
        trashInfo = std::filesystem::path(home) / ".local/share/Trash/info";
    }

    std::vector<std::filesystem::path> denied;
    for (const auto& p : paths) {
        if (cancelToken && cancelToken->load()) {
            errorMsg = "Cancelled";
            return false;
        }
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        if (ec) {
            // отказавшие пути уходят одним привилегированным rm rf вместе
            // со свежим листингом каталога (один системный диалог)
            if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
                denied.push_back(p);
                continue;
            }
            errorMsg = QString("Failed to delete %1: %2").arg(QString::fromUtf8(p.c_str()), QString::fromStdString(ec.message()));
            return false;
        }

        // элемент из корзины: удаляем и его .trashinfo; сравнение по
        // реальному префиксу каталога, а не подстроке, иначе похожий
        // путь в середине запустит удаление .trashinfo
        const std::string parentNorm = p.parent_path().lexically_normal().string();
        const std::string trashNorm = trashFiles.lexically_normal().string();
        const bool insideTrash = !trashFiles.empty() &&
            (p.parent_path() == trashFiles ||
             parentNorm.compare(0, trashNorm.size() + 1, trashNorm + "/") == 0);
        if (insideTrash) {
            std::filesystem::path infoPath = trashInfo / (p.filename().string() + ".trashinfo");
            std::error_code infoEc;
            std::filesystem::remove(infoPath, infoEc);
            if (infoEc) {
                QProcess proc;
                const QStringList argv = rootArgs({"rm", "-f", "--", QString::fromUtf8(infoPath.c_str())});
                proc.start(argv.first(), argv.mid(1));
                proc.waitForFinished(1000);
            }
        }
    }

    // отказавшие пути уходят одним привилегированным батчем rm со свежим
    // листингом каталога (один системный диалог)
    if (!denied.empty()) {
        // если отмена пришла после сбора denied путей то привилегированный rm не запускаем
        // уже не запускаем и честно сообщаем, что операция не завершена
        if (cancelToken && cancelToken->load()) {
            errorMsg = "Cancelled";
            return false;
        }
        QString sudoErr;
        if (!sudoFileOp(PrivOp::Remove, denied, denied.front().parent_path(), sudoErr)) {
            errorMsg = QString("Failed to delete %1: %2")
                           .arg(QString::fromUtf8(denied.front().c_str()),
                                sudoErr.isEmpty() ? QStringLiteral("permission denied") : sudoErr);
            return false;
        }
    }
    return true;
}

bool XdgUtils::moveItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& errorMsg,
                        std::function<void(uintmax_t bytesCopied, uintmax_t totalBytes)> progressCallback,
                        std::shared_ptr<std::atomic<bool>> cancelToken) {
    std::error_code ec;
    std::filesystem::rename(src, dest, ec);
    if (ec) {
        // откат при переносе между устройствами (EXDEV): копируем, затем удаляем источник
        if (ec == std::make_error_code(std::errc::cross_device_link)) {
            QString copyErr;
            if (copyItem(src, dest, copyErr, progressCallback, cancelToken)) {
                std::error_code rmEc;
                std::filesystem::remove_all(src, rmEc);
                if (!rmEc) return true;
                // копия создана, но источник не удалился: сообщаем
                // реальную причину
                errorMsg = QString("Copied to %1, but failed to remove source %2: %3")
                               .arg(QString::fromUtf8(dest.c_str()),
                                    QString::fromUtf8(src.c_str()),
                                    QString::fromStdString(rmEc.message()));
                return false;
            }
            errorMsg = copyErr;
            return false;
        }
        // sudo фолбэк только на отказе доступа (иначе лишний вопрос
        // пароля на «не нашей» ошибке); мутация и листинг одним вызовом
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            QString sudoErr;
            if (sudoFileOp(PrivOp::Move, {src, dest}, dest.parent_path(), sudoErr)) {
                return true;
            }
            errorMsg = sudoErr.isEmpty() ? QString::fromStdString(ec.message()) : sudoErr;
            return false;
        }
        errorMsg = QString::fromStdString(ec.message());
        return false;
    }
    return true;
}

bool XdgUtils::copyItem(const std::filesystem::path& src, const std::filesystem::path& dest, QString& errorMsg,
                        std::function<void(uintmax_t bytesCopied, uintmax_t totalBytes)> progressCallback,
                        std::shared_ptr<std::atomic<bool>> cancelToken) {
    std::error_code ec;
    auto symStatus = std::filesystem::symlink_status(src, ec);
    if (!ec && std::filesystem::is_symlink(symStatus)) {
        auto target = std::filesystem::read_symlink(src, ec);
        if (!ec) {
            std::filesystem::remove(dest, ec);
            std::filesystem::create_symlink(target, dest, ec);
            if (!ec) return true;
        }
    }

    if (std::filesystem::is_regular_file(src, ec)) {
        uintmax_t totalBytes = std::filesystem::file_size(src, ec);
        if (ec) totalBytes = 0;

        errno = 0;
        std::ifstream in(src, std::ios::binary);
        if (!in) {
            // sudo фолбэк, если источник нечитаем без root (только EACCES/
            // cp a и свежий листинг одним вызовом
            const int openErrno = errno;
            if (openErrno == EACCES || openErrno == EPERM) {
                QString sudoErr;
                if (sudoFileOp(PrivOp::Copy, {src, dest}, dest.parent_path(), sudoErr)) {
                    return true;
                }
                errorMsg = sudoErr.isEmpty() ? QStringLiteral("Cannot open source file for reading") : sudoErr;
                return false;
            }
            errorMsg = "Cannot open source file for reading";
            return false;
        }

        errno = 0;
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            // sudo фолбэк, если каталог назначения защищён root
            // (только EACCES/EPERM, прочие ошибки не повод спрашивать пароль)
            const int openErrno = errno;
            if (openErrno == EACCES || openErrno == EPERM) {
                QString sudoErr;
                if (sudoFileOp(PrivOp::Copy, {src, dest}, dest.parent_path(), sudoErr)) {
                    return true;
                }
                errorMsg = sudoErr.isEmpty() ? QStringLiteral("Cannot open destination file for writing") : sudoErr;
                return false;
            }
            errorMsg = "Cannot open destination file for writing";
            return false;
        }

        auto isCancelled = [&]() { return cancelToken && cancelToken->load(); };
        if (!copyStream(in, out,
                        isCancelled,
                        [&](uintmax_t copied) {
                            if (progressCallback && totalBytes > 0) {
                                progressCallback(copied, totalBytes);
                            }
                        }, errorMsg)) {
            in.close();
            out.close();
            std::filesystem::remove(dest, ec);
            return false;
        }
        out.close();
        in.close();

        // сохраняем права исходного файла (включая +x)
        auto srcStatus = std::filesystem::status(src, ec);
        if (!ec) {
            std::filesystem::permissions(dest, srcStatus.permissions(), std::filesystem::perm_options::replace, ec);
        }

        return true;
    } else {
        // проверка cancelToken на каждом файле: std::filesystem::copy()
        // нельзя прервать извне
        auto isCancelled = [&]() { return cancelToken && cancelToken->load(); };
        if (isCancelled()) {
            errorMsg = "Cancelled";
            return false;
        }
        std::error_code walkEc;
        std::filesystem::create_directories(dest, walkEc);
        if (walkEc) {
            // sudo фолбэк для дерева в каталоге root, только на отказе доступа
            if (walkEc == std::errc::permission_denied || walkEc == std::errc::operation_not_permitted) {
                QString sudoErr;
                if (sudoFileOp(PrivOp::Copy, {src, dest}, dest.parent_path(), sudoErr)) {
                    return true;
                }
                errorMsg = sudoErr.isEmpty() ? QString::fromStdString(walkEc.message()) : sudoErr;
                return false;
            }
            errorMsg = QString::fromStdString(walkEc.message());
            return false;
        }
        uintmax_t dirCopied = 0;
        int walkErrors = 0;
        auto rit = std::filesystem::recursive_directory_iterator(
            src, std::filesystem::directory_options::skip_permission_denied, walkEc);
        auto rend = std::filesystem::recursive_directory_iterator();
        while (rit != rend) {
            if (walkEc) {
            // ошибка обхода означает неполную копию
            ++walkErrors;
                walkEc.clear();
            }
            if (isCancelled()) {
                std::error_code rmEc;
                std::filesystem::remove_all(dest, rmEc);
                errorMsg = "Cancelled";
                return false;
            }
            const auto& entry = *rit;
            std::error_code relEc;
            auto rel = std::filesystem::relative(entry.path(), src, relEc);
            if (relEc) {
                rit.increment(walkEc);
                continue;
            }
            std::filesystem::path target = dest / rel;
            std::error_code eEc;
            auto symSt = entry.symlink_status(eEc);
            if (!eEc && std::filesystem::is_symlink(symSt)) {
                auto linkTarget = std::filesystem::read_symlink(entry.path(), eEc);
                if (!eEc) {
                    std::filesystem::remove(target, eEc);
                    std::filesystem::create_symlink(linkTarget, target, eEc);
                }
            } else if (!eEc && std::filesystem::is_directory(symSt)) {
                std::filesystem::create_directories(target, eEc);
            } else if (!eEc && std::filesystem::is_regular_file(symSt)) {
                std::ifstream in(entry.path(), std::ios::binary);
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (in && out) {
                    if (!copyStream(in, out, isCancelled,
                                    [&](uintmax_t n) {
                                        if (progressCallback) progressCallback(dirCopied + n, 0);
                                    }, errorMsg)) {
                        in.close();
                        out.close();
                        std::error_code rmEc;
                        std::filesystem::remove_all(dest, rmEc);
                        return false;
                    }
                    out.close();
                    std::error_code sizeEc;
                    const uintmax_t fileSize = std::filesystem::file_size(target, sizeEc);
                    if (!sizeEc) dirCopied += fileSize;
                } else {
                    std::filesystem::copy_file(entry.path(), target,
                        std::filesystem::copy_options::overwrite_existing, eEc);
                }
                auto entrySt = std::filesystem::status(entry.path(), eEc);
                if (!eEc) {
                    std::filesystem::permissions(target, entrySt.permissions(),
                        std::filesystem::perm_options::replace, eEc);
                }
            } else if (!eEc && std::filesystem::is_fifo(symSt)) {
                // mkfifo, иначе копия станет пустым каталогом; права от
                // источника, ошибку не глотаем (неполная копия)
                std::filesystem::remove(target, eEc);
                std::error_code fifoEc;
                const auto fifoSt = entry.status(fifoEc);
                const mode_t fifoMode = fifoEc ? 0644 : static_cast<mode_t>(fifoSt.permissions());
                if (::mkfifo(target.c_str(), fifoMode) != 0) ++walkErrors;
            }
            // сокеты и устройства осознанно пропускаем
            rit.increment(walkEc);
        }
        if (walkEc) {
            ++walkErrors;
            walkEc.clear();
        }
        if (isCancelled()) {
            std::error_code rmEc;
            std::filesystem::remove_all(dest, rmEc);
            errorMsg = "Cancelled";
            return false;
        }
        if (walkErrors > 0) {
            // копия неполная, успехом не считаем
            errorMsg = QString("Some entries could not be read (%1 skipped)").arg(walkErrors);
            return false;
        }
        // права корня как у источника
        auto srcStatus = std::filesystem::status(src, walkEc);
        if (!walkEc) {
            std::filesystem::permissions(dest, srcStatus.permissions(),
                std::filesystem::perm_options::replace, walkEc);
        }
        return true;
    }
}

bool XdgUtils::createSymlink(const std::filesystem::path& target, const std::filesystem::path& linkPath, QString& errorMsg) {
    std::error_code ec;
    std::filesystem::create_symlink(target, linkPath, ec);
    if (ec) {
        errorMsg = QString::fromStdString(ec.message());
        return false;
    }
    return true;
}

// защита от инъекции аргументов и вывод читается порциями чтобы избежать дедлока

namespace {

// 7z/7zz/7za предпочтительный инструмент для zip/7z/rar/потоков; семейство
// tar.* ими не распаковываем: `7z x` на .tar.gz выдаёт только внутренний .tar
QString findSevenZip() {
    static const QString cached = []() -> QString {
        for (const char* name : {"7z", "7zz", "7za"}) {
            const QString p = QStandardPaths::findExecutable(QString::fromLatin1(name));
            if (!p.isEmpty()) return p;
        }
        return QString();
    }();
    return cached;
}

QString findTool(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!p.isEmpty()) return p;
    }
    return QString();
}

bool hasSuffix(const QString& lower, std::initializer_list<const char*> sufs) {
    for (const char* s : sufs) {
        if (lower.endsWith(QString::fromLatin1(s))) return true;
    }
    return false;
}

bool isTarArchiveName(const QString& lower) {
    return hasSuffix(lower, {".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2",
                             ".tbz2", ".tbz", ".tar.zst", ".tzst", ".tar.lz4", ".tar.lzma"});
}

bool isSevenZipArchiveName(const QString& lower) {
    return hasSuffix(lower, {".zip", ".7z", ".rar", ".jar", ".iso", ".cab", ".lzh", ".lha", ".arj"});
}

bool isSingleStreamName(const QString& lower) {
    return hasSuffix(lower, {".gz", ".xz", ".bz2", ".zst", ".zstd", ".lz4", ".lzma"});
}

QString archiveDummyPassword() {
    return QStringLiteral("atlifm-no-password");
}

bool looksLikePasswordError(const QString& text) {
    const QString lower = text.toLower();
    return lower.contains(QStringLiteral("password")) ||
           lower.contains(QStringLiteral("passphrase")) ||
           lower.contains(QStringLiteral("encrypted"));
}

QString archivePasswordError() {
    return QStringLiteral("Archive password is required.");
}

QMutex& archivePasswordMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString>& archivePasswordCache() {
    static QHash<QString, QString> cache;
    return cache;
}

QString cachedArchivePassword(const QString& archivePath) {
    QMutexLocker lock(&archivePasswordMutex());
    return archivePasswordCache().value(archivePath);
}

void rememberArchivePassword(const QString& archivePath, const QString& password) {
    QMutexLocker lock(&archivePasswordMutex());
    archivePasswordCache().insert(archivePath, password);
}

QString archivePasswordArg(const QString& password) {
    return password.isEmpty() ? archiveDummyPassword() : password;
}

QString askArchivePassword(const QString& archivePath, bool wrongPassword) {
    const QString name = QFileInfo(archivePath).fileName();
    const QString label = wrongPassword
        ? QStringLiteral("Wrong password. Enter password for %1 again:").arg(name)
        : QStringLiteral("Enter password for %1:").arg(name);
    bool ok = false;
    QString password;
    auto ask = [&]() {
        password = Widgets::TuiInputPopup::getText(
            nullptr, QStringLiteral("Archive Password"), label, QString(), &ok,
            QPoint(), false, true);
    };
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        ask();
    else
        QMetaObject::invokeMethod(app, ask, Qt::BlockingQueuedConnection);
    return ok ? password : QString();
}

// общий запуск внешней операции с опросом процесса
// чтобы можно было отменить долгий архив из тасков
bool runAndWait(QProcess& proc, QString& errorMsg,
                const std::shared_ptr<std::atomic_bool>& cancelToken) {
    while (proc.state() != QProcess::NotRunning) {
        if (cancelToken && cancelToken->load()) {
            proc.terminate();
            if (!proc.waitForFinished(300)) {
                proc.kill();
                proc.waitForFinished(300);
            }
            errorMsg = QStringLiteral("Cancelled");
            return false;
        }
        proc.waitForFinished(100);
    }
    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
        return true;
    }
    errorMsg = QString::fromUtf8(proc.readAllStandardError());
    if (errorMsg.isEmpty()) errorMsg = proc.errorString();
    return false;
}

struct ArchiveCmd {
    QString program;
    QStringList args;
    QString cwd; // пусто не менять рабочий каталог
};

// первый успешный кандидат; при всех отказах stderr последнего
bool runFirstSuccessful(const std::vector<ArchiveCmd>& cmds, QString& errorMsg,
                        const std::shared_ptr<std::atomic_bool>& cancelToken) {
    QStringList errors;
    for (const auto& cmd : cmds) {
        if (cancelToken && cancelToken->load()) {
            errorMsg = QStringLiteral("Cancelled");
            return false;
        }
        QProcess proc;
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        proc.setStandardInputFile(QProcess::nullDevice());
        if (!cmd.cwd.isEmpty()) proc.setWorkingDirectory(cmd.cwd);
        proc.start(cmd.program, cmd.args);
        if (!proc.waitForStarted(5000)) {
            if (cancelToken && cancelToken->load()) {
                errorMsg = QStringLiteral("Cancelled");
                return false;
            }
            errors << cmd.program + ": " + proc.errorString();
            continue;
        }
        if (runAndWait(proc, errorMsg, cancelToken)) return true;
        if (cancelToken && cancelToken->load()) return false;
        // GNU tar/7z сами начинают stderr с «<программа>:», не дублируем
        const QString detail = errorMsg.trimmed();
        const QString progName = QFileInfo(cmd.program).fileName();
        if (detail.startsWith(cmd.program + ":") || detail.startsWith(progName + ":"))
            errors << detail;
        else
            errors << cmd.program + ": " + detail;
    }
    errorMsg = errors.isEmpty() ? QStringLiteral("no suitable tool found") : errors.last();
    if (looksLikePasswordError(errors.join('\n'))) errorMsg = archivePasswordError();
    return false;
}

using ArchiveCmdBuilder = std::function<std::vector<ArchiveCmd>(const QString&)>;

bool runArchiveCommand(const QString& archivePath, const ArchiveCmdBuilder& buildCmds,
                       QString& errorMsg,
                       const std::shared_ptr<std::atomic_bool>& cancelToken) {
    QString password = cachedArchivePassword(archivePath);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (runFirstSuccessful(buildCmds(password), errorMsg, cancelToken)) {
            errorMsg.clear();
            if (!password.isEmpty()) rememberArchivePassword(archivePath, password);
            return true;
        }
        if (cancelToken && cancelToken->load()) return false;
        if (!looksLikePasswordError(errorMsg)) return false;
        password = askArchivePassword(archivePath, attempt > 0);
        if (password.isEmpty()) {
            errorMsg = archivePasswordError();
            return false;
        }
    }
    errorMsg = QStringLiteral("Wrong archive password.");
    return false;
}

// одиночный поток (gz/xz/bz2/zst без внешнего архиватора): stdout
// компрессора пишем прямо в файл назначения
bool compressStreamToFile(const QString& prog, const QStringList& args,
                          const QString& cwd, const QString& destStr, QString& errorMsg,
                          const std::shared_ptr<std::atomic_bool>& cancelToken) {
    QProcess proc;
    proc.setWorkingDirectory(cwd);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(prog, args);
    if (!proc.waitForStarted(5000)) {
        errorMsg = proc.errorString();
        return false;
    }
    // сжатый вывод пишет прямо в файл, иначе QProcess сначала буферизует
    // его целиком (гигабайты RAM на больших файлах)
    QFile out(destStr);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMsg = out.errorString();
        proc.kill();
        proc.waitForFinished(300);
        return false;
    }
    QByteArray stderrTail;
    while (true) {
        const QByteArray chunk = proc.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            if (out.write(chunk) != chunk.size()) {
                errorMsg = QStringLiteral("Write error (disk full or I/O failure)");
                proc.kill();
                proc.waitForFinished(300);
                out.close();
                return false;
            }
        }
        stderrTail += proc.readAllStandardError();
        if (stderrTail.size() > 64 * 1024) stderrTail = stderrTail.right(64 * 1024);
        if (proc.state() == QProcess::NotRunning) break;
        if (cancelToken && cancelToken->load()) {
            proc.terminate();
            if (!proc.waitForFinished(300)) {
                proc.kill();
                proc.waitForFinished(300);
            }
            out.close();
            errorMsg = QStringLiteral("Cancelled");
            return false;
        }
        proc.waitForReadyRead(100);
    }
    // остаток данных после завершения процесса
    {
        const QByteArray rest = proc.readAllStandardOutput();
        if (!rest.isEmpty() && out.write(rest) != rest.size()) {
            errorMsg = QStringLiteral("Write error (disk full or I/O failure)");
            out.close();
            return false;
        }
    }
    out.close();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        errorMsg = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (errorMsg.isEmpty()) errorMsg = QString::fromUtf8(stderrTail).trimmed();
        if (errorMsg.isEmpty()) errorMsg = proc.errorString();
        return false;
    }
    return true;
}

}

bool XdgUtils::isArchive(const std::filesystem::path& path) {
    const QString lower = QString::fromUtf8(path.filename().c_str()).toLower();
    if (lower.isEmpty()) return false;
    return isTarArchiveName(lower) || isSevenZipArchiveName(lower) || isSingleStreamName(lower);
}

bool XdgUtils::isSingleStreamArchive(const std::filesystem::path& path) {
    const QString lower = QString::fromUtf8(path.filename().c_str()).toLower();
    // tar.gz/.tar.xz это не одиночный поток: суффикс .tar.* имеет приоритет
    return !lower.isEmpty() && isSingleStreamName(lower) && !isTarArchiveName(lower);
}

std::filesystem::path XdgUtils::archiveExtractDirName(const std::filesystem::path& archivePath) {
    // a.tar.gz → a, b.zip → b, c → c (самый длинный суффикс срезается один раз)
    QString name = QString::fromUtf8(archivePath.filename().c_str());
    const QString lower = name.toLower();
    static const char* kSufs[] = {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tar.lz4", ".tar.lzma",
                                  ".tgz", ".txz", ".tbz2", ".tbz", ".tzst",
                                  ".tar", ".zip", ".7z", ".rar", ".jar", ".iso",
                                  ".gz", ".xz", ".bz2", ".zst", ".zstd", ".lz4", ".lzma"};
    for (const char* s : kSufs) {
        const QString suf = QString::fromLatin1(s);
        if (lower.endsWith(suf) && name.size() > suf.size()) {
            name.chop(suf.size());
            break;
        }
    }
    return std::filesystem::path(name.toUtf8().toStdString());
}

bool XdgUtils::createArchive(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& destination,
                             QString& errorMsg,
                             std::shared_ptr<std::atomic_bool> cancelToken) {
    if (sources.empty()) return false;
    if (cancelToken && cancelToken->load()) {
        errorMsg = QStringLiteral("Cancelled");
        return false;
    }

    std::error_code destEc;
    const bool destExisted = std::filesystem::exists(destination, destEc);
    auto cleanupPartial = [&]() {
        if (cancelToken && cancelToken->load() && !destExisted) {
            std::error_code ec;
            std::filesystem::remove(destination, ec);
        }
    };

    const QString destStr = QString::fromUtf8(destination.c_str());
    const QString lower = destStr.toLower();
    const QString cwd = QString::fromUtf8(sources.front().parent_path().c_str());
    const QString sevenZip = findSevenZip();

    // архив собирается из cwd + имён файлов, поэтому все источники обязаны
    // лежать в одном каталоге: иначе архиватор молча теряет или подменяет файлы
    for (const auto& s : sources) {
        std::error_code sameEc;
        const bool same = std::filesystem::equivalent(s.parent_path(),
                                                      sources.front().parent_path(), sameEc);
        if (sameEc || !same) {
            errorMsg = "All items must be in the same folder to compress";
            return false;
        }
    }

    // только имена файлов, cwd = их общий родитель: без пробельных проблем
    // защита от инъекции опций
    QStringList names;
    QStringList safeNames;
    for (const auto& s : sources) {
        const QString n = QString::fromUtf8(s.filename().c_str());
        names << n;
        safeNames << (n.startsWith('-') ? "./" + n : n);
    }

    // одиночные потоки через 7z или gzip или xz
    // tar gz и tar xz это tar контейнер а не поток одного файла
    if (isSingleStreamName(lower) && !isTarArchiveName(lower)) {
        if (sources.size() != 1) {
            errorMsg = "Only one file can be compressed into " + destStr;
            return false;
        }
        const QString single = safeNames.first();
        if (!sevenZip.isEmpty()) {
            const char* fmtFlag = lower.endsWith(".gz") ? "-tgzip"
                                : lower.endsWith(".xz") ? "-txz"
                                : lower.endsWith(".bz2") ? "-tbzip2" : nullptr;
            if (fmtFlag) {
                std::vector<ArchiveCmd> cmds{
                    {sevenZip, {"a", QString::fromLatin1(fmtFlag), "-bso0", "-bsp0", destStr, "--", single}, cwd}};
                if (runFirstSuccessful(cmds, errorMsg, cancelToken)) return true;
                if (cancelToken && cancelToken->load()) {
                    cleanupPartial();
                    errorMsg = QStringLiteral("Cancelled");
                    return false;
                }
            }
        }
        const char* tool = lower.endsWith(".gz") ? "gzip"
                         : lower.endsWith(".xz") ? "xz"
                         : lower.endsWith(".bz2") ? "bzip2"
                         : (lower.endsWith(".zst") || lower.endsWith(".zstd")) ? "zstd" : nullptr;
        if (tool) {
            const QString prog = findTool({tool});
            if (!prog.isEmpty()) {
                const QStringList args = (QString::fromLatin1(tool) == "zstd")
                    ? QStringList{"-q", "-c", single}
                    : QStringList{"-c", "--", single};
                if (compressStreamToFile(prog, args, cwd, destStr, errorMsg, cancelToken)) return true;
                if (cancelToken && cancelToken->load()) {
                    cleanupPartial();
                    errorMsg = QStringLiteral("Cancelled");
                    return false;
                }
            }
        }
        if (errorMsg.isEmpty()) errorMsg = "Unsupported archive format: " + destStr;
        return false;
    }

    std::vector<ArchiveCmd> cmds;
    if (isTarArchiveName(lower)) {
        QStringList flags;
        if (lower.endsWith(".tar.gz") || lower.endsWith(".tgz")) flags = {"-czf"};
        else if (lower.endsWith(".tar.xz") || lower.endsWith(".txz")) flags = {"-cJf"};
        else if (lower.endsWith(".tar.bz2") || lower.endsWith(".tbz2") || lower.endsWith(".tbz")) flags = {"-cjf"};
        else if (lower.endsWith(".tar.zst") || lower.endsWith(".tzst")) flags = {"--zstd", "-cf"};
        else flags = {"-cf"};
        if (const QString tar = findTool({"tar"}); !tar.isEmpty()) {
            QStringList args = flags;
            args << destStr << "--" << names;
            cmds.push_back({tar, args, cwd});
        }
    } else if (lower.endsWith(".zip")) {
        if (!sevenZip.isEmpty()) {
            QStringList args{"a", "-tzip", "-bso0", "-bsp0", destStr, "--"};
            args += names;
            cmds.push_back({sevenZip, args, cwd});
        }
        if (const QString zip = findTool({"zip"}); !zip.isEmpty()) {
            QStringList args{"-q", "-r", destStr};
            args += safeNames;
            cmds.push_back({zip, args, cwd});
        }
    } else if (lower.endsWith(".7z")) {
        if (!sevenZip.isEmpty()) {
            QStringList args{"a", "-bso0", "-bsp0", destStr, "--"};
            args += names;
            cmds.push_back({sevenZip, args, cwd});
        }
    }

    if (cmds.empty()) {
        errorMsg = "Unsupported archive format: " + destStr;
        return false;
    }
    if (runFirstSuccessful(cmds, errorMsg, cancelToken)) return true;
    if (cancelToken && cancelToken->load()) {
        cleanupPartial();
        errorMsg = QStringLiteral("Cancelled");
    }
    return false;
}

bool XdgUtils::extractArchive(const std::filesystem::path& archivePath,
                              const std::filesystem::path& destination,
                              QString& errorMsg,
                              std::filesystem::path* outActualDestination,
                              std::shared_ptr<std::atomic_bool> cancelToken) {
    if (cancelToken && cancelToken->load()) {
        errorMsg = QStringLiteral("Cancelled");
        return false;
    }
    const QString arcStr = QString::fromUtf8(archivePath.c_str());
    const QString lower = arcStr.toLower();
    if (!isTarArchiveName(lower) && !isSevenZipArchiveName(lower) && !isSingleStreamName(lower)) {
        errorMsg = "Unsupported archive format.";
        return false;
    }

    // если имя занято файлом то берем свободное имя
    // поэтому берём свободное «имя (2)»; существующий каталог валиден
    std::filesystem::path dest = destination;
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && !std::filesystem::is_directory(dest, ec)) {
        const std::filesystem::path parent = dest.parent_path();
        const std::string base = dest.filename().string();
        dest.clear();
        for (int n = 2; n < 1000; ++n) {
            std::filesystem::path candidate =
                parent / (base + " (" + std::to_string(n) + ")");
            std::error_code cec;
            if (!std::filesystem::exists(candidate, cec)) {
                dest = std::move(candidate);
                break;
            }
        }
        if (dest.empty()) {
            errorMsg = "Cannot find a free folder name for " +
                       QString::fromUtf8(destination.c_str());
            return false;
        }
    }

    ec.clear();
    const bool destExisted = std::filesystem::exists(dest, ec);
    ec.clear();
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        errorMsg = "Cannot create folder " + QString::fromUtf8(dest.c_str()) + ": " +
                   QString::fromUtf8(ec.message().c_str());
        return false;
    }
    auto cleanupPartial = [&]() {
        if (cancelToken && cancelToken->load() && !destExisted) {
            std::error_code rmEc;
            std::filesystem::remove_all(dest, rmEc);
            if (outActualDestination && !rmEc) outActualDestination->clear();
        }
    };
    if (cancelToken && cancelToken->load()) {
        cleanupPartial();
        errorMsg = QStringLiteral("Cancelled");
        return false;
    }
    if (outActualDestination) *outActualDestination = dest;
    const QString destStr = QString::fromUtf8(dest.c_str());

    const QString sevenZip = findSevenZip();
    const auto buildCmds = [&](const QString& password) {
        std::vector<ArchiveCmd> cmds;
        const QString pw = archivePasswordArg(password);
        if (isTarArchiveName(lower)) {
            if (const QString bsdtar = findTool({"bsdtar"}); !bsdtar.isEmpty())
                cmds.push_back({bsdtar, {"-xf", arcStr, "-C", destStr}, {}});
            if (const QString tar = findTool({"tar"}); !tar.isEmpty())
                cmds.push_back({tar, {"-xf", arcStr, "-C", destStr}, {}});
        } else {
            if (!sevenZip.isEmpty())
                cmds.push_back({sevenZip, {"x", "-y", "-bso0", "-bsp0", "-p" + pw,
                                           "-o" + destStr, "--", arcStr}, {}});
            if (const QString bsdtar = findTool({"bsdtar"}); !bsdtar.isEmpty())
                cmds.push_back({bsdtar, {"--passphrase", pw,
                                         "-xf", arcStr, "-C", destStr}, {}});
            if (lower.endsWith(".zip")) {
                if (const QString unzip = findTool({"unzip"}); !unzip.isEmpty())
                    cmds.push_back({unzip, {"-q", "-o", "-P", pw,
                                            arcStr, "-d", destStr}, {}});
            }
        }
        return cmds;
    };

    if (buildCmds(QString()).empty()) {
        errorMsg = "No extractor available (install 7z or tar).";
        return false;
    }
    if (runArchiveCommand(arcStr, buildCmds, errorMsg, cancelToken)) return true;
    if (cancelToken && cancelToken->load()) {
        cleanupPartial();
        errorMsg = QStringLiteral("Cancelled");
    }
    return false;
}

bool XdgUtils::extractArchiveEntries(const std::filesystem::path& archivePath,
                                     const QStringList& entryPaths,
                                     const std::filesystem::path& destinationFolder,
                                     QString& errorMsg,
                                     std::shared_ptr<std::atomic_bool> cancelToken) {
    if (cancelToken && cancelToken->load()) {
        errorMsg = QStringLiteral("Cancelled");
        return false;
    }
    const QString arcStr = QString::fromUtf8(archivePath.c_str());
    const QString lower = arcStr.toLower();
    if (!isTarArchiveName(lower) && !isSevenZipArchiveName(lower) && !isSingleStreamName(lower)) {
        errorMsg = "Unsupported archive format.";
        return false;
    }

    // одиночный поток: выборочной распаковки нет, разворачиваем целиком
    if (isSingleStreamName(lower) && !isTarArchiveName(lower)) {
        return extractArchive(archivePath, destinationFolder, errorMsg, nullptr, cancelToken);
    }

    std::error_code ec;
    const bool destExisted = std::filesystem::exists(destinationFolder, ec);
    ec.clear();
    std::filesystem::create_directories(destinationFolder, ec);
    if (ec) {
        errorMsg = "Cannot create folder " + QString::fromUtf8(destinationFolder.c_str()) + ": " +
                   QString::fromUtf8(ec.message().c_str());
        return false;
    }
    auto cleanupPartial = [&]() {
        if (cancelToken && cancelToken->load() && !destExisted) {
            std::error_code rmEc;
            std::filesystem::remove_all(destinationFolder, rmEc);
        }
    };
    if (cancelToken && cancelToken->load()) {
        cleanupPartial();
        errorMsg = QStringLiteral("Cancelled");
        return false;
    }

    // имена подставляем как в листинге (tar хранит «./sub/…», 7z/zip без
    // завершающий слеш срезаем а ведущий минус защищаем точкой со слешем
    QStringList names;
    for (const QString& raw : entryPaths) {
        QString n = raw;
        while (n.endsWith('/')) n.chop(1);
        if (n.isEmpty()) continue;
        names << (n.startsWith('-') ? "./" + n : n);
    }
    if (names.isEmpty()) {
        errorMsg = "No entries selected.";
        return false;
    }

    const QString destStr = QString::fromUtf8(destinationFolder.c_str());
    const QString sevenZip = findSevenZip();
    const auto buildCmds = [&](const QString& password) {
        std::vector<ArchiveCmd> cmds;
        const QString pw = archivePasswordArg(password);
        if (isTarArchiveName(lower)) {
            // 7z не подходит для tar архивов так как распакует только внутренний тарбол
            if (const QString bsdtar = findTool({"bsdtar"}); !bsdtar.isEmpty()) {
                QStringList args{"-xf", arcStr, "-C", destStr, "--"};
                args += names;
                cmds.push_back({bsdtar, args, {}});
            }
            if (const QString tar = findTool({"tar"}); !tar.isEmpty()) {
                QStringList args{"-xf", arcStr, "-C", destStr, "--"};
                args += names;
                cmds.push_back({tar, args, {}});
            }
        } else {
            if (!sevenZip.isEmpty()) {
                QStringList args{"x", "-y", "-bso0", "-bsp0", "-p" + pw,
                                 "-o" + destStr, "--", arcStr};
                args += names;
                cmds.push_back({sevenZip, args, {}});
            }
            // bsdtar умеет работать с зип и 7z и находит пути со слешем и без него
            if (const QString bsdtar = findTool({"bsdtar"}); !bsdtar.isEmpty()) {
                QStringList args{"--passphrase", pw,
                                 "-xf", arcStr, "-C", destStr, "--"};
                args += names;
                cmds.push_back({bsdtar, args, {}});
            }
        }
        return cmds;
    };

    if (buildCmds(QString()).empty()) {
        errorMsg = "No extractor available (install 7z or tar).";
        return false;
    }
    if (runArchiveCommand(arcStr, buildCmds, errorMsg, cancelToken)) return true;
    if (cancelToken && cancelToken->load()) {
        cleanupPartial();
        errorMsg = QStringLiteral("Cancelled");
    }
    return false;
}

bool XdgUtils::listArchive(const std::filesystem::path& archivePath,
                           std::vector<ArchiveEntry>& outEntries,
                           bool& outTruncated, QString& outError) {
    outEntries.clear();
    outTruncated = false;
    outError.clear();

    const QString arcStr = QString::fromUtf8(archivePath.c_str());
    const QString lower = arcStr.toLower();
    constexpr int kMaxEntries = 2000;
    constexpr qint64 kMaxOutput = 8 * 1024 * 1024;

    const bool tarFamily = isTarArchiveName(lower);
    const QString bsdtar = findTool({"bsdtar"});
    QString prog;
    if (tarFamily) {
        if (bsdtar.isEmpty()) {
            outError = "bsdtar not found";
            return false;
        }
        prog = bsdtar;
    } else {
        const QString sevenZip = findSevenZip();
        if (!sevenZip.isEmpty()) {
            prog = sevenZip;
        } else if (!bsdtar.isEmpty()) {
            prog = bsdtar;
        } else {
            outError = "No archive lister available (install 7z).";
            return false;
        }
    }

    auto buildArgs = [&](const QString& password) {
        const QString pw = archivePasswordArg(password);
        if (prog == bsdtar) return QStringList{"--passphrase", pw, "-tvf", arcStr};
        return QStringList{"l", "-slt", "-ba", "-p" + pw, "--", arcStr};
    };

    QString password = cachedArchivePassword(arcStr);
    QByteArray raw;
    bool outputOverflow = false;
    for (int attempt = 0; ; ++attempt) {
        QProcess proc;
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("LC_ALL", "C.UTF-8");
        proc.setProcessEnvironment(env);
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.start(prog, buildArgs(password));
        if (!proc.waitForStarted(5000)) {
            outError = proc.errorString();
            return false;
        }
        // stdout читаем порциями с жестким лимитом
        // иначе весь листинг в памяти съедал бы всю оперативку
        raw.clear();
        outputOverflow = false;
        QElapsedTimer listingTimer;
        listingTimer.start();
        while (true) {
            const QByteArray chunk = proc.readAllStandardOutput();
            if (!chunk.isEmpty()) {
                if (raw.size() + chunk.size() > kMaxOutput) {
                    raw.append(chunk.left(kMaxOutput - raw.size()));
                    outputOverflow = true;
                    outTruncated = true;
                    proc.kill();
                    proc.waitForFinished(1000);
                    break;
                }
                raw.append(chunk);
            }
            if (proc.state() == QProcess::NotRunning) break;
            if (listingTimer.elapsed() > 30000) {
                proc.kill();
                proc.waitForFinished(1000);
                outError = QStringLiteral("listing timed out");
                return false;
            }
            proc.waitForReadyRead(1000);
        }
        if (!outputOverflow &&
            (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)) {
            outError = QString::fromUtf8(proc.readAllStandardError()).trimmed();
            if (outError.isEmpty()) outError = "listing failed";
            if (!looksLikePasswordError(outError)) return false;
            if (attempt >= 2) {
                outError = QStringLiteral("Wrong archive password.");
                return false;
            }
            password = askArchivePassword(arcStr, attempt > 0);
            if (password.isEmpty()) {
                outError = archivePasswordError();
                return false;
            }
            continue;
        }
        if (!password.isEmpty()) rememberArchivePassword(arcStr, password);
        outError.clear();
        break;
    }
    if (raw.size() > kMaxOutput) {
        raw.truncate(kMaxOutput);
        outTruncated = true;
    }
    const QString text = QString::fromUtf8(raw);

    if (prog == bsdtar) {
        // разбор стандартной строки листинга bsdtar
        static const QRegularExpression re(
            QStringLiteral(R"(^(\S+)\s+\d+\s+\S+\s+\S+\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*)$)"));
        const QStringList lines = text.split('\n');
        for (const QString& line : lines) {
            if (line.trimmed().isEmpty()) continue;
            if (outEntries.size() >= kMaxEntries) { outTruncated = true; break; }
            const QRegularExpressionMatch m = re.match(line);
            if (!m.hasMatch()) continue;
            ArchiveEntry e;
            e.isDir = m.captured(1).startsWith('d');
            e.size = m.captured(2).toULongLong();
            e.modified = QString("%1 %2 %3").arg(m.captured(3), m.captured(4), m.captured(5));
            // у симлинков bsdtar печатает стрелку и цель в имя не входит
            // входит, иначе выборочная распаковка никогда не матчится
            QString entryPath = m.captured(6);
            const int arrow = entryPath.indexOf(QStringLiteral(" -> "));
            if (arrow >= 0) entryPath.truncate(arrow);
            e.path = entryPath;
            if (e.isDir && !e.path.endsWith('/')) e.path += '/';
            outEntries.push_back(std::move(e));
        }
        return true;
    }

    // листинг 7z разбирает блоки ключ значение разделенные пустой строкой
    ArchiveEntry cur;
    bool have = false;
    auto flush = [&]() {
        if (have && !cur.path.isEmpty()) {
            if (cur.isDir && !cur.path.endsWith('/')) cur.path += '/';
            outEntries.push_back(cur);
        }
        cur = ArchiveEntry();
        have = false;
    };
    const QStringList lines = text.split('\n');
    for (const QString& lineRaw : lines) {
        const QString line = lineRaw.endsWith('\r') ? lineRaw.chopped(1) : lineRaw;
        if (line.trimmed().isEmpty()) { flush(); continue; }
        const int eq = line.indexOf(QStringLiteral(" = "));
        if (eq < 0) continue;
        const QString key = line.left(eq);
        const QString val = line.mid(eq + 3);
        if (key == "Path") {
            flush();
            cur.path = val;
            have = true;
        } else if (key == "Size") {
            cur.size = val.toULongLong();
        } else if (key == "Folder") {
            if (val == "+") cur.isDir = true;
        } else if (key == "Attributes") {
            if (val.startsWith('D')) cur.isDir = true;
        } else if (key == "Modified") {
            cur.modified = val.trimmed();
        }
        if (outEntries.size() >= kMaxEntries) { outTruncated = true; break; }
    }
    flush();
    if (outEntries.size() > kMaxEntries) {
        outEntries.resize(kMaxEntries);
        outTruncated = true;
    }
    return true;
}

}
