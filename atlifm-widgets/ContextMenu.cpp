#include "TuiInputPopup.hpp"
#include "ContextMenu.hpp"
#include "NativePlugins.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <algorithm>

namespace Widgets {

// подменю plugins, сторонние плагины из ~/.config/atlifm/plugins/*.conf (core::plugins), пусто значит подменю нет
void TuiContextMenu::addPluginsSubmenu() {
    const auto plugins = Core::Plugins::load();
    if (plugins.empty()) return;

    auto* pluginsMenu = addMenu(QString::fromUtf8("\U000F06EB Plugins"));
    pluginsMenu->setObjectName(QStringLiteral("pluginsMenu"));
    pluginsMenu->setFont(font());
    pluginsMenu->setStyleSheet(styleSheet());

    for (const auto& p : plugins) {
        auto* act = pluginsMenu->addAction(p.name);
        QString tip = p.description;
        if (!p.rejected.isEmpty()) {
            if (!tip.isEmpty()) tip += QStringLiteral("\n\n");
            tip += QStringLiteral("Ignored (sudo surface is protected): ") +
                   p.rejected.join(QStringLiteral("; "));
        }
        if (!tip.isEmpty()) act->setToolTip(tip);

        // пустое выделение значит плагин применяется к текущему каталогу, иначе ко всем выделенным
        const bool applicable = p.native
            ? NativePluginHost::instance().isActionEnabled(p.action, m_selectedPaths, m_currentDir)
            : (m_selectedPaths.empty()
                   ? Core::Plugins::matches(p, m_currentDir, true)
                   : Core::Plugins::appliesToSelection(p, m_selectedPaths));
        act->setEnabled(applicable);
        if (!applicable) continue;

        if (p.native) {
            const QString actionId = p.action;
            connect(act, &QAction::triggered, this, [this, actionId]() {
                emit nativeActionRequested(actionId);
            });
        } else {
            const QString id = p.id;
            connect(act, &QAction::triggered, this, [this, id]() {
                emit pluginRequested(id);
            });
        }
    }
}

TuiContextMenu::TuiContextMenu(const std::vector<std::filesystem::path>& selectedPaths,
                               const std::filesystem::path& currentDir,
                               QWidget* parent)
    : QMenu(parent), m_selectedPaths(selectedPaths), m_currentDir(currentDir) {
    setFont(Theme::getMonospaceFont(9));
    setStyleSheet(Theme::getApplicationStyleSheet());
    populateMenu();
}

void TuiContextMenu::hideEvent(QHideEvent* event) {
    if (parentWidget()) {
        parentWidget()->setFocus(Qt::MouseFocusReason);
    }
    QMenu::hideEvent(event);
}

void TuiContextMenu::populateMenu() {
    bool hasSelection = !m_selectedPaths.empty();
    bool inTrash = (m_currentDir.string().find(".local/share/Trash") != std::string::npos);

    const QClipboard* clipboard = QApplication::clipboard();
    const QMimeData* mimeData = clipboard->mimeData();
    bool canPaste = mimeData && Dnd::DragDropHandler::canAcceptDrop(mimeData);

    if (hasSelection) {
        const auto& targetPath = m_selectedPaths.front();

        auto actOpen = addAction("\U000F0214 Open\tEnter");
        actOpen->setShortcut(QKeySequence(Qt::Key_Return));
        connect(actOpen, &QAction::triggered, this, &TuiContextMenu::openRequested);

        auto actOpenWith = addAction("\U000F02FD Open With...");
        connect(actOpenWith, &QAction::triggered, this, &TuiContextMenu::openWithRequested);

        auto actOpenWithCustom = addAction("\U000F02FD Open With Custom Command...");
        connect(actOpenWithCustom, &QAction::triggered, this, [this]() {
            bool ok = false;
            QString cmd = TuiInputPopup::getText(this, "Open With Custom Command",
                                                 "Command line executable:",
                                                 "", &ok);
            if (ok && !cmd.trimmed().isEmpty()) {
                emit openWithAppRequested(cmd.trimmed());
            }
        });

        bool hasDirectory = false;
        for (const auto& path : m_selectedPaths) {
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec)) {
                hasDirectory = true;
                break;
            }
        }
        auto actOpenTab = addAction("\U000F04E6 Open in New Tab\tCtrl+Return");
        actOpenTab->setShortcut(QKeySequence("Ctrl+Return"));
        actOpenTab->setEnabled(hasDirectory);
        connect(actOpenTab, &QAction::triggered, this, &TuiContextMenu::openInNewTabRequested);

        addSeparator();

        auto actCut = addAction("\U000F0190 Cut\tCtrl+X");
        actCut->setShortcut(QKeySequence::Cut);
        connect(actCut, &QAction::triggered, this, &TuiContextMenu::cutRequested);

        auto actCopy = addAction("\U000F018F Copy\tCtrl+C");
        actCopy->setShortcut(QKeySequence::Copy);
        connect(actCopy, &QAction::triggered, this, &TuiContextMenu::copyRequested);

        auto actPaste = addAction("\U000F0192 Paste\tCtrl+V");
        actPaste->setShortcut(QKeySequence::Paste);
        actPaste->setEnabled(canPaste);
        connect(actPaste, &QAction::triggered, this, &TuiContextMenu::pasteRequested);

        auto copyPathMenu = addMenu("\U000F0337 Copy Path");
        copyPathMenu->setFont(font());
        copyPathMenu->setStyleSheet(styleSheet());

        auto actAbs = copyPathMenu->addAction("Full Absolute Path\tCtrl+Shift+C");
        actAbs->setShortcut(QKeySequence("Ctrl+Shift+C"));
        connect(actAbs, &QAction::triggered, [this]() { emit copyPathRequested(0); });

        auto actName = copyPathMenu->addAction("File Name");
        connect(actName, &QAction::triggered, [this]() { emit copyPathRequested(1); });

        auto actRel = copyPathMenu->addAction("Relative Path\tCtrl+Alt+C");
        connect(actRel, &QAction::triggered, [this]() { emit copyPathRequested(2); });

        addSeparator();

        auto actRename = addAction("\U000F0455 Rename\tF2");
        actRename->setShortcut(QKeySequence(Qt::Key_F2));
        connect(actRename, &QAction::triggered, this, &TuiContextMenu::renameRequested);

        bool isInsideTrash = inTrash || (targetPath.string().find(".local/share/Trash") != std::string::npos);
        if (isInsideTrash) {
            auto actDelPerm = addAction("\U000F01B4 Delete Permanently\tDel");
            connect(actDelPerm, &QAction::triggered, this, &TuiContextMenu::deletePermanentlyRequested);

            auto actClearTrash = addAction("\U000F0A7A Clear Bin");
            connect(actClearTrash, &QAction::triggered, this, &TuiContextMenu::clearTrashRequested);
        } else {
            auto actTrash = addAction("\U000F0A79 Move to Trash\tDel");
            connect(actTrash, &QAction::triggered, this, &TuiContextMenu::trashRequested);

            auto actDelPerm = addAction("\U000F01B4 Delete Permanently\tShift+Del");
            connect(actDelPerm, &QAction::triggered, this, &TuiContextMenu::deletePermanentlyRequested);
        }

        addSeparator();

        auto actBm = addAction("\U000F00BA Add to Bookmarks");
        connect(actBm, &QAction::triggered, this, &TuiContextMenu::bookmarkRequested);

        // 10 самых популярных форматов архивов
        auto compressMenu = addMenu("\U000F01B7 Compress");
        compressMenu->setFont(font());
        compressMenu->setStyleSheet(styleSheet());

        struct CompressFormat {
            QString label;
            QString ext;
        };
        const std::vector<CompressFormat> formats = {
            {"ZIP (.zip)", ".zip"},
            {"Gzip Tarball (.tar.gz)", ".tar.gz"},
            {"XZ Tarball (.tar.xz)", ".tar.xz"},
            {"Bzip2 Tarball (.tar.bz2)", ".tar.bz2"},
            {"Zstandard Tarball (.tar.zst)", ".tar.zst"},
            {"7-Zip (.7z)", ".7z"},
            {"TAR archive (.tar)", ".tar"},
            {"GZIP (.gz)", ".gz"},
            {"XZ (.xz)", ".xz"},
            {"BZIP2 (.bz2)", ".bz2"}
        };

        for (const auto& fmt : formats) {
            auto act = compressMenu->addAction(fmt.label);
            connect(act, &QAction::triggered, this, [this, ext = fmt.ext]() {
                emit compressRequested(ext);
            });
        }

        // extract если выбранный элемент архив, extract in это выбор каталога через FileChooserDialog,
        // extract here сразу в текущую папку, формат проверяет XdgUtils::isArchive
        if (!m_selectedPaths.empty() && Core::XdgUtils::isArchive(m_selectedPaths.front())) {
            auto extractMenu = addMenu(QString::fromUtf8("\U000F06EB Extract"));
            extractMenu->setFont(font());
            extractMenu->setStyleSheet(styleSheet());

            auto actExtractIn = extractMenu->addAction("Extract In...");
            connect(actExtractIn, &QAction::triggered, this, &TuiContextMenu::extractInRequested);

            auto actExtractHere = extractMenu->addAction("Extract Here\tCtrl+Alt+X");
            actExtractHere->setShortcut(QKeySequence("Ctrl+Alt+X"));
            connect(actExtractHere, &QAction::triggered, this, &TuiContextMenu::extractHereRequested);
        }

        // plugins, сторонние плагины
        addPluginsSubmenu();

        addSeparator();

        auto sortMenu = addMenu(QString::fromUtf8("\xEF\x82\xB0 Sort By"));
        sortMenu->setFont(font());
        sortMenu->setStyleSheet(styleSheet());

        auto actSortName = sortMenu->addAction("Name\tAlt+1");
        actSortName->setShortcut(QKeySequence("Alt+1"));
        connect(actSortName, &QAction::triggered, [this]() { emit sortModeRequested(0); });

        auto actSortSize = sortMenu->addAction("Size\tAlt+2");
        actSortSize->setShortcut(QKeySequence("Alt+2"));
        connect(actSortSize, &QAction::triggered, [this]() { emit sortModeRequested(1); });

        auto actSortDate = sortMenu->addAction("Date Modified\tAlt+3");
        actSortDate->setShortcut(QKeySequence("Alt+3"));
        connect(actSortDate, &QAction::triggered, [this]() { emit sortModeRequested(2); });

        auto actSortType = sortMenu->addAction("Type\tAlt+4");
        actSortType->setShortcut(QKeySequence("Alt+4"));
        connect(actSortType, &QAction::triggered, [this]() { emit sortModeRequested(3); });

        addSeparator();

        auto actProps = addAction("\U000F02FD Properties\tAlt+Enter");
        actProps->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
        connect(actProps, &QAction::triggered, this, &TuiContextMenu::propertiesRequested);

    } else {
        // меню по пустому месту и фону каталога
        auto actNewFolder = addAction("\U000F024B New Folder\tCtrl+Shift+N");
        actNewFolder->setShortcut(QKeySequence("Ctrl+Shift+N"));
        connect(actNewFolder, &QAction::triggered, this, &TuiContextMenu::newFolderRequested);

        auto actNewFile = addAction("\U000F0214 New File\tCtrl+Alt+N");
        actNewFile->setShortcut(QKeySequence("Ctrl+Alt+N"));
        connect(actNewFile, &QAction::triggered, this, &TuiContextMenu::newFileRequested);

        auto actOpenTab = addAction("\U000F04E6 Open Current Folder in New Tab\tCtrl+Return");
        actOpenTab->setShortcut(QKeySequence("Ctrl+Return"));
        connect(actOpenTab, &QAction::triggered, this, &TuiContextMenu::openInNewTabRequested);

        addSeparator();

        auto actPaste = addAction("\U000F0192 Paste\tCtrl+V");
        actPaste->setShortcut(QKeySequence::Paste);
        actPaste->setEnabled(canPaste);
        connect(actPaste, &QAction::triggered, this, &TuiContextMenu::pasteRequested);

        addSeparator();

        auto actTerminal = addAction("\U000F0A36 Open Terminal Here\tF4");
        connect(actTerminal, &QAction::triggered, this, &TuiContextMenu::openTerminalRequested);

        if (inTrash) {
            auto actClearTrash = addAction("\U000F0A7A Clear Bin");
            connect(actClearTrash, &QAction::triggered, this, &TuiContextMenu::clearTrashRequested);
        }

        auto actBm = addAction("\U000F00BA Bookmark This Folder");
        connect(actBm, &QAction::triggered, this, &TuiContextMenu::bookmarkRequested);

        // plugins, для фона применяются к текущему каталогу
        addPluginsSubmenu();

        addSeparator();

        auto sortMenu = addMenu(QString::fromUtf8("\xEF\x82\xB0 Sort By"));
        sortMenu->setFont(font());
        sortMenu->setStyleSheet(styleSheet());

        auto actSortName = sortMenu->addAction("Name\tAlt+1");
        actSortName->setShortcut(QKeySequence("Alt+1"));
        connect(actSortName, &QAction::triggered, [this]() { emit sortModeRequested(0); });

        auto actSortSize = sortMenu->addAction("Size\tAlt+2");
        actSortSize->setShortcut(QKeySequence("Alt+2"));
        connect(actSortSize, &QAction::triggered, [this]() { emit sortModeRequested(1); });

        auto actSortDate = sortMenu->addAction("Date Modified\tAlt+3");
        actSortDate->setShortcut(QKeySequence("Alt+3"));
        connect(actSortDate, &QAction::triggered, [this]() { emit sortModeRequested(2); });

        auto actSortType = sortMenu->addAction("Type\tAlt+4");
        actSortType->setShortcut(QKeySequence("Alt+4"));
        connect(actSortType, &QAction::triggered, [this]() { emit sortModeRequested(3); });

        addSeparator();

        auto actProps = addAction("\U000F02FD Properties\tAlt+Enter");
        actProps->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
        connect(actProps, &QAction::triggered, this, &TuiContextMenu::propertiesRequested);
    }
}

}
