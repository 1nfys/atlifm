#pragma once

// дефолтные конфиги при первом запуске, существующие файлы не перезаписываем

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

// шаблон keybinds.conf со всеми встроенными действиями
inline QString keybindsTemplate() {
    QString out;

    QString section;
    for (const Keybinds::Action& a : Keybinds::actionTable()) {
        if (a.section != section) {
            section = a.section;
            out += QStringLiteral("\n# ---- %1 ----\n").arg(section);
        }
        out += QStringLiteral("%1 = %2\n")
                   .arg(a.id, Keybinds::parseKeys(a.defaults).join(QStringLiteral(", ")));
    }
    return out;
}

// шаблон theme.conf с атомами и закомментированными компонентами ui
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

// шаблон plugins/example.conf
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

    const QString pluginsDir = dir + QStringLiteral("/plugins");
    if (!QDir(pluginsDir).exists()) {
        QDir().mkpath(pluginsDir);
        writeIfMissing(pluginsDir + QStringLiteral("/example.conf"),
                       pluginExampleTemplate());
    }
}

}
