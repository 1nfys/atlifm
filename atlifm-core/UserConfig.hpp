#pragma once

// дефолтные конфиги при первом запуске

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTextStream>
#include <utility>

#include "Keybinds.hpp"
#include "../atlifm-theme/Theme.hpp"

namespace Core::UserConfig {

inline bool writeIfMissing(const QString& path, const QString& content) {
    if (QFileInfo::exists(path)) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out << content;
    return true;
}

inline void migrateSearchFilterBind() {
    const QString path = Theme::configDir() + QStringLiteral("/keybinds.conf");
    if (!QFileInfo::exists(path)) return;

    QStringList lines;
    {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream ts(&in);
        while (!ts.atEnd()) lines.append(ts.readLine());
    }

    bool hasFilter = false;
    bool changed = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        const int eq = t.indexOf('=');
        if (eq <= 0) continue;
        const QString id = t.left(eq).trimmed();
        const QString value = t.mid(eq + 1).trimmed();
        if (id == QLatin1String("filter")) {
            hasFilter = true;
            QStringList keys = Keybinds::parseKeys(value);
            if (!keys.contains(QStringLiteral("Ctrl+F"))) {
                keys.prepend(QStringLiteral("Ctrl+F"));
            }
            keys.removeAll(QStringLiteral("Ctrl+Shift+F"));
            const QString rebuilt = QStringLiteral("filter = %1")
                                        .arg(keys.join(QStringLiteral(", ")));
            if (rebuilt != lines.at(i)) {
                lines[i] = rebuilt;
                changed = true;
            }
        } else if (id == QLatin1String("search_command") ||
                   id == QLatin1String("search_next") ||
                   id == QLatin1String("search_prev")) {
            lines[i] = QStringLiteral("# removed: %1").arg(t);
            changed = true;
        }
    }

    if (!hasFilter) {
        int insertAt = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).contains(QLatin1String("SEARCH & FILTER"))) {
                insertAt = i + 1;
                break;
            }
        }
        if (insertAt < 0) insertAt = lines.size();
        lines.insert(insertAt, QStringLiteral("filter = Ctrl+F"));
        changed = true;
    }

    if (!changed) return;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream os(&out);
    for (const QString& line : lines) os << line << '\n';
}

// миграция старых биндов где одиночные клавиши мешали поиску
inline void migrateLegacySingleLetterBinds() {
    const QString path = Theme::configDir() + QStringLiteral("/keybinds.conf");
    if (!QFileInfo::exists(path)) return;

    QStringList lines;
    {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream ts(&in);
        while (!ts.atEnd()) lines.append(ts.readLine());
    }

    bool hasTasks = false;
    bool changed = false;
    int quickJumpLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        const int eq = t.indexOf('=');
        if (eq <= 0) continue;
        const QString id = t.left(eq).trimmed();
        const QString value = t.mid(eq + 1).trimmed();
        if (id == QLatin1String("tasks")) hasTasks = true;
        if (id == QLatin1String("quick_jump")) {
            quickJumpLine = i;
            if (value.compare(QLatin1String("g"), Qt::CaseInsensitive) == 0 ||
                value.compare(QLatin1String("Alt+G"), Qt::CaseInsensitive) == 0) {
                lines[i] = QStringLiteral("quick_jump = Alt+Space");
                changed = true;
            }
        } else if (id == QLatin1String("tasks") &&
                   value.compare(QLatin1String("w"), Qt::CaseInsensitive) == 0) {
            lines[i] = QStringLiteral("tasks = Alt+W");
            changed = true;
        }
    }

    if (!changed) return;
    if (!hasTasks) {
        const int at = quickJumpLine >= 0 ? quickJumpLine + 1 : lines.size();
        lines.insert(at, QStringLiteral("tasks = Alt+W"));
    }

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream os(&out);
    for (const QString& line : lines) os << line << '\n';
}

// шаблон конфига клавиш
inline QString keybindsTemplate() {
    QString out;

    QString section;
    for (const Keybinds::Action& a : Keybinds::actionTable()) {
        if (a.section != section) {
            section = a.section;
            out += QStringLiteral("\n# %1\n").arg(section);
        }
        out += QStringLiteral("%1 = %2\n")
                   .arg(a.id, Keybinds::parseKeys(a.defaults).join(QStringLiteral(", ")));
    }
    return out;
}

// шаблон конфига темы
inline QString themeTemplate() {
    const Theme::PaletteDefinition p = Theme::palette();
    auto hex = [](const QColor& c) {
        return c.alpha() < 255 ? c.name(QColor::HexArgb) : c.name(QColor::HexRgb);
    };

    QString out;

    out += QStringLiteral("dark = %1\n")
               .arg(p.isDark ? QStringLiteral("true") : QStringLiteral("false"));

    out += QStringLiteral("\n# Window & Font\n");
    out += QStringLiteral("# window_width = 1200\n");
    out += QStringLiteral("# window_height = 750\n");
    out += QStringLiteral("# sidebar_width = 180\n");
    out += QStringLiteral("# font_family = DejaVu Sans Mono\n");
    out += QStringLiteral("# font_size = 13\n");

    out += QStringLiteral("\n# Layout\n");
    out += QStringLiteral("# Ctrl+Shift+S saves the current layout to layout.ini;\n");
    out += QStringLiteral("# Ctrl+Shift+R removes it and restores these defaults.\n");
    out += QStringLiteral("# show_tabs = true\n");
    out += QStringLiteral("# show_bookmarks = true\n");
    out += QStringLiteral("# show_status_bar = true\n");
    out += QStringLiteral("# show_parent_column = true\n");
    out += QStringLiteral("# show_preview = true\n");
    out += QStringLiteral("# parent_column_width = 300\n");
    out += QStringLiteral("# preview_width = 420\n");

    out += QStringLiteral("\n# Search\n");
    out += QStringLiteral("# search_hidden = true\n");
    out += QStringLiteral("# search_ignored = false\n");
    out += QStringLiteral("# search_max_results = 1500\n");

    out += QStringLiteral("\n# Palette\n");
    const std::pair<const char*, QColor> atoms[] = {
        {"base", p.base},         {"mantle", p.mantle},
        {"crust", p.crust},       {"surface0", p.surface0},
        {"surface1", p.surface1}, {"surface2", p.surface2},
        {"overlay0", p.overlay0}, {"text", p.text},
        {"subtext", p.subtext},   {"dim", p.dim},
        {"accent", p.accent},     {"accent_fg", p.accentFg},
        {"blue", p.blue},         {"sapphire", p.sapphire},
        {"yellow", p.yellow},     {"green", p.green},
        {"red", p.red},           {"mauve", p.mauve},
        {"peach", p.peach},       {"teal", p.teal},
    };
    for (const auto& [key, color] : atoms)
        out += QStringLiteral("%1 = %2\n").arg(key, hex(color));

    out += QStringLiteral("\n# UI Components\n");
    const std::pair<const char*, QColor> roles[] = {
        {"border", Theme::Colors::Border()},
        {"selection_bg", Theme::Colors::SelectionBg()},
        {"selection_fg", Theme::Colors::SelectionFg()},
        {"hover_bg", Theme::Colors::HoverBg()},
        {"drop_bg", Theme::Colors::DropBg()},
        {"drop_border", Theme::Colors::DropBorder()},
        {"tab_bg", Theme::Colors::TabBg()},
        {"tab_fg", Theme::Colors::TabFg()},
        {"tab_active_bg", Theme::Colors::TabActiveBg()},
        {"tab_active_fg", Theme::Colors::TabActiveFg()},
        {"tab_active_text", Theme::Colors::TabActiveText()},
        {"tab_hover_bg", Theme::Colors::TabHoverBg()},
        {"tab_close_fg", Theme::Colors::TabCloseFg()},
        {"tab_close_hover_fg", Theme::Colors::TabCloseHoverFg()},
        {"menu_bg", Theme::Colors::MenuBg()},
        {"menu_fg", Theme::Colors::MenuFg()},
        {"menu_border", Theme::Colors::MenuBorder()},
        {"menu_selected_bg", Theme::Colors::MenuSelectedBg()},
        {"menu_selected_fg", Theme::Colors::MenuSelectedFg()},
        {"menu_disabled_fg", Theme::Colors::MenuDisabledFg()},
        {"input_bg", Theme::Colors::InputBg()},
        {"input_fg", Theme::Colors::InputFg()},
        {"input_border", Theme::Colors::InputBorder()},
        {"input_focus_border", Theme::Colors::InputFocusBorder()},
        {"button_bg", Theme::Colors::ButtonBg()},
        {"button_fg", Theme::Colors::ButtonFg()},
        {"button_border", Theme::Colors::ButtonBorder()},
        {"button_hover_bg", Theme::Colors::ButtonHoverBg()},
        {"button_hover_fg", Theme::Colors::ButtonHoverFg()},
        {"checkbox_border", Theme::Colors::CheckboxBorder()},
        {"checkbox_checked_bg", Theme::Colors::CheckboxCheckedBg()},
        {"scrollbar_bg", Theme::Colors::ScrollbarBg()},
        {"scrollbar_handle", Theme::Colors::ScrollbarHandle()},
        {"scrollbar_handle_hover", Theme::Colors::ScrollbarHandleHover()},
        {"rubberband_border", Theme::Colors::RubberbandBorder()},
        {"rubberband_bg", Theme::Colors::RubberbandBg()},
        {"overlay_bg", Theme::Colors::OverlayBg()},
        {"overlay_border", Theme::Colors::OverlayBorder()},
        {"badge_bg", Theme::Colors::BadgeBg()},
        {"badge_border", Theme::Colors::BadgeBorder()},
        {"badge_fg", Theme::Colors::BadgeFg()},
    };
    for (const auto& [key, color] : roles)
        out += QStringLiteral("# %1 = %2\n").arg(key, hex(color));
    return out;
}

// шаблон конфига плагина
inline QString pluginExampleTemplate() {
    return QStringLiteral(
        "# Name = Open in VS Code\n"
        "# Exec = code\n"
        "# Extensions = .cpp, .hpp\n"
        "# AppliesTo = files\n"
        "# Detach = false\n"
        "# Description = Open in VS Code\n"
        "# Action = open_in_vscode\n"
        "# Keys = Ctrl+Alt+V\n"
        "# Override = open, open_with\n"
        "# Type = preview\n"
        "# Hook = on_start, on_navigate, on_select\n");
}

inline void ensureDefaultConfigs() {
    const QString dir = Theme::configDir();
    if (dir.isEmpty()) return;

    QDir().mkpath(dir);
    writeIfMissing(dir + QStringLiteral("/keybinds.conf"), keybindsTemplate());
    writeIfMissing(dir + QStringLiteral("/theme.conf"), themeTemplate());
    migrateSearchFilterBind();
    migrateLegacySingleLetterBinds();

    const QString pluginsDir = dir + QStringLiteral("/plugins");
    if (!QDir(pluginsDir).exists()) {
        QDir().mkpath(pluginsDir);
        writeIfMissing(pluginsDir + QStringLiteral("/example.conf"),
                       pluginExampleTemplate());
    }
}

}
