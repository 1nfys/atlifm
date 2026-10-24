#pragma once

#include <QString>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <shared_mutex>
#include <mutex>
#include <cstdlib>
#include <algorithm>

namespace Theme {

// палитра в духе catppuccin с точными тонами логотипа atlifm
struct PaletteDefinition {
    bool isDark{true};
    QColor base{0x0e, 0x0e, 0x25};
    QColor mantle{0x0b, 0x0b, 0x1c};
    QColor crust{0x08, 0x08, 0x16};
    QColor surface0{0x18, 0x18, 0x36};
    QColor surface1{0x23, 0x23, 0x48};
    QColor surface2{0x34, 0x34, 0x5e};
    QColor overlay0{0x45, 0x45, 0x70};
    QColor text{0xcd, 0xd6, 0xf4};
    QColor subtext{0xa6, 0xad, 0xc8};
    QColor dim{0x6c, 0x70, 0x86};
    QColor accent{0x6b, 0xbc, 0xc6};     // голубая буква 'A'
    QColor accentFg{0x0e, 0x0e, 0x25};   // тёмный контрастный текст на акценте
    QColor blue{0x6b, 0xbc, 0xc6};
    QColor sapphire{0x74, 0xc7, 0xec};
    QColor yellow{0xf9, 0xe2, 0xaf};
    QColor green{0xa6, 0xe3, 0xa1};
    QColor red{0xc0, 0x4a, 0x6d};        // розовая загогулина над 'i'
    QColor mauve{0xcb, 0xa6, 0xf7};
    QColor peach{0xc6, 0x8e, 0x6b};      // карамельные буквы 'tlifm'
    QColor teal{0x6b, 0xbc, 0xc6};

    // компоненты ui: необязательные переопределения, пусто (invalid) =
    // фолбэк на палитру или родительскую роль (цепочки в colors::)
    QColor ovBorder;              // все рамки/швы/разделители   → surface1
    QColor ovSelectionBg;         // выделение в списках/меню    → accent
    QColor ovSelectionFg;         // текст выделения             → accentFg
    QColor ovHoverBg;             // подсветка под курсором      → surface0
    QColor ovDropBg;              // цель drag&drop / таргет по ПКМ → surface1
    QColor ovDropBorder;          // рамка/текст цели drag&drop  → accent
    QColor ovTabBg;               // фон таббара                 → base
    QColor ovTabFg;               // неактивная вкладка          → dim
    QColor ovTabActiveBg;         // фон активной вкладки        → base
    QColor ovTabActiveFg;         // префикс с номером активной      → accent
    QColor ovTabActiveText;       // имя активной вкладки        → text
    QColor ovTabHoverBg;          // вкладка под курсором        → hover_bg
    QColor ovTabCloseFg;          // крестик закрытия            → subtext
    QColor ovTabCloseHoverFg;     // крестик под курсором        → red
    QColor ovMenuBg;              // фон меню                    → mantle
    QColor ovMenuFg;              // текст меню                  → text
    QColor ovMenuBorder;          // рамка меню                  → border
    QColor ovMenuSelectedBg;      // выделенный пункт меню       → selection_bg
    QColor ovMenuSelectedFg;      // текст выделенного пункта    → selection_fg
    QColor ovMenuDisabledFg;      // выключенный пункт меню      → dim
    QColor ovInputBg;             // фон полей ввода             → mantle
    QColor ovInputFg;             // текст полей ввода           → text
    QColor ovInputBorder;         // рамка полей ввода           → border
    QColor ovInputFocusBorder;    // рамка поля в фокусе         → accent
    QColor ovButtonBg;            // фон кнопок                  → mantle
    QColor ovButtonFg;            // текст кнопок                → text
    QColor ovButtonBorder;        // рамка кнопок                → border
    QColor ovButtonHoverBg;       // кнопка под курсором         → selection_bg
    QColor ovButtonHoverFg;       // текст кнопки под курсором   → selection_fg
    QColor ovCheckboxBorder;      // рамка чекбокса              → border
    QColor ovCheckboxCheckedBg;   // включённый чекбокс          → selection_bg
    QColor ovScrollbarBg;         // дорожка скроллбара          → base
    QColor ovScrollbarHandle;     // ползунок скроллбара         → surface1
    QColor ovScrollbarHandleHover;// ползунок под курсором       → accent
    QColor ovRubberbandBorder;    // рамка резинки выделения     → accent
    QColor ovRubberbandBg;        // заливка резинки             → accent@60
    QColor ovOverlayBg;           // фон оверлеев/панелей        → base
    QColor ovOverlayBorder;       // рамка оверлеев (2px)        → accent
    QColor ovBadgeBg;             // бейдж drag&drop             → crust
    QColor ovBadgeBorder;         // рамка бейджа                → accent
    QColor ovBadgeFg;             // текст бейджа                → text
};

// глобальный экземпляр активной палитры под защитой shared_mutex
inline PaletteDefinition s_currentPalette;
inline std::shared_mutex s_paletteMutex;

inline PaletteDefinition palette() {
    std::shared_lock lock(s_paletteMutex);
    return s_currentPalette;
}

namespace Colors {
    inline QColor Base()     { std::shared_lock lock(s_paletteMutex); return s_currentPalette.base; }
    inline QColor Mantle()   { std::shared_lock lock(s_paletteMutex); return s_currentPalette.mantle; }
    inline QColor Crust()    { std::shared_lock lock(s_paletteMutex); return s_currentPalette.crust; }
    inline QColor Surface0() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.surface0; }
    inline QColor Surface1() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.surface1; }
    inline QColor Surface2() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.surface2; }
    inline QColor Overlay0() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.overlay0; }
    inline QColor Text()     { std::shared_lock lock(s_paletteMutex); return s_currentPalette.text; }
    inline QColor Subtext()  { std::shared_lock lock(s_paletteMutex); return s_currentPalette.subtext; }
    inline QColor Dim()      { std::shared_lock lock(s_paletteMutex); return s_currentPalette.dim; }
    inline QColor Accent()   { std::shared_lock lock(s_paletteMutex); return s_currentPalette.accent; }
    inline QColor AccentFg() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.accentFg; }
    inline QColor Blue()     { std::shared_lock lock(s_paletteMutex); return s_currentPalette.blue; }
    inline QColor Sapphire() { std::shared_lock lock(s_paletteMutex); return s_currentPalette.sapphire; }
    inline QColor Yellow()   { std::shared_lock lock(s_paletteMutex); return s_currentPalette.yellow; }
    inline QColor Green()    { std::shared_lock lock(s_paletteMutex); return s_currentPalette.green; }
    inline QColor Red()      { std::shared_lock lock(s_paletteMutex); return s_currentPalette.red; }
    inline QColor Mauve()    { std::shared_lock lock(s_paletteMutex); return s_currentPalette.mauve; }
    inline QColor Peach()    { std::shared_lock lock(s_paletteMutex); return s_currentPalette.peach; }
    inline QColor Teal()     { std::shared_lock lock(s_paletteMutex); return s_currentPalette.teal; }
    inline QColor Cyan()     { return Teal(); }

    // фолбэк роли: theme.conf → палитра → родительская роль,
    // без конфига цвета ровно прежние
    inline QColor Border() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovBorder.isValid() ? s_currentPalette.ovBorder
                                                   : s_currentPalette.surface1;
    }
    inline QColor SelectionBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovSelectionBg.isValid() ? s_currentPalette.ovSelectionBg
                                                        : s_currentPalette.accent;
    }
    inline QColor SelectionFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovSelectionFg.isValid() ? s_currentPalette.ovSelectionFg
                                                        : s_currentPalette.accentFg;
    }
    inline QColor HoverBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovHoverBg.isValid() ? s_currentPalette.ovHoverBg
                                                    : s_currentPalette.surface0;
    }
    inline QColor DropBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovDropBg.isValid() ? s_currentPalette.ovDropBg
                                                   : s_currentPalette.surface1;
    }
    inline QColor DropBorder() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovDropBorder.isValid() ? s_currentPalette.ovDropBorder
                                                       : s_currentPalette.accent;
    }
    inline QColor TabBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabBg.isValid() ? s_currentPalette.ovTabBg
                                                  : s_currentPalette.base;
    }
    inline QColor TabFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabFg.isValid() ? s_currentPalette.ovTabFg
                                                  : s_currentPalette.dim;
    }
    inline QColor TabActiveBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabActiveBg.isValid() ? s_currentPalette.ovTabActiveBg
                                                        : s_currentPalette.base;
    }
    inline QColor TabActiveFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabActiveFg.isValid() ? s_currentPalette.ovTabActiveFg
                                                        : s_currentPalette.accent;
    }
    inline QColor TabActiveText() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabActiveText.isValid() ? s_currentPalette.ovTabActiveText
                                                          : s_currentPalette.text;
    }
    inline QColor TabHoverBg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovTabHoverBg.isValid()) return s_currentPalette.ovTabHoverBg;
        return s_currentPalette.ovHoverBg.isValid() ? s_currentPalette.ovHoverBg
                                                    : s_currentPalette.surface0;
    }
    inline QColor TabCloseFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabCloseFg.isValid() ? s_currentPalette.ovTabCloseFg
                                                       : s_currentPalette.subtext;
    }
    inline QColor TabCloseHoverFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovTabCloseHoverFg.isValid() ? s_currentPalette.ovTabCloseHoverFg
                                                            : s_currentPalette.red;
    }
    inline QColor MenuBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovMenuBg.isValid() ? s_currentPalette.ovMenuBg
                                                   : s_currentPalette.mantle;
    }
    inline QColor MenuFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovMenuFg.isValid() ? s_currentPalette.ovMenuFg
                                                   : s_currentPalette.text;
    }
    inline QColor MenuBorder() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovMenuBorder.isValid()) return s_currentPalette.ovMenuBorder;
        return s_currentPalette.ovBorder.isValid() ? s_currentPalette.ovBorder
                                                   : s_currentPalette.surface1;
    }
    inline QColor MenuSelectedBg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovMenuSelectedBg.isValid()) return s_currentPalette.ovMenuSelectedBg;
        return s_currentPalette.ovSelectionBg.isValid() ? s_currentPalette.ovSelectionBg
                                                        : s_currentPalette.accent;
    }
    inline QColor MenuSelectedFg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovMenuSelectedFg.isValid()) return s_currentPalette.ovMenuSelectedFg;
        return s_currentPalette.ovSelectionFg.isValid() ? s_currentPalette.ovSelectionFg
                                                        : s_currentPalette.accentFg;
    }
    inline QColor MenuDisabledFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovMenuDisabledFg.isValid() ? s_currentPalette.ovMenuDisabledFg
                                                           : s_currentPalette.dim;
    }
    inline QColor InputBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovInputBg.isValid() ? s_currentPalette.ovInputBg
                                                    : s_currentPalette.mantle;
    }
    inline QColor InputFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovInputFg.isValid() ? s_currentPalette.ovInputFg
                                                    : s_currentPalette.text;
    }
    inline QColor InputBorder() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovInputBorder.isValid()) return s_currentPalette.ovInputBorder;
        return s_currentPalette.ovBorder.isValid() ? s_currentPalette.ovBorder
                                                   : s_currentPalette.surface1;
    }
    inline QColor InputFocusBorder() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovInputFocusBorder.isValid() ? s_currentPalette.ovInputFocusBorder
                                                             : s_currentPalette.accent;
    }
    inline QColor ButtonBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovButtonBg.isValid() ? s_currentPalette.ovButtonBg
                                                     : s_currentPalette.mantle;
    }
    inline QColor ButtonFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovButtonFg.isValid() ? s_currentPalette.ovButtonFg
                                                     : s_currentPalette.text;
    }
    inline QColor ButtonBorder() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovButtonBorder.isValid()) return s_currentPalette.ovButtonBorder;
        return s_currentPalette.ovBorder.isValid() ? s_currentPalette.ovBorder
                                                   : s_currentPalette.surface1;
    }
    inline QColor ButtonHoverBg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovButtonHoverBg.isValid()) return s_currentPalette.ovButtonHoverBg;
        return s_currentPalette.ovSelectionBg.isValid() ? s_currentPalette.ovSelectionBg
                                                        : s_currentPalette.accent;
    }
    inline QColor ButtonHoverFg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovButtonHoverFg.isValid()) return s_currentPalette.ovButtonHoverFg;
        return s_currentPalette.ovSelectionFg.isValid() ? s_currentPalette.ovSelectionFg
                                                        : s_currentPalette.accentFg;
    }
    inline QColor CheckboxBorder() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovCheckboxBorder.isValid()) return s_currentPalette.ovCheckboxBorder;
        return s_currentPalette.ovBorder.isValid() ? s_currentPalette.ovBorder
                                                   : s_currentPalette.surface1;
    }
    inline QColor CheckboxCheckedBg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovCheckboxCheckedBg.isValid()) return s_currentPalette.ovCheckboxCheckedBg;
        return s_currentPalette.ovSelectionBg.isValid() ? s_currentPalette.ovSelectionBg
                                                        : s_currentPalette.accent;
    }
    inline QColor ScrollbarBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovScrollbarBg.isValid() ? s_currentPalette.ovScrollbarBg
                                                        : s_currentPalette.base;
    }
    inline QColor ScrollbarHandle() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovScrollbarHandle.isValid() ? s_currentPalette.ovScrollbarHandle
                                                            : s_currentPalette.surface1;
    }
    inline QColor ScrollbarHandleHover() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovScrollbarHandleHover.isValid() ? s_currentPalette.ovScrollbarHandleHover
                                                                 : s_currentPalette.accent;
    }
    inline QColor RubberbandBorder() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovRubberbandBorder.isValid() ? s_currentPalette.ovRubberbandBorder
                                                             : s_currentPalette.accent;
    }
    inline QColor RubberbandBg() {
        std::shared_lock lock(s_paletteMutex);
        if (s_currentPalette.ovRubberbandBg.isValid()) return s_currentPalette.ovRubberbandBg;
        const QColor a = s_currentPalette.accent;
        return QColor(a.red(), a.green(), a.blue(), 60);
    }
    inline QColor OverlayBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovOverlayBg.isValid() ? s_currentPalette.ovOverlayBg
                                                      : s_currentPalette.base;
    }
    inline QColor OverlayBorder() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovOverlayBorder.isValid() ? s_currentPalette.ovOverlayBorder
                                                          : s_currentPalette.accent;
    }
    inline QColor BadgeBg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovBadgeBg.isValid() ? s_currentPalette.ovBadgeBg
                                                    : s_currentPalette.crust;
    }
    inline QColor BadgeBorder() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovBadgeBorder.isValid() ? s_currentPalette.ovBadgeBorder
                                                        : s_currentPalette.accent;
    }
    inline QColor BadgeFg() {
        std::shared_lock lock(s_paletteMutex);
        return s_currentPalette.ovBadgeFg.isValid() ? s_currentPalette.ovBadgeFg
                                                    : s_currentPalette.text;
    }
}

// каталог конфигов atlifm: ATLIFM_CONFIG_DIR (тесты/рендеры) или ~/.config/atlifm
inline QString configDir() {
    const char* override = std::getenv("ATLIFM_CONFIG_DIR");
    if (override && *override) return QString::fromUtf8(override);
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return QString::fromUtf8(home) + "/.config/atlifm";
}

inline bool setConfigValue(const QString& key, const QString& value) {
    const QString dir = configDir();
    if (dir.isEmpty() || key.isEmpty()) return false;
    const QString path = dir + QStringLiteral("/theme.conf");

    QStringList lines;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&in);
            while (!ts.atEnd()) lines.append(ts.readLine());
        }
    }

    bool replaced = false;
    for (QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) continue;
        const int eq = trimmed.indexOf('=');
        if (eq <= 0) continue;
        if (trimmed.left(eq).trimmed().compare(key, Qt::CaseInsensitive) != 0) continue;
        line = key + QStringLiteral(" = ") + value;
        replaced = true;
        break;
    }
    if (!replaced) lines.append(key + QStringLiteral(" = ") + value);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream os(&out);
    for (const QString& line : lines) os << line << '\n';
    return true;
}

// конфиг темы: ~/.config/atlifm/theme.conf (каталог переопределяется ATLIFM_CONFIG_DIR)
// формат плоский `key = #rrggbb` (или `#aarrggbb` с альфой), `#` в начале строки это комментарий
// два слоя: 1) атомы палитры (base, mantle, ... accent, teal, dark)
// 2) компоненты ui (border, selection_bg, tab_*, menu_*, input_*, button_*,
// checkbox_*, scrollbar_*, rubberband_*, overlay_*, badge_*), незаданный ключ
// наследует из палитры
// приоритет: встроенная палитра < theme.conf
struct UiConfig {
    int windowWidth{1200};
    int windowHeight{750};
    int sidebarWidth{180};
    // layout: какие панели показывать и сколько места отдавать колонкам
    bool showTabBar{true};
    bool showBookmarks{true};
    bool showStatusBar{true};
    bool showParentColumn{true};
    bool showPreview{true};
    int parentColumnWidth{300};
    int previewColumnWidth{420};
    // поиск: искать ли в скрытых/игнорируемых файлах и сколько держать результатов
    bool searchHidden{true};
    bool searchIgnored{false};
    int searchMaxResults{1500};
    QString fontFamily;
    int fontSize{13};
    int fontPointSize{10};
    bool firstRun{true};
};

inline UiConfig& uiConfig() {
    static UiConfig s_config;
    return s_config;
}

inline void applyAppThemeConf(PaletteDefinition& p) {
    const QString dir = configDir();
    if (dir.isEmpty()) return;

    QString path = dir + "/theme.conf";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString key = line.left(eq).trimmed().toLower();
        QString val = line.mid(eq + 1).trimmed();
        if (val.startsWith('"') && val.endsWith('"') && val.size() >= 2) {
            val = val.mid(1, val.size() - 2);
        }
        if (key.isEmpty() || val.isEmpty()) continue;

        if (key == "dark" || key == "is_dark") {
            QString v = val.toLower();
            p.isDark = (v != "false" && v != "0" && v != "light");
            continue;
        }

        if (key == "first_run") {
            const QString v = val.toLower();
            uiConfig().firstRun = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }

        if (key == "window_width" || key == "default_width") {
            bool ok = false;
            int w = val.toInt(&ok);
            if (ok && w >= 400 && w <= 7680) uiConfig().windowWidth = w;
            continue;
        }
        if (key == "window_height" || key == "default_height") {
            bool ok = false;
            int h = val.toInt(&ok);
            if (ok && h >= 300 && h <= 4320) uiConfig().windowHeight = h;
            continue;
        }
        if (key == "sidebar_width" || key == "bookmarks_width") {
            bool ok = false;
            int sw = val.toInt(&ok);
            if (ok && sw >= 80 && sw <= 800) uiConfig().sidebarWidth = sw;
            continue;
        }
        if (key == "parent_width" || key == "parent_column_width") {
            bool ok = false;
            int pw = val.toInt(&ok);
            if (ok && pw >= 80 && pw <= 800) uiConfig().parentColumnWidth = pw;
            continue;
        }
        if (key == "preview_width" || key == "preview_column_width") {
            bool ok = false;
            int pw = val.toInt(&ok);
            if (ok && pw >= 80 && pw <= 1000) uiConfig().previewColumnWidth = pw;
            continue;
        }
        if (key == "show_tabs" || key == "show_tabbar") {
            const QString v = val.toLower();
            uiConfig().showTabBar = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "show_bookmarks" || key == "show_sidebar") {
            const QString v = val.toLower();
            uiConfig().showBookmarks = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "show_status_bar" || key == "show_statusbar") {
            const QString v = val.toLower();
            uiConfig().showStatusBar = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "show_parent_column" || key == "show_parent") {
            const QString v = val.toLower();
            uiConfig().showParentColumn = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "show_preview" || key == "show_preview_pane") {
            const QString v = val.toLower();
            uiConfig().showPreview = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "search_hidden") {
            const QString v = val.toLower();
            uiConfig().searchHidden = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "search_ignored" || key == "search_no_ignore") {
            const QString v = val.toLower();
            uiConfig().searchIgnored = (v != "false" && v != "0" && v != "off" && v != "no");
            continue;
        }
        if (key == "search_max_results") {
            bool ok = false;
            int count = val.toInt(&ok);
            if (ok && count >= 100 && count <= 20000) uiConfig().searchMaxResults = count;
            continue;
        }
        if (key == "font_family") {
            uiConfig().fontFamily = val;
            continue;
        }
        if (key == "font_size") {
            bool ok = false;
            int sz = val.toInt(&ok);
            if (ok && sz >= 8 && sz <= 48) {
                uiConfig().fontSize = sz;
                // px → pt для текстов QPainter (≈96 dpi): round(sz*0.75)
                uiConfig().fontPointSize = std::max(6, (sz * 3 + 2) / 4);
            }
            continue;
        }

        QColor c(val);
        if (!c.isValid()) continue;

        if      (key == "base")      p.base = c;
        else if (key == "mantle")    p.mantle = c;
        else if (key == "crust")     p.crust = c;
        else if (key == "surface0")  p.surface0 = c;
        else if (key == "surface1")  p.surface1 = c;
        else if (key == "surface2")  p.surface2 = c;
        else if (key == "overlay0")  p.overlay0 = c;
        else if (key == "text")      p.text = c;
        else if (key == "subtext")   p.subtext = c;
        else if (key == "dim")       p.dim = c;
        else if (key == "accent")    p.accent = c;
        else if (key == "accent_fg") p.accentFg = c;
        else if (key == "blue")      p.blue = c;
        else if (key == "sapphire")  p.sapphire = c;
        else if (key == "yellow")    p.yellow = c;
        else if (key == "green")     p.green = c;
        else if (key == "red")       p.red = c;
        else if (key == "mauve")     p.mauve = c;
        else if (key == "peach")     p.peach = c;
        else if (key == "teal")      p.teal = c;
        else {
            // компоненты UI (ключ → поле)
            struct RoleKey { const char* key; QColor PaletteDefinition::*field; };
            static const RoleKey roles[] = {
                {"border",                  &PaletteDefinition::ovBorder},
                {"selection_bg",            &PaletteDefinition::ovSelectionBg},
                {"selection_fg",            &PaletteDefinition::ovSelectionFg},
                {"hover_bg",                &PaletteDefinition::ovHoverBg},
                {"drop_bg",                 &PaletteDefinition::ovDropBg},
                {"drop_border",             &PaletteDefinition::ovDropBorder},
                {"tab_bg",                  &PaletteDefinition::ovTabBg},
                {"tab_fg",                  &PaletteDefinition::ovTabFg},
                {"tab_active_bg",           &PaletteDefinition::ovTabActiveBg},
                {"tab_active_fg",           &PaletteDefinition::ovTabActiveFg},
                {"tab_active_text",         &PaletteDefinition::ovTabActiveText},
                {"tab_hover_bg",            &PaletteDefinition::ovTabHoverBg},
                {"tab_close_fg",            &PaletteDefinition::ovTabCloseFg},
                {"tab_close_hover_fg",      &PaletteDefinition::ovTabCloseHoverFg},
                {"menu_bg",                 &PaletteDefinition::ovMenuBg},
                {"menu_fg",                 &PaletteDefinition::ovMenuFg},
                {"menu_border",             &PaletteDefinition::ovMenuBorder},
                {"menu_selected_bg",        &PaletteDefinition::ovMenuSelectedBg},
                {"menu_selected_fg",        &PaletteDefinition::ovMenuSelectedFg},
                {"menu_disabled_fg",        &PaletteDefinition::ovMenuDisabledFg},
                {"input_bg",                &PaletteDefinition::ovInputBg},
                {"input_fg",                &PaletteDefinition::ovInputFg},
                {"input_border",            &PaletteDefinition::ovInputBorder},
                {"input_focus_border",      &PaletteDefinition::ovInputFocusBorder},
                {"button_bg",               &PaletteDefinition::ovButtonBg},
                {"button_fg",               &PaletteDefinition::ovButtonFg},
                {"button_border",           &PaletteDefinition::ovButtonBorder},
                {"button_hover_bg",         &PaletteDefinition::ovButtonHoverBg},
                {"button_hover_fg",         &PaletteDefinition::ovButtonHoverFg},
                {"checkbox_border",         &PaletteDefinition::ovCheckboxBorder},
                {"checkbox_checked_bg",     &PaletteDefinition::ovCheckboxCheckedBg},
                {"scrollbar_bg",            &PaletteDefinition::ovScrollbarBg},
                {"scrollbar_handle",        &PaletteDefinition::ovScrollbarHandle},
                {"scrollbar_handle_hover",  &PaletteDefinition::ovScrollbarHandleHover},
                {"rubberband_border",       &PaletteDefinition::ovRubberbandBorder},
                {"rubberband_bg",           &PaletteDefinition::ovRubberbandBg},
                {"overlay_bg",              &PaletteDefinition::ovOverlayBg},
                {"overlay_border",          &PaletteDefinition::ovOverlayBorder},
                {"badge_bg",                &PaletteDefinition::ovBadgeBg},
                {"badge_border",            &PaletteDefinition::ovBadgeBorder},
                {"badge_fg",                &PaletteDefinition::ovBadgeFg},
            };
            for (const RoleKey& r : roles) {
                if (key == QLatin1String(r.key)) {
                    p.*(r.field) = c;
                    break;
                }
            }
        }
    }
}

inline PaletteDefinition getSystemPalette() {
    PaletteDefinition p;
    applyAppThemeConf(p); // единственное переопределение поверх встроенной
    return p;
}

inline void updatePaletteFromSystem() {
    PaletteDefinition pal = getSystemPalette();
    std::unique_lock lock(s_paletteMutex);
    s_currentPalette = pal;
}

inline void reloadPalette() {
    updatePaletteFromSystem();
}

// только моноширинные шрифты (без утечки системных serif/sans),
// семейство кэшируется, чтобы не запрашивать QFontDatabase при каждой отрисовке делегата
inline QFont getMonospaceFont(int pointSize = 10, QFont::Weight weight = QFont::Normal) {
    static const QString s_resolvedFamily = []() -> QString {
        const QStringList available = QFontDatabase::families();
        const QStringList preferredFamilies = {
            "JetBrainsMono Nerd Font",
            "JetBrainsMono NF",
            "JetBrains Mono",
            "DejaVu Sans Mono",
            "FiraCode Nerd Font",
            "monospace"
        };
        for (const auto& fam : preferredFamilies) {
            if (available.contains(fam)) {
                return fam;
            }
        }
        return QString();
    }();

    QFont font;
    if (!uiConfig().fontFamily.isEmpty()) {
        font.setFamily(uiConfig().fontFamily);
    } else if (!s_resolvedFamily.isEmpty()) {
        font.setFamily(s_resolvedFamily);
    } else {
        font.setStyleHint(QFont::Monospace);
        font.setFamily("monospace");
    }

    // прямые тексты qpainter (делегат, таббар, закладки) не ловят qss размер шрифта
    // масштабируем их пропорционально font size (10 это дефолт или font size=13)
    int effectiveSize = pointSize;
    if (uiConfig().fontPointSize != 10) {
        effectiveSize = std::max(4, (pointSize * uiConfig().fontPointSize + 5) / 10);
    }
    font.setPointSize(effectiveSize);
    font.setWeight(weight);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferFullHinting);
    return font;
}

// плотная непрозрачная таблица стилей (сохраняет точную раскладку виджетов tui)

// qcolor → значение css: hex или rgba() при альфе < 255
inline QString cssColor(const QColor& c) {
    if (c.alpha() >= 255) return c.name();
    return QString("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

inline QString getApplicationStyleSheet() {
    // все компоненты переопределяются theme.conf,
    // без конфига роли дают прежние цвета палитры
    const QString textHex    = cssColor(Colors::Text());              // %1
    const QString baseHex    = cssColor(Colors::Base());              // %2
    const QString borderHex  = cssColor(Colors::Border());            // %3
    const QString selBgHex   = cssColor(Colors::SelectionBg());       // %4
    const QString selFgHex   = cssColor(Colors::SelectionFg());       // %5
    const QString rbBorder   = cssColor(Colors::RubberbandBorder());  // %6
    const QString rbBg       = cssColor(Colors::RubberbandBg());      // %7
    const QString sbBg       = cssColor(Colors::ScrollbarBg());       // %8
    const QString sbHandle   = cssColor(Colors::ScrollbarHandle());   // %9
    const QString sbHover    = cssColor(Colors::ScrollbarHandleHover());// %10
    const QString tabBgHex   = cssColor(Colors::TabBg());             // %11
    const QString tabFgHex   = cssColor(Colors::TabFg());             // %12
    const QString tabActBg   = cssColor(Colors::TabActiveBg());       // %13
    const QString tabActFg   = cssColor(Colors::TabActiveFg());       // %14
    const QString tabHoverBg = cssColor(Colors::TabHoverBg());        // %15
    const QString menuBgHex  = cssColor(Colors::MenuBg());            // %16
    const QString menuFgHex  = cssColor(Colors::MenuFg());            // %17
    const QString menuBrdHex = cssColor(Colors::MenuBorder());        // %18
    const QString menuSelBg  = cssColor(Colors::MenuSelectedBg());    // %19
    const QString menuSelFg  = cssColor(Colors::MenuSelectedFg());    // %20
    const QString menuDisFg  = cssColor(Colors::MenuDisabledFg());    // %21
    const QString inputBgHex = cssColor(Colors::InputBg());           // %22
    const QString inputFgHex = cssColor(Colors::InputFg());           // %23
    const QString inputBrdHex= cssColor(Colors::InputBorder());       // %24
    const QString inputFocus = cssColor(Colors::InputFocusBorder());  // %25
    const QString btnBgHex   = cssColor(Colors::ButtonBg());          // %26
    const QString btnFgHex   = cssColor(Colors::ButtonFg());          // %27
    const QString btnBrdHex  = cssColor(Colors::ButtonBorder());      // %28
    const QString btnHoverBg = cssColor(Colors::ButtonHoverBg());     // %29
    const QString btnHoverFg = cssColor(Colors::ButtonHoverFg());     // %30
    const QString checkBrd   = cssColor(Colors::CheckboxBorder());    // %31
    const QString checkOn    = cssColor(Colors::CheckboxCheckedBg()); // %32

    QString fontFamilyCss;
    {
        QString family = uiConfig().fontFamily.trimmed();
        family.remove(QLatin1Char('"'));
        family.remove(QLatin1Char('%'));
        if (family.isEmpty()) {
            fontFamilyCss = QStringLiteral(
                "\"JetBrainsMono Nerd Font\", \"JetBrainsMono NF\", "
                "\"DejaVu Sans Mono\", monospace");
        } else {
            fontFamilyCss = QStringLiteral("\"%1\", monospace").arg(family);
        }
    }

    return QString(R"css(
        * {
            font-family: %33;
            font-size: %34px;
            color: %1;
            border-radius: 0px;
            outline: none;
        }

        QMainWindow, QWidget#centralWidget, QDialog {
            background-color: %2;
            color: %1;
        }

        QSplitter {
            background-color: %2;
        }

        /* ручка сплиттера: невидимая зона захвата + 1px линия шва (как в tui),
           на hover вся зона подсвечивается, чтобы было видно, что тащишь */
        QSplitter::handle {
            background-color: %2;
        }
        QSplitter::handle:horizontal {
            width: 4px;
            border-left: 1px solid %3;
        }
        QSplitter::handle:horizontal:hover {
            background-color: %3;
            border-left: 1px solid %3;
        }
        QSplitter::handle:vertical {
            height: 4px;
            border-top: 1px solid %3;
        }
        QSplitter::handle:vertical:hover {
            background-color: %3;
            border-top: 1px solid %3;
        }

        /* выделение прямоугольной резинкой */
        QRubberBand {
            border: 1px solid %6;
            background-color: %7;
            border-radius: 0px;
        }

        /* плоский скроллбар */
        QScrollBar:vertical {
            background: %8;
            width: 5px;
            margin: 0px;
            border: none;
        }
        QScrollBar::handle:vertical {
            background: %9;
            min-height: 20px;
            border-radius: 0px;
        }
        QScrollBar::handle:vertical:hover {
            background: %10;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            height: 0px;
            background: transparent;
        }

        QScrollBar::horizontal {
            background: %8;
            height: 5px;
            margin: 0px;
            border: none;
        }
        QScrollBar::handle:horizontal {
            background: %9;
            min-width: 20px;
            border-radius: 0px;
        }
        QScrollBar::handle:horizontal:hover {
            background: %10;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            width: 0px;
            background: transparent;
        }

        /* верхние вкладки в стиле моноширинного TUI */
        QTabBar::tab {
            background: %11;
            color: %12;
            padding: 4px 12px;
            border: none;
            border-right: 1px solid %3;
            margin-right: 0px;
        }
        QTabBar::tab:selected {
            background: %13;
            color: %14;
            border-bottom: 2px solid %14;
        }
        QTabBar::tab:hover:!selected {
            background: %15;
            color: %1;
        }

        /* списки */
        QListView, QListWidget, QTreeView {
            background-color: %2;
            border: none;
            selection-background-color: %4;
            selection-color: %5;
            outline: none;
        }
        QListView::item, QListWidget::item {
            border-radius: 0px;
            padding: 0px;
            margin: 0px;
        }
        QListView::item:selected, QListWidget::item:selected {
            background-color: %4;
            color: %5;
        }

        /* меню */
        QMenu {
            background-color: %16;
            border: 1px solid %18;
            padding: 3px 0px;
        }
        QMenu::item {
            background-color: transparent;
            padding: 5px 18px 5px 12px;
            color: %17;
        }
        QMenu::item:selected {
            background-color: %19;
            color: %20;
        }
        QMenu::item:disabled {
            color: %21;
        }
        QMenu::separator {
            height: 1px;
            background: %18;
            margin: 3px 0px;
        }
        /* никаких галочек ОС в меню: активное состояние показывается фоном
           (invert в меню Sort подсвечен как выделенный элемент, руководство §3.4) */
        QMenu::indicator {
            width: 0px;
            height: 0px;
            border: none;
            background: transparent;
        }

        /* плоские поля ввода */
        QLineEdit, QTextEdit, QPlainTextEdit {
            background-color: %22;
            border: 1px solid %24;
            color: %23;
            selection-background-color: %4;
            selection-color: %5;
            padding: 2px 6px;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
            border: 1px solid %25;
        }

        QPushButton {
            background-color: %26;
            border: 1px solid %28;
            color: %27;
            padding: 3px 8px;
        }
        QPushButton:hover {
            background-color: %29;
            color: %30;
        }

        QCheckBox {
            color: %1;
            background: transparent;
            spacing: 6px;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid %31;
            background-color: %2;
            border-radius: 0px;
        }
        QCheckBox::indicator:hover {
            border: 1px solid %32;
        }
        QCheckBox::indicator:checked {
            background-color: %32;
            border: 1px solid %32;
            border-radius: 0px;
        }

        QLabel {
            color: %1;
            background-color: transparent;
        }
    )css")
    .arg(textHex)      // %1
    .arg(baseHex)      // %2
    .arg(borderHex)    // %3
    .arg(selBgHex)     // %4
    .arg(selFgHex)     // %5
    .arg(rbBorder)     // %6
    .arg(rbBg)         // %7
    .arg(sbBg)         // %8
    .arg(sbHandle)     // %9
    .arg(sbHover)      // %10
    .arg(tabBgHex)     // %11
    .arg(tabFgHex)     // %12
    .arg(tabActBg)     // %13
    .arg(tabActFg)     // %14
    .arg(tabHoverBg)   // %15
    .arg(menuBgHex)    // %16
    .arg(menuFgHex)    // %17
    .arg(menuBrdHex)   // %18
    .arg(menuSelBg)    // %19
    .arg(menuSelFg)    // %20
    .arg(menuDisFg)    // %21
    .arg(inputBgHex)   // %22
    .arg(inputFgHex)   // %23
    .arg(inputBrdHex)  // %24
    .arg(inputFocus)   // %25
    .arg(btnBgHex)     // %26
    .arg(btnFgHex)     // %27
    .arg(btnBrdHex)    // %28
    .arg(btnHoverBg)   // %29
    .arg(btnHoverFg)   // %30
    .arg(checkBrd)     // %31
    .arg(checkOn)      // %32
    .arg(fontFamilyCss) // %33
    .arg(uiConfig().fontSize); // %34
}

}
