#pragma once

// бинды клавиш из конфига и дефолты

#include <QString>
#include <QStringList>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QHash>
#include <QKeySequence>
#include <vector>
#include <cstdlib>
#include "Plugins.hpp"

namespace Core::Keybinds {

struct Action {
    QString id;          // ключ в конфиге
    QString defaults;    // дефолтные клавиши
    QString section;     // секция в читшите
    QString label;       // описание
    bool alwaysActive;   // работает даже в текстовых полях
};

// дефолтная таблица действий
inline const std::vector<Action>& actionTable() {
    static const std::vector<Action> table = [] {
        std::vector<Action> t = {
            // вкладки
            {"new_tab",          "Ctrl+T",                    "TABS", "New tab in $HOME", false},
            {"close_tab",        "Ctrl+W, Ctrl+F4",           "TABS", "Close active tab", false},
            {"reopen_tab",       "Ctrl+Shift+T",              "TABS", "Reopen recently closed tab", false},
            {"pin_tab",          "Ctrl+P",                    "TABS", "Pin / unpin active tab", false},
            {"next_tab",         "Ctrl+Tab, Ctrl+PgDown",     "TABS", "Next tab", false},
            {"prev_tab",         "Ctrl+Shift+Tab, Ctrl+PgUp", "TABS", "Previous tab", false},
            {"open_in_new_tab",  "Ctrl+Return, Ctrl+Enter",   "TABS", "Open selected folder in new tab", false},
            // навигация и сортировка
            {"focus_path",       "Ctrl+L, Alt+D, F4",         "NAVIGATION & SORTING", "Focus address / path input", true},
            {"parent_dir",       "Alt+Up",                    "NAVIGATION & SORTING", "Parent folder", false},
            {"go_back",          "Alt+Left",                  "NAVIGATION & SORTING", "History back", false},
            {"go_forward",       "Alt+Right",                 "NAVIGATION & SORTING", "History forward", false},
            {"go_home",          "Alt+Home",                  "NAVIGATION & SORTING", "Go to Home directory", false},
            {"first_item",       "Home",                      "NAVIGATION & SORTING", "Go to first item", false},
            {"last_item",        "End",                       "NAVIGATION & SORTING", "Go to last item", false},
            {"quick_jump",       "Alt+Space",                 "NAVIGATION & SORTING", "Quick Jump menu", false},
            {"toggle_bookmarks", "Ctrl+B, Ctrl+\\, F9",       "NAVIGATION & SORTING", "Toggle bookmarks sidebar", false},
            {"toggle_hidden",    "Ctrl+H, Ctrl+.",            "NAVIGATION & SORTING", "Show / hide hidden files", false},
            {"refresh",          "Ctrl+R, F5",                "NAVIGATION & SORTING", "Refresh directory", false},
            {"tasks",            "Alt+W",                     "NAVIGATION & SORTING", "Background tasks (progress / cancel / log)", false},
            {"sort_name",        "Alt+1",                     "NAVIGATION & SORTING", "Sort by Name", false},
            {"sort_size",        "Alt+2",                     "NAVIGATION & SORTING", "Sort by Size", false},
            {"sort_date",        "Alt+3",                     "NAVIGATION & SORTING", "Sort by Date", false},
            {"sort_type",        "Alt+4",                     "NAVIGATION & SORTING", "Sort by Type", false},
            // раскладка сохраняет панели или сбрасывает на дефолт
            {"save_layout",      "Ctrl+Shift+S",              "LAYOUT", "Save current layout", false},
            {"reset_layout",     "Ctrl+Shift+R",              "LAYOUT", "Reset layout to theme.conf defaults", false},
            // файловые операции
            {"cut",              "Ctrl+X",                    "FILE OPERATIONS", "Cut selection", false},
            {"copy",             "Ctrl+C",                    "FILE OPERATIONS", "Copy selection", false},
            {"paste",            "Ctrl+V",                    "FILE OPERATIONS", "Paste", false},
            {"copy_path",        "Ctrl+Shift+C",              "FILE OPERATIONS", "Copy full absolute path", false},
            {"copy_rel_path",    "Ctrl+Alt+C",                "FILE OPERATIONS", "Copy relative path", false},
            {"extract_here",     "Ctrl+Alt+X",                "FILE OPERATIONS", "Extract archive into current folder", false},
            {"select_all",       "Ctrl+A",                    "FILE OPERATIONS", "Select all items", false},
            {"undo",             "Ctrl+Z",                    "FILE OPERATIONS", "Undo file operation", false},
            {"redo",             "Ctrl+Y, Ctrl+Shift+Z",      "FILE OPERATIONS", "Redo file operation", false},
            {"rename",           "F2",                        "FILE OPERATIONS", "Rename file / folder", false},
            {"trash",            "Del",                       "FILE OPERATIONS", "Move to Trash", false},
            {"delete_perm",      "Shift+Del",                 "FILE OPERATIONS", "Delete permanently", false},
            {"new_folder",       "Ctrl+Shift+N",              "FILE OPERATIONS", "Create new folder", false},
            {"new_file",         "Ctrl+N, Ctrl+Alt+N",        "FILE OPERATIONS", "Create new file", false},
            {"create_bulk",      "Ctrl+Shift+A",              "FILE OPERATIONS", "Create multiple files / folders", false},
            {"properties",       "Alt+Return, Alt+Enter",     "FILE OPERATIONS", "Properties (compact overlay)", false},
            // поиск и фильтр
            {"filter",           "Ctrl+F",                    "SEARCH & FILTER", "Open fuzzy filter bar", true},
            {"cancel",           "Esc",                       "SEARCH & FILTER", "Cancel prompt / clear filter & selection", true},
            // общее
            {"help",             "F1, ?",                     "GENERAL", "Show this help window", false},
            {"quit",             "Ctrl+Q",                    "GENERAL", "Quit atlifm", true},
        };
        std::vector<Action> tabs;
        for (int i = 1; i <= 9; ++i) {
            tabs.push_back({
                QString("tab_%1").arg(i),
                QString("Ctrl+%1").arg(i),
                "TABS",
                QString("Switch directly to tab %1").arg(i),
                false,
            });
        }
        // вкладки с первой по девятую
        t.insert(t.begin() + 6, tabs.begin(), tabs.end());
        return t;
    }();
    return table;
}

inline QString configDir() {
    const char* override = std::getenv("ATLIFM_CONFIG_DIR");
    if (override && *override) return QString::fromUtf8(override);
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return QString::fromUtf8(home) + "/.config/atlifm";
}

inline QString configPath() {
    const QString dir = configDir();
    return dir.isEmpty() ? QString() : dir + "/keybinds.conf";
}

// алиасы клавиш для qt
inline QString normalizeKeySpec(QString spec) {
    spec = spec.trimmed();
    static const QHash<QString, QString> aliases = {
        {"Escape", "Esc"}, {"Delete", "Del"},
        {"PageDown", "PgDown"}, {"Page Down", "PgDown"},
        {"PageUp", "PgUp"}, {"Page Up", "PgUp"},
        {"BackSpace", "Backspace"},
    };
    return aliases.value(spec, spec);
}

inline QStringList parseKeys(const QString& spec) {
    QStringList keys;
    const QStringList parts = spec.split(',', Qt::SkipEmptyParts);
    for (const QString& raw : parts) {
        const QString norm = normalizeKeySpec(raw);
        if (norm.isEmpty()) continue;
        // берем только валидные последовательности
        const QString canonical = QKeySequence::fromString(norm)
                                      .toString(QKeySequence::PortableText);
        if (canonical.isEmpty()) continue;
        keys.append(canonical);
    }
    return keys;
}

inline QHash<QString, QStringList>& cache() {
    static QHash<QString, QStringList> c;
    return c;
}

// действия из плагинов без дублей
inline QHash<QString, QString> acceptedPluginActions() {
    QHash<QString, QString> out;
    for (const Core::Plugins::Plugin& p : Core::Plugins::load()) {
        if (p.action.isEmpty() || out.contains(p.action)) continue;
        bool builtin = false;
        for (const Action& a : actionTable())
            if (a.id == p.action) { builtin = true; break; }
        if (builtin) continue;
        out.insert(p.action, p.id);
    }
    return out;
}

inline const QHash<QString, QStringList>& loaded() {
    if (!cache().isEmpty()) return cache();

    auto& c = cache();
    for (const Action& a : actionTable())
        c.insert(a.id, parseKeys(a.defaults));

    // подмешиваем дефолты плагинов
    for (const Core::Plugins::Plugin& p : Core::Plugins::load()) {
        if (p.action.isEmpty() || c.contains(p.action)) continue;
        QStringList defaults;
        for (const QString& spec : p.keys) defaults += parseKeys(spec);
        c.insert(p.action, defaults);
    }

    QFile file(configPath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            const int eq = line.indexOf('=');
            if (eq <= 0) continue;
            const QString id = line.left(eq).trimmed();
            const QString val = line.mid(eq + 1).trimmed();
            if (!c.contains(id)) continue; // неизвестные действия пропускаем
            c.insert(id, parseKeys(val));  // пусто значит отключено
        }
    }
    return c;
}

inline QStringList keysFor(const QString& actionId) {
    return loaded().value(actionId);
}

// поиск конфликтов для читшита и сохранения
inline QString conflictForAction(const QString& actionId, const QString& key) {
    const QString canonical = QKeySequence(key).toString(QKeySequence::PortableText);
    if (canonical.isEmpty()) return {};
    for (const Action& action : actionTable()) {
        if (action.id == actionId) continue;
        if (keysFor(action.id).contains(canonical)) return action.id;
    }
    const auto pluginActions = acceptedPluginActions();
    for (auto it = pluginActions.constBegin(); it != pluginActions.constEnd(); ++it) {
        if (it.key() == actionId) continue;
        if (keysFor(it.key()).contains(canonical)) return it.key();
    }
    return {};
}

// дефолтные клавиши для сброса
inline QStringList defaultKeysFor(const QString& actionId) {
    for (const Action& a : actionTable())
        if (a.id == actionId) return parseKeys(a.defaults);
    for (const Core::Plugins::Plugin& p : Core::Plugins::load()) {
        if (p.action != actionId) continue;
        QStringList out;
        for (const QString& spec : p.keys) out += parseKeys(spec);
        return out;
    }
    return {};
}

// запись бинда в конфиг из читшита
inline bool saveUserBinding(const QString& actionId, const QStringList& keys) {
    const QString path = configPath();
    if (path.isEmpty() || actionId.isEmpty()) return false;
    // при конфликте не пишем чтобы не вешать два обработчика на одну кнопку
    for (const QString& key : keys) {
        if (!conflictForAction(actionId, key).isEmpty()) return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());

    QStringList lines;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&in);
            while (!ts.atEnd()) lines.append(ts.readLine());
        }
    }

    const QString value = QStringList(keys).join(QStringLiteral(", "));
    bool replaced = false;
    for (QString& line : lines) {
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        const int eq = t.indexOf('=');
        if (eq <= 0) continue;
        if (t.left(eq).trimmed() == actionId) {
            line = actionId + QStringLiteral(" = ") + value;
            replaced = true;
            break;
        }
    }
    if (!replaced) lines.append(actionId + QStringLiteral(" = ") + value);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream os(&out);
    for (const QString& l : lines) os << l << '\n';
    return true;
}

// сброс к дефолту удаляет строку из конфига
inline bool resetUserBinding(const QString& actionId) {
    const QString path = configPath();
    if (path.isEmpty() || actionId.isEmpty()) return false;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return true; // если файла нет то уже дефолт

    QStringList lines;
    {
        QTextStream ts(&in);
        while (!ts.atEnd()) {
            const QString line = ts.readLine();
            const QString t = line.trimmed();
            if (!t.isEmpty() && !t.startsWith('#')) {
                const int eq = t.indexOf('=');
                if (eq > 0 && t.left(eq).trimmed() == actionId) continue;
            }
            lines.append(line);
        }
    }

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream os(&out);
    for (const QString& l : lines) os << l << '\n';
    return true;
}

inline bool isAlwaysActive(const QString& actionId) {
    for (const Action& a : actionTable())
        if (actionId == a.id) return a.alwaysActive;
    return false;
}

// сброс кэша биндов
inline void reload() {
    cache().clear();
}

}
