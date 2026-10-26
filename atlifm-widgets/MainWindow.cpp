#include "MainWindow.hpp"
#include "NativePlugins.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/Bookmarks.hpp"
#include "../atlifm-core/Keybinds.hpp"
#include "../atlifm-core/Plugins.hpp"
#include "../atlifm-core/UndoManager.hpp"
#include "../atlifm-dnd/DragDropHandler.hpp"
#include "PropertiesDialog.hpp"
#include "OpenWithDialog.hpp"
#include "FileChooserDialog.hpp"
#include "ConnectionDialog.hpp"
#include "../atlifm-core/NetworkMount.hpp"
#include "HelpDialog.hpp"
#include "TuiOutputOverlay.hpp"
#include "TasksOverlay.hpp"
#include "BulkRenamePanel.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"
#include <QVBoxLayout>
#include <QShortcut>
#include <QKeySequence>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QFile>
#include <QTimer>
#include <QMenu>
#include <QClipboard>
#include <QGuiApplication>
#include <QApplication>
#include <QIcon>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QStyleHints>
#include <QPointer>
#include <QHash>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

namespace {

// в тайлинговом wm размер окна задаёт композитор. если приложение вызывает
// resize() уже после show (сброс layout), hyprland/sway могут оставить окно
// отдельным прямоугольником вместо тайла а на плавающих wm сброс по прежнему
// возвращает размер из theme.conf.
bool compositorManagesWindowSize() {
    const QString platform = QGuiApplication::platformName();
    if (platform.isEmpty() || platform == QLatin1String("offscreen") ||
        platform == QLatin1String("minimal")) {
        return false;
    }
    return qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE") ||
           qEnvironmentVariableIsSet("SWAYSOCK") ||
           qEnvironmentVariableIsSet("I3SOCK") ||
           qEnvironmentVariableIsSet("NIRI_SOCKET") ||
           qEnvironmentVariableIsSet("RIVER_SOCKET");
}

} // namespace

namespace Widgets {

MainWindow::MainWindow(const std::filesystem::path& initialPath, QWidget* parent)
    : QMainWindow(parent) {
    // плагины c++ (~/.config/atlifm/plugins/*.so) загружаются до первого обращения
    // к keybinds/меню: их действия участвуют в биндингах, подменю plugins, override/hooks
    NativePluginHost::instance().setWindow(this);
    NativePluginHost::instance().initialize();
    Core::Plugins::setNativeProvider([]() {
        return Widgets::NativePluginHost::instance().manifests();
    });

    setWindowTitle("atlifm");
    // это ещё до show(): тайлинговый wm всё равно выдаст свой тайл, а на
    // плавающем wm окно сразу откроется нормального размера.
    const auto& ui = Theme::uiConfig();
    resize(ui.windowWidth, ui.windowHeight);
    // иконка вшита в ресурсы а бары вяленого сопоставляют ее по app id,
    // x11 напрямую; ставим на уровне окна для любого способа запуска
    setWindowIcon(QIcon(QStringLiteral(":/atlifm.png")));

    applySystemTheme();

    setupUi();
    setupShortcuts();

    // ловим боковые кнопки мыши (назад/вперёд) в любом месте окна
    qApp->installEventFilter(this);

    // следим за сменой системной тёмной/светлой темы
    if (auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme /*scheme*/) {
            applySystemTheme();
        });
    }

    std::filesystem::path startPath = initialPath;
    std::filesystem::path selectFile;
    if (!startPath.empty()) {
        std::error_code ec;
        // open in: передан файл (не папка), открываем его каталог, файл
        // выделяем и показываем в превью после загрузки списка
        if (std::filesystem::is_regular_file(startPath, ec)) {
            selectFile = startPath;
            startPath = startPath.parent_path();
        }
    }
    if (startPath.empty()) {
        const char* home = std::getenv("HOME");
        startPath = home ? home : "/";
    }

    m_tabBar->addTab(startPath);
    m_millerView->setDirectory(startPath);
    if (!selectFile.empty()) {
        // каталог грузится асинхронно, выбираем с ретраями после refresh
        m_millerView->selectPathAfterRefresh(selectFile);
    }
    if (m_bookmarksBar) {
        m_bookmarksBar->setActiveDirectory(startPath);
    }
    m_statusBar->setPath(startPath);
    m_lastTabIndex = 0;
    NativePluginHost::fireHook(QStringLiteral("on_start"), {}, QString::fromUtf8(startPath.c_str()));
}

MainWindow::~MainWindow() {
    // при уничтожении дочерних виджетов qt меняет фокус и шлёт focuschanged;
    // слот успевал обратиться к уже удалённым qshortcut/оверлеям. отключаем
    // глобальные сигналы и фильтр ещё до разрушения дерева.
    if (qApp) {
        qApp->removeEventFilter(this);
        disconnect(qApp, nullptr, this, nullptr);
    }
    m_keybindShortcuts.clear();
    m_textSensitiveShortcuts.clear();
    m_contextSensitiveShortcuts.clear();

    // сначала выгружаем плагины, а потом задачи. плагин при выгрузке может
    // успеть создать задачу, и она не должна пережить qapplication.
    NativePluginHost::instance().setWindow(nullptr);
    NativePluginHost::instance().shutdownAll();
    Core::Tasks::TaskManager::instance().waitForIdle(5000);
}

void MainWindow::applySystemTheme() {
    Theme::reloadPalette();
    setStyleSheet(Theme::getApplicationStyleSheet());
    if (m_statusBar) m_statusBar->updateTheme();
    if (m_millerView) m_millerView->refresh();
    if (m_bookmarksBar) m_bookmarksBar->refresh();
    if (m_openWithPage) m_openWithPage->updateTheme();
    if (m_fileChooserPage) m_fileChooserPage->updateTheme();
    update();
}

void MainWindow::setupUi() {
    auto central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 1. верхняя плоская панель вкладок
    m_tabBar = new EngineTabBar(this);
    m_tabBar->setFixedHeight(EngineTabBar::kNormalHeight);
    rootLayout->addWidget(m_tabBar, 0);

    // 2. средний ряд: разделитель закладок (110..220) + колонки miller;
    // левую панель можно сжать, при нехватке места она сжимается сама (см. resizeevent)
    m_middleSplitter = new QSplitter(Qt::Horizontal, this);
    m_middleSplitter->setObjectName(QStringLiteral("middleSplitter"));
    m_middleSplitter->setHandleWidth(2);
    m_middleSplitter->setChildrenCollapsible(false);
    m_middleSplitter->setOpaqueResize(true);

    // левая панель закладок (доступна всегда, постоянная)
    m_bookmarksBar = new BookmarksBar(m_middleSplitter);
    m_bookmarksBar->show(); // доступна всегда
    m_bookmarksBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    // минимумы адаптивные (см. resizeevent): в узком окне (<400px) снижаются до 48/140,
    // иначе сумма минимумов (302px) превышала ширину окна и рукоятка не двигалась
    m_bookmarksBar->setMinimumWidth(100);
    m_bookmarksBar->setMaximumWidth(std::max(380, preferredSidebarWidth()));

    // движок miller из трёх колонок
    m_millerView = new MillerViewContainer(m_middleSplitter);
    // минимум адаптивный (resizeevent): в узком окне снижается до 140, чтобы
    // рукоятка bookmarks сохраняла реальный ход
    m_millerView->setMinimumWidth(200);

    m_middleSplitter->addWidget(m_bookmarksBar);
    m_middleSplitter->addWidget(m_millerView);
    m_middleSplitter->setStretchFactor(0, 0);
    m_middleSplitter->setStretchFactor(1, 1);
    m_middleSplitter->setSizes({preferredSidebarWidth(), std::max(200, width() - preferredSidebarWidth())});

    rootLayout->addWidget(m_middleSplitter, 1);

    connect(m_millerView, &MillerViewContainer::sudoModeChanged, this, [this](bool isSudo) {
        if (m_tabBar && m_tabBar->currentIndex() >= 0 && m_tabBar->currentIndex() < m_tabBar->count()) {
            m_tabBar->tabState(m_tabBar->currentIndex()).isSudo = isSudo;
        }
        if (m_statusBar) {
            m_statusBar->setSudoBadge(isSudo);
        }
    });

    // 3. нижняя моноширинная строка статуса и навигации
    m_statusBar = new TuiStatusBar(this);
    rootLayout->addWidget(m_statusBar, 0);

    // раскладка из theme.conf: бары/сайдбар/крайние колонки можно скрыть,
    // а не только поменять ширину сайдбара
    const auto& layoutUi = Theme::uiConfig();
    m_tabBar->setVisible(layoutUi.showTabBar);
    m_bookmarksBar->setVisible(layoutUi.showBookmarks);
    m_bookmarksEnabled = layoutUi.showBookmarks;
    m_statusBar->setVisible(layoutUi.showStatusBar);
    m_millerView->setColumnsConfig(layoutUi.showParentColumn, layoutUi.showPreview,
                                   layoutUi.parentColumnWidth, layoutUi.previewColumnWidth);

    // свойства (пкм и properties, alt+enter, закладки) оверлей поверх всего окна:
    // «как внешнее окно», но часть одного окна; для корня диска/фс внутри
    // автоматически раскрывается консоль disk usage
    connect(m_millerView, &MillerViewContainer::propertiesRequested, this, &MainWindow::showPropertiesOverlay);

    // open with… полноразмерный оверлей диалога (не подменю и не окно)
    connect(m_millerView, &MillerViewContainer::openWithDialogRequested, this, &MainWindow::showOpenWithOverlay);
    // extract in…: меню просит выбор каталога, открываем тот же оверлей
    connect(m_millerView, &MillerViewContainer::extractInRequested, this, &MainWindow::showExtractChooserOverlay);
    connect(m_millerView, &MillerViewContainer::extractInPathRequested, this,
            &MainWindow::showExtractChooserOverlayFor);
    connect(m_millerView, &MillerViewContainer::bulkRenameRequested, this,
            &MainWindow::showBulkRenameOverlay);

    // фоновые задачи: агрегированный индикатор в статусбаре
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::added,
            this, [this](const Core::Tasks::TaskInfo&) { updateTaskSummary(); });
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::updated,
            this, [this](const Core::Tasks::TaskInfo&) { updateTaskSummary(); });
    connect(&Core::Tasks::TaskManager::instance(), &Core::Tasks::TaskManager::finished,
            this, [this](const Core::Tasks::TaskInfo&) { updateTaskSummary(); });

    // подключаем события панели вкладок
    connect(m_tabBar, &EngineTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabBar, &EngineTabBar::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    connect(m_tabBar, &EngineTabBar::newTabRequested, this, &MainWindow::onNewTabRequested);
    connect(m_tabBar, &EngineTabBar::duplicateTabRequested, this, &MainWindow::onDuplicateTabRequested);
    connect(m_tabBar, &EngineTabBar::pinTabRequested, this, &MainWindow::onPinTabRequested);
    connect(m_tabBar, &EngineTabBar::closeOtherTabsRequested, this, &MainWindow::onCloseOtherTabsRequested);
    connect(m_tabBar, &EngineTabBar::filesDroppedOnTab, this, [this](int tabIndex, const auto& srcs, auto act, auto mods) {
        if (tabIndex >= 0 && tabIndex < m_tabBar->count()) {
            auto targetDir = m_tabBar->tabState(tabIndex).currentPath;
            Dnd::DragDropHandler::handleDrop(srcs, targetDir, act, mods, m_millerView);
        }
    });

    // подключаем события панели закладок
    connect(m_bookmarksBar, &BookmarksBar::navigateRequested, this, [this](const std::filesystem::path& p) {
        m_millerView->setDirectory(p);
    });
    connect(m_bookmarksBar, &BookmarksBar::openInNewTabRequested, this, [this](const std::filesystem::path& p) {
        m_tabBar->addTab(p);
    });
    connect(m_millerView, &MillerViewContainer::openInNewTabRequested, this,
            [this](const std::filesystem::path& p) {
                if (m_millerView &&
                    m_millerView->tryPluginOverrideWith(QStringLiteral("open_in_new_tab"),
                                                        {p}, p.parent_path())) {
                    return;
                }
                if (m_tabBar) m_tabBar->addTab(p);
            });

    connect(m_bookmarksBar, &BookmarksBar::filesDropped, this, [this](const auto& srcs, const auto& dest, auto act, auto mods) {
        QPointer<MainWindow> guard(this);
        Dnd::DragDropHandler::handleDrop(srcs, dest, act, mods, m_millerView, [guard]() {
            if (!guard) return;
            if (guard->m_millerView) guard->m_millerView->refresh();
            if (guard->m_bookmarksBar) guard->m_bookmarksBar->refresh();
        });
    });

    connect(m_millerView, &MillerViewContainer::bookmarkAdded, this, [this](const std::filesystem::path& p) {
        m_bookmarksBar->addBookmark(p);
    });
    // пункты меню закладок проходят через диспетчер оверлеев как одноименные
    // действия miller (copy_path, remove_bookmark, clear_trash, properties)
    connect(m_bookmarksBar, &BookmarksBar::copyPathRequested, this, [this](const std::filesystem::path& p) {
        if (m_millerView->tryPluginOverrideWith(QStringLiteral("copy_path"), {p}, p)) return;
        QApplication::clipboard()->setText(QString::fromUtf8(p.c_str()));
    });
    connect(m_bookmarksBar, &BookmarksBar::removeBookmarkRequested, this, [this](const std::filesystem::path& p) {
        if (m_millerView->tryPluginOverrideWith(QStringLiteral("remove_bookmark"), {p}, p)) return;
        Core::BookmarksManager::removeBookmark(p);
        m_bookmarksBar->refresh();
    });
    connect(m_bookmarksBar, &BookmarksBar::trashClearRequested, this, [this]() {
        if (m_millerView->tryPluginOverride(QStringLiteral("clear_trash"))) return;
        Core::XdgUtils::clearTrash();
        m_bookmarksBar->refresh();
        m_millerView->refresh();
    });
    // свойства из закладок и дисков тот же оверлей, что и пкм и properties
    connect(m_bookmarksBar, &BookmarksBar::propertiesRequested, this, &MainWindow::showPropertiesOverlay);
    connect(m_millerView, &MillerViewContainer::itemSelected, this, [this](const std::filesystem::path& p) {
        if (!m_isSwitchingTab && m_tabBar && m_tabBar->currentIndex() >= 0 && m_tabBar->currentIndex() < m_tabBar->count()) {
            m_tabBar->currentTabState().selectedItem = p;
        }
    });

    // подключения miller view
    connect(m_millerView, &MillerViewContainer::directoryChanged, this, &MainWindow::onDirectoryChanged);
    connect(m_millerView, &MillerViewContainer::archiveEntered, this, [this](const std::filesystem::path& archive) {
        // путь вкладки остаётся каталогом; в заголовке окна и вкладки показываем архив
        const QString archiveQ = QString::fromUtf8(archive.c_str());
        const QString name = QString::fromUtf8(archive.filename().c_str());
        if (m_tabBar->count() > 0 && m_tabBar->currentIndex() >= 0) {
            m_tabBar->updateTabTitle(m_tabBar->currentIndex(), name);
        }
        setWindowTitle(QStringLiteral("atlifm: %1").arg(archiveQ));
    });
    connect(m_millerView, &MillerViewContainer::archiveLeft, this, [this](const std::filesystem::path& dir) {
        const QString dirQ = QString::fromUtf8(dir.c_str());
        const QString name = QString::fromUtf8(dir.filename().c_str());
        if (m_tabBar->count() > 0 && m_tabBar->currentIndex() >= 0) {
            m_tabBar->updateTabTitle(m_tabBar->currentIndex(), name.isEmpty() ? dirQ : name);
        }
        if (m_statusBar) m_statusBar->setPath(dir);
        setWindowTitle(QStringLiteral("atlifm: %1").arg(dirQ));
    });
    connect(m_millerView, &MillerViewContainer::statusUpdated, this, &MainWindow::onStatusUpdated);
    connect(m_millerView, &MillerViewContainer::messageEmitted, this, [this](const QString& msg) {
        m_statusBar->setProgressMessage(msg);
    });
    // плагины c++: notify() и refresh() из host api
    connect(&NativePluginHost::instance(), &NativePluginHost::messageEmitted, this,
            [this](const QString& msg) { m_statusBar->setProgressMessage(msg); });
    connect(&NativePluginHost::instance(), &NativePluginHost::refreshRequested, this, [this]() {
        if (m_millerView) m_millerView->refresh();
        if (m_bookmarksBar) m_bookmarksBar->refresh();
    });
    connect(m_millerView, &MillerViewContainer::typeAheadSearchTriggered, this, [this](const QString& text) {
        m_statusBar->activateFilterWithText(text);
    });
    connect(m_millerView, &MillerViewContainer::filterTriggered, this, [this]() {
        m_statusBar->activateFilter();
    });
    // аппаратные боковые кнопки мыши из miller view (override как у go_back/go_forward)
    connect(m_millerView, &MillerViewContainer::navigateHistoryBackRequested, this, [this]() {
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        if (m_millerView->tryPluginOverride(QStringLiteral("go_back"))) return;
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoBack()) {
            auto exited = tab.currentPath;
            tab.goBack();
            m_millerView->setDirectory(tab.currentPath, exited);
        }
    });
    connect(m_millerView, &MillerViewContainer::navigateHistoryForwardRequested, this, [this]() {
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        if (m_millerView->tryPluginOverride(QStringLiteral("go_forward"))) return;
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoForward()) {
            tab.goForward();
            m_millerView->setDirectory(tab.currentPath);
        }
    });

    // подключения строки статуса
    connect(m_statusBar, &TuiStatusBar::pathEntered, this, [this](const std::filesystem::path& p) {
        m_millerView->setDirectory(p);
    });
    connect(m_statusBar, &TuiStatusBar::networkLocationEntered, this,
            &MainWindow::showConnectionOverlay);
    connect(m_statusBar, &TuiStatusBar::filterChanged, this, &MainWindow::onFilterChanged);
    connect(m_statusBar, &TuiStatusBar::filterCommandChanged, this,
            &MainWindow::onFilterCommandChanged);
    connect(m_statusBar, &TuiStatusBar::filterSubmitted, this, &MainWindow::onFilterSubmitted);
    connect(m_statusBar, &TuiStatusBar::filterDismissed, this, [this]() {
        if (m_millerView) m_millerView->cancelPluginPreview(true);
        if (m_filterSearchTimer) m_filterSearchTimer->stop();
        if (m_keepGlobalResultsOnDismiss) {
            m_keepGlobalResultsOnDismiss = false;
            m_millerView->focusCenter();
            return;
        }
        m_millerView->clearGlobalSearch();
        m_millerView->setFilter("");
    });

    m_filterSearchTimer = new QTimer(this);
    m_filterSearchTimer->setSingleShot(true);
    m_filterSearchTimer->setInterval(220);
    connect(m_filterSearchTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSearchToken == QLatin1String("fd")) {
            m_millerView->searchEverywhere(m_pendingSearchQuery);
        } else if (m_pendingSearchToken == QLatin1String("rg")) {
            m_millerView->searchContents(m_pendingSearchQuery);
        }
    });

    refreshFilterCommands();
    loadSavedLayout();
}

// оверлей свойств: «как внешнее окно», но часть этого же окна, плавающая панель
// с рамкой accent по центру, перекрывает только часть колонок (как окно свойств
// nautilus); без затемнений и анимаций, radius 0. внутри propertiesdialog во
// встроенном режиме; для корня диска/фс он сам раскрывает консоль disk usage
// (единственный путь для пкм и properties, alt+enter и закладок)
void MainWindow::setupPropertiesOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_propsOverlay = new QWidget(central);
    m_propsOverlay->setObjectName(QStringLiteral("propsOverlay"));
    m_propsOverlay->setAttribute(Qt::WA_StyledBackground, true);
    // обводка в несколько пикселей, чтобы оверлей не сливался с окнами менеджера:
    // 2px рамка accent и такие же отступы лэйаута, иначе содержимое перекрывает рамку
    m_propsOverlay->setStyleSheet(QString(
        "QWidget#propsOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    auto* overlayLayout = new QVBoxLayout(m_propsOverlay);
    overlayLayout->setContentsMargins(2, 2, 2, 2);
    overlayLayout->setSpacing(0);

    m_propsPage = new PropertiesDialog(std::filesystem::path(), m_propsOverlay);
    m_propsPage->setWindowFlags(Qt::Widget);
    m_propsPage->setEmbedded(true);
    overlayLayout->addWidget(m_propsPage);

    // close/esc внутри диалога: скрыть панель, фокус назад списку файлов
    connect(m_propsPage, &PropertiesDialog::closeRequested, this, [this]() {
        if (m_propsOverlay) m_propsOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
    });

    m_propsOverlay->hide();
    positionPropertiesOverlay();
}

void MainWindow::positionPropertiesOverlay() {
    if (!m_propsOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    // по центру; для корня диска propertiesdialog сам раскрывает консоль disk usage,
    // панель большая (~62%×82% окна, клампы 560..840 × 520..760), колонки видны по краям;
    // компактные свойства панель по высоте содержимого
    const int cw = central->width();
    const int ch = central->height();
    constexpr int kMargin = 36;
    const int availW = std::max(320, cw - kMargin * 2);
    const int availH = std::max(260, ch - kMargin * 2);

    int w = 0;
    int h = 0;
    if (m_propsPage && m_propsPage->diskUsageVisible()) {
        w = std::min(availW, std::clamp(cw * 62 / 100, 560, 840));
        h = std::min(availH, std::clamp(ch * 82 / 100, 520, 760));
    } else {
        w = std::min(availW, 560);
        const int hint = m_propsPage ? m_propsPage->sizeHint().height() : 420;
        h = std::min(availH, std::max(320, hint + 4));
    }
    m_propsOverlay->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::showPropertiesOverlay(const std::filesystem::path& path) {
    // вызов из закладок или списка дисков не проходит через miller, ещё одна точка override диспетчера
    if (!path.empty() && m_millerView->tryPluginOverrideWith(QStringLiteral("properties"), {path}, path))
        return;
    if (!m_propsOverlay || !m_propsPage) setupPropertiesOverlay();
    if (!m_propsOverlay || !m_propsPage || path.empty()) return;
    // порядок важен: setpath пересчитывает режим консоли disk usage (корень диска/фс),
    // positionpropertiesoverlay знает итоговую геометрию
    m_propsPage->setPath(path);
    positionPropertiesOverlay();
    m_propsOverlay->show();
    m_propsOverlay->raise();
    m_propsPage->setFocus();
}

// open with: та же механика оверлея, что и у свойств, содержимое другой диалог (поиск, default app / other apps, always use)
void MainWindow::setupOpenWithOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_openWithOverlay = new QWidget(central);
    m_openWithOverlay->setObjectName(QStringLiteral("openWithOverlay"));
    m_openWithOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_openWithOverlay->setStyleSheet(QString(
        "QWidget#openWithOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    auto* overlayLayout = new QVBoxLayout(m_openWithOverlay);
    overlayLayout->setContentsMargins(2, 2, 2, 2);
    overlayLayout->setSpacing(0);

    m_openWithPage = new OpenWithDialog(m_openWithOverlay);
    overlayLayout->addWidget(m_openWithPage);

    connect(m_openWithPage, &OpenWithDialog::closeRequested, this, [this]() {
        if (m_openWithOverlay) m_openWithOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
    });

    m_openWithOverlay->hide();
    positionOpenWithOverlay();
}

void MainWindow::positionOpenWithOverlay() {
    if (!m_openWithOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    // компактнее свойств: список приложений + поиск, по центру окна
    const int cw = central->width();
    const int ch = central->height();
    constexpr int kMargin = 36;
    const int availW = std::max(320, cw - kMargin * 2);
    const int availH = std::max(260, ch - kMargin * 2);

    const int w = std::min(availW, 520);
    const int h = std::min(availH, std::max(380, std::min(560, ch * 72 / 100)));
    m_openWithOverlay->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::showOpenWithOverlay(const std::filesystem::path& path) {
    if (!m_openWithOverlay || !m_openWithPage) setupOpenWithOverlay();
    if (!m_openWithOverlay || !m_openWithPage || path.empty()) return;
    m_openWithPage->setFile(path);
    positionOpenWithOverlay();
    m_openWithOverlay->show();
    m_openWithOverlay->raise();
    m_openWithPage->setFocus();
}

// диалог выбора файла в стиле tui для extract in и выбора ssh ключа а esc возвращает к предыдущему оверлею
void MainWindow::setupFileChooserOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_fileChooserOverlay = new QWidget(central);
    m_fileChooserOverlay->setObjectName(QStringLiteral("fileChooserOverlay"));
    m_fileChooserOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_fileChooserOverlay->setStyleSheet(QString(
        "QWidget#fileChooserOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    auto* overlayLayout = new QVBoxLayout(m_fileChooserOverlay);
    overlayLayout->setContentsMargins(2, 2, 2, 2);
    overlayLayout->setSpacing(0);

    m_fileChooserPage = new FileChooserDialog(m_fileChooserOverlay);
    overlayLayout->addWidget(m_fileChooserPage);

    connect(m_fileChooserPage, &FileChooserDialog::closeRequested, this, [this]() {
        if (m_fileChooserOverlay) m_fileChooserOverlay->hide();
        const bool wasSshKey = m_fileChooserForSshKey;
        m_fileChooserForExtract = false;
        m_fileChooserForSshKey = false;
        m_extractArchiveOverride.clear();
        if (wasSshKey) {
            if (m_connectionDialog) {
                m_connectionDialog->show();
                m_connectionDialog->raise();
                m_connectionDialog->focusSshKey();
            }
        } else if (m_millerView) {
            m_millerView->setFocus();
        }
        updateShortcutAvailability(QApplication::focusWidget());
    });
    // выбор подтверждён: каталог extract in… запускает распаковку выделенного
    // архива в выбранную папку
    connect(m_fileChooserPage, &FileChooserDialog::fileChosen, this, [this](const QString& path) {
        if (m_fileChooserForSshKey) {
            m_fileChooserForSshKey = false;
            if (m_fileChooserOverlay) m_fileChooserOverlay->hide();
            if (m_connectionDialog) {
                m_connectionDialog->show();
                m_connectionDialog->raise();
                m_connectionDialog->setSshKeyPath(path);
            }
            updateShortcutAvailability(QApplication::focusWidget());
            return;
        }
        if (m_fileChooserForExtract) {
            m_fileChooserForExtract = false;
            if (m_fileChooserOverlay) m_fileChooserOverlay->hide();
            if (m_millerView) {
                const std::filesystem::path dest(path.toUtf8().toStdString());
                if (m_extractArchiveOverride.empty()) {
                    m_millerView->extractSelectionInto(dest);
                } else {
                    m_millerView->extractArchiveInto(m_extractArchiveOverride, dest);
                }
                m_extractArchiveOverride.clear();
                m_millerView->setFocus();
            }
            return;
        }
    });

    m_fileChooserOverlay->hide();
    positionFileChooserOverlay();
}

void MainWindow::positionFileChooserOverlay() {
    if (!m_fileChooserOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    // заметно крупнее open with: сайдбар + широкий список файлов с
    // колонками размера/даты; по центру окна, с клампами для мелких окон
    const int cw = central->width();
    const int ch = central->height();
    constexpr int kMargin = 28;
    const int availW = std::max(360, cw - kMargin * 2);
    const int availH = std::max(300, ch - kMargin * 2);

    int w = std::clamp(cw * 78 / 100, 640, 1000);
    int h = std::clamp(ch * 82 / 100, 420, 700);
    w = std::min(w, availW);
    h = std::min(h, availH);
    m_fileChooserOverlay->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::showSshKeyChooserOverlay(const QString& startPath) {
    if (!m_fileChooserOverlay || !m_fileChooserPage) setupFileChooserOverlay();
    if (!m_fileChooserOverlay || !m_fileChooserPage) return;
    m_fileChooserForExtract = false;
    m_fileChooserForSshKey = true;
    m_extractArchiveOverride.clear();
    m_fileChooserPage->setDirectoryMode(false);
    m_fileChooserPage->setMultipleSelectionEnabled(false);
    m_fileChooserPage->setAcceptLabel(QStringLiteral("Select"));

    std::filesystem::path start(startPath.toUtf8().toStdString());
    std::error_code ec;
    if (!start.empty() && !std::filesystem::is_directory(start, ec)) {
        ec.clear();
        if (std::filesystem::is_regular_file(start, ec)) start = start.parent_path();
    }
    ec.clear();
    if (start.empty() || !std::filesystem::is_directory(start, ec)) {
        const char* home = std::getenv("HOME");
        const std::filesystem::path homePath = home && *home ? home : "/";
        const std::filesystem::path sshDir = homePath / ".ssh";
        ec.clear();
        start = std::filesystem::is_directory(sshDir, ec) ? sshDir : homePath;
    }
    m_fileChooserPage->setDirectory(start);
    positionFileChooserOverlay();
    m_fileChooserOverlay->show();
    m_fileChooserOverlay->raise();
    m_fileChooserPage->focusPrimaryInput();
    updateShortcutAvailability(QApplication::focusWidget());
}

// cheat sheet: та же механика оверлея, что у свойств, панель с 2px рамкой
// accent по центру поверх колонок (не отдельное окно и не страница превью);
// внутри таблица биндов helpdialog, клик по клавише переназначает действие,
// правый клик reset/disable
void MainWindow::setupHelpOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_helpOverlay = new QWidget(central);
    m_helpOverlay->setObjectName(QStringLiteral("helpOverlay"));
    m_helpOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_helpOverlay->setStyleSheet(QString(
        "QWidget#helpOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }"
    ).arg(Theme::Colors::OverlayBg().name(), Theme::Colors::OverlayBorder().name()));

    auto* overlayLayout = new QVBoxLayout(m_helpOverlay);
    overlayLayout->setContentsMargins(2, 2, 2, 2);
    overlayLayout->setSpacing(0);

    m_helpPage = new HelpDialog(m_helpOverlay);
    overlayLayout->addWidget(m_helpPage);

    connect(m_helpPage, &HelpDialog::closeRequested, this, [this]() {
        // крестик можно нажать прямо во время захвата: без cancelcapture()
        // все глобальные qshortcut останутся выключенными, а окно уже скрыто
        if (m_helpPage && m_helpPage->isCapturing()) m_helpPage->cancelCapture();
        if (m_helpOverlay) m_helpOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
        updateShortcutAvailability(QApplication::focusWidget());
    });
    // живое применение переназначенных биндов (keybinds.conf пишет сам
    // helpdialog через core::keybinds::saveuserbinding)
    connect(m_helpPage, &HelpDialog::keybindChanged, this, &MainWindow::reloadKeybinds);
    // захват клавиш: пока cheat sheet ждет комбинацию, все глобальные
    // qshortcut выключены. иначе alwaysactive f3/ctrl+q перехватят клавишу,
    // и назначить ее будет нельзя. после захвата состояния восстанавливает
    // updateshortcutavailability
    connect(m_helpPage, &HelpDialog::captureChanged, this, [this](bool capturing) {
        if (capturing) {
            for (QShortcut* sc : std::as_const(m_keybindShortcuts)) {
                if (sc) sc->setEnabled(false);
            }
            return;
        }
        for (QShortcut* sc : std::as_const(m_keybindShortcuts)) {
            if (sc) sc->setEnabled(true);
        }
        updateShortcutAvailability(QApplication::focusWidget());
    });

    m_helpOverlay->hide();
    positionHelpOverlay();
}

void MainWindow::positionHelpOverlay() {
    if (!m_helpOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    const int cw = central->width();
    const int ch = central->height();
    constexpr int kMargin = 36;
    const int availW = std::max(420, cw - kMargin * 2);
    const int availH = std::max(300, ch - kMargin * 2);

    int w = std::min(availW, std::clamp(cw * 62 / 100, 620, 900));
    int h = std::min(availH, std::clamp(ch * 84 / 100, 460, 740));
    m_helpOverlay->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::setupTasksOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_tasksOverlay = new TasksOverlay(central);
    connect(m_tasksOverlay, &TasksOverlay::closeRequested, this, [this]() {
        if (m_tasksOverlay) m_tasksOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
        updateShortcutAvailability(QApplication::focusWidget());
    });
    m_tasksOverlay->hide();
    positionTasksOverlay();
}

void MainWindow::positionTasksOverlay() {
    if (!m_tasksOverlay) return;
    m_tasksOverlay->repositionWithinParent();
}

void MainWindow::toggleTasksOverlay() {
    if (!m_tasksOverlay) setupTasksOverlay();
    if (!m_tasksOverlay) return;
    if (m_tasksOverlay->isVisible()) {
        m_tasksOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
        updateShortcutAvailability(QApplication::focusWidget());
        return;
    }
    m_tasksOverlay->refresh();
    positionTasksOverlay();
    m_tasksOverlay->show();
    m_tasksOverlay->raise();
    m_tasksOverlay->setFocus();
    // del, enter и стрелки должны попадать в оверлей, а не в глобальные шорткаты.
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::setupConnectionOverlay() {
    if (m_connectionDialog) return;
    auto* central = centralWidget();
    if (!central) return;

    m_connectionDialog = new ConnectionDialog(central);
    connect(m_connectionDialog, &ConnectionDialog::closeRequested, this, [this]() {
        if (m_millerView) m_millerView->setFocus();
        updateShortcutAvailability(QApplication::focusWidget());
    });
    connect(m_connectionDialog, &ConnectionDialog::connectRequested, this,
            &MainWindow::connectNetwork);
    connect(m_connectionDialog, &ConnectionDialog::sshKeyBrowseRequested, this,
            &MainWindow::showSshKeyChooserOverlay);

    m_networkMount = new Core::Network::NetworkMount(this);
    connect(m_networkMount, &Core::Network::NetworkMount::mounted, this,
            [this](const QString& localPath, const QString& uri) {
                if (m_connectionDialog) {
                    m_connectionDialog->hide();
                    m_connectionDialog->setBusy(false);
                }
                const std::filesystem::path path(localPath.toUtf8().toStdString());
                if (m_millerView) {
                    std::error_code ec;
                    if (std::filesystem::is_directory(path, ec)) {
                        m_millerView->setDirectory(path);
                    } else if (std::filesystem::exists(path, ec)) {
                        m_millerView->setDirectory(path.parent_path());
                        m_millerView->selectPathAfterRefresh(path);
                    } else {
                        m_millerView->setDirectory(path);
                    }
                    m_millerView->setFocus();
                }
                if (m_statusBar) {
                    m_statusBar->setProgressMessage(
                        uri.isEmpty() ? QStringLiteral("Connected") : QStringLiteral("Connected: %1").arg(uri));
                }
                updateShortcutAvailability(QApplication::focusWidget());
            });
    connect(m_networkMount, &Core::Network::NetworkMount::failed, this,
            [this](const QString& message) {
                if (!m_connectionDialog) return;
                m_connectionDialog->showError(message);
                m_connectionDialog->show();
                m_connectionDialog->raise();
                m_connectionDialog->setFocus();
                updateShortcutAvailability(QApplication::focusWidget());
            });

    m_connectionDialog->hide();
    positionConnectionOverlay();
}

void MainWindow::positionConnectionOverlay() {
    if (!m_connectionDialog) return;
    auto* central = centralWidget();
    if (!central) return;
    const int cw = central->width();
    const int ch = central->height();
    const int w = std::min(std::max(360, cw - 56), 600);
    const int h = std::min(std::max(300, ch - 56), 500);
    m_connectionDialog->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::showConnectionOverlay(const QString& location) {
    QString error;
    Core::Network::Location parsed;
    if (!Core::Network::parse(location, &parsed, &error)) {
        if (m_statusBar) m_statusBar->setProgressMessage(error);
        return;
    }
    setupConnectionOverlay();
    if (!m_connectionDialog) return;

    positionConnectionOverlay();
    m_connectionDialog->open(parsed);
    m_connectionDialog->show();
    m_connectionDialog->raise();
    m_connectionDialog->focusPrimaryInput();
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::connectNetwork(const Core::Network::Location& location) {
    if (!m_connectionDialog) setupConnectionOverlay();
    if (!m_networkMount) return;
    m_connectionDialog->setBusy(true);
    m_networkMount->mount(location);
}

void MainWindow::setupBulkRenameOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_bulkRenameOverlay = new QWidget(central);
    m_bulkRenameOverlay->setObjectName(QStringLiteral("bulkRenameOverlay"));
    m_bulkRenameOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_bulkRenameOverlay->setStyleSheet(QString(
        "QWidget#bulkRenameOverlay { background-color: %1; border: 2px solid %2; border-radius: 0px; }")
        .arg(Theme::Colors::OverlayBg().name(), Theme::Colors::Accent().name()));

    auto* layout = new QVBoxLayout(m_bulkRenameOverlay);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    m_bulkRenamePage = new BulkRenamePanel(m_bulkRenameOverlay);
    layout->addWidget(m_bulkRenamePage, 1);

    connect(m_bulkRenamePage, &BulkRenamePanel::accepted, this, [this]() {
        const auto renames = m_bulkRenamePage ? m_bulkRenamePage->renames()
                                              : std::vector<std::pair<std::filesystem::path,
                                                                      std::filesystem::path>>{};
        hideBulkRenameOverlay();
        if (m_millerView && !renames.empty()) m_millerView->applyBulkRenames(renames);
    });
    connect(m_bulkRenamePage, &BulkRenamePanel::cancelled, this, [this]() {
        hideBulkRenameOverlay();
    });
    connect(m_bulkRenamePage, &BulkRenamePanel::statusMessage, this, [this](const QString& text) {
        if (m_statusBar) m_statusBar->setProgressMessage(text);
    });

    m_bulkRenameOverlay->hide();
    positionBulkRenameOverlay();
}

void MainWindow::positionBulkRenameOverlay() {
    if (!m_bulkRenameOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    const int cw = central->width();
    const int ch = central->height();
    constexpr int kMargin = 32;
    const int availW = std::max(360, cw - kMargin * 2);
    const int availH = std::max(280, ch - kMargin * 2);
    const int w = std::min(availW, std::clamp(cw * 72 / 100, 620, 980));
    const int h = std::min(availH, std::clamp(ch * 74 / 100, 360, 720));
    m_bulkRenameOverlay->setGeometry((cw - w) / 2, (ch - h) / 2, w, h);
}

void MainWindow::showBulkRenameOverlay(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    if (m_statusBar && m_statusBar->isPromptActive()) return;
    if (!m_bulkRenameOverlay || !m_bulkRenamePage) setupBulkRenameOverlay();
    if (!m_bulkRenameOverlay || !m_bulkRenamePage) return;

    m_bulkRenamePage->setPaths(paths);
    positionBulkRenameOverlay();
    m_bulkRenameOverlay->show();
    m_bulkRenameOverlay->raise();
    m_bulkRenamePage->focusFirstEdit();
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::hideBulkRenameOverlay(bool restoreFocus) {
    if (m_bulkRenameOverlay) m_bulkRenameOverlay->hide();
    if (restoreFocus && m_millerView) m_millerView->setFocus();
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::updateTaskSummary() {
    if (!m_statusBar) return;
    const auto tasks = Core::Tasks::TaskManager::instance().tasks();
    int active = 0;
    qint64 done = 0;
    qint64 total = 0;
    bool hasTotal = false;
    for (const auto& task : tasks) {
        if (task.state != Core::Tasks::State::Queued &&
            task.state != Core::Tasks::State::Running) {
            continue;
        }
        ++active;
        if (task.total > 0) {
            hasTotal = true;
            done += std::clamp<qint64>(task.done, 0, task.total);
            total += task.total;
        }
    }
    if (active == 0) {
        m_statusBar->setTaskSummary({});
        return;
    }
    if (hasTotal && total > 0) {
        const int pct = std::clamp<int>(static_cast<int>(done * 100 / total), 0, 100);
        m_statusBar->setTaskSummary(QStringLiteral("TASKS %1 · %2%").arg(active).arg(pct));
    } else {
        m_statusBar->setTaskSummary(QStringLiteral("TASKS %1").arg(active));
    }
}

QString MainWindow::layoutFilePath() const {
    const QString dir = Theme::configDir();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/layout.ini");
}

int MainWindow::preferredSidebarWidth() const {
    return m_sidebarWidth > 0 ? m_sidebarWidth : Theme::uiConfig().sidebarWidth;
}

void MainWindow::loadSavedLayout() {
    const QString path = layoutFilePath();
    if (path.isEmpty()) return;
    QSettings settings(path, QSettings::IniFormat);
    if (!settings.value(QStringLiteral("layout/saved"), false).toBool()) return;

    const auto& ui = Theme::uiConfig();
    bool ok = false;
    int w = settings.value(QStringLiteral("layout/window_width"), ui.windowWidth).toInt(&ok);
    if (!ok || w < 400 || w > 7680) w = ui.windowWidth;
    int h = settings.value(QStringLiteral("layout/window_height"), ui.windowHeight).toInt(&ok);
    if (!ok || h < 300 || h > 4320) h = ui.windowHeight;
    // setupui() выполняется до show(); wm всё равно перетайлит окно.
    resize(w, h);

    int sidebar = settings.value(QStringLiteral("layout/sidebar_width"), ui.sidebarWidth).toInt(&ok);
    m_sidebarWidth = std::clamp(ok ? sidebar : ui.sidebarWidth, 80, 800);

    const bool showBookmarks =
        settings.value(QStringLiteral("layout/show_bookmarks"), ui.showBookmarks).toBool();
    m_bookmarksEnabled = showBookmarks;
    m_bookmarksAutoHidden = false;
    if (m_bookmarksBar) {
        m_bookmarksBar->setMaximumWidth(std::max(380, preferredSidebarWidth()));
        m_bookmarksBar->setVisible(showBookmarks);
    }
    if (m_tabBar) {
        m_tabBar->setVisible(settings.value(QStringLiteral("layout/show_tabs"), ui.showTabBar).toBool());
    }
    if (m_statusBar) {
        m_statusBar->setVisible(
            settings.value(QStringLiteral("layout/show_status_bar"), ui.showStatusBar).toBool());
    }
    if (m_millerView) {
        const bool showParent = settings.value(
            QStringLiteral("layout/show_parent_column"), ui.showParentColumn).toBool();
        const bool showPreview = settings.value(
            QStringLiteral("layout/show_preview"), ui.showPreview).toBool();
        int parent = settings.value(
            QStringLiteral("layout/parent_width"), ui.parentColumnWidth).toInt(&ok);
        if (!ok) parent = ui.parentColumnWidth;
        int preview = settings.value(
            QStringLiteral("layout/preview_width"), ui.previewColumnWidth).toInt(&ok);
        if (!ok) preview = ui.previewColumnWidth;
        m_millerView->setColumnsConfig(showParent, showPreview,
                                       std::clamp(parent, 80, 10000),
                                       std::clamp(preview, 80, 10000),
                                       /*exactwidths=*/true);
    }

    // sizes() нельзя надёжно отложить до showevent: resize() внутри конструктора
    // может уже применить тему и выставить initial=true. поэтому ширину
    // сохранённого сайдбара задаём сразу.
    m_initialSplitterSizesApplied = false;
    if (m_middleSplitter && m_bookmarksBar &&
        m_bookmarksEnabled && !m_bookmarksBar->isHidden()) {
        const int handle = m_middleSplitter->handleWidth();
        const int avail = std::max(0, width() - handle);
        const int bookW = std::clamp(preferredSidebarWidth(), 80, std::max(80, avail / 2));
        m_middleSplitter->setSizes({bookW, std::max(0, avail - bookW)});
        m_initialSplitterSizesApplied = true;
    }
}

void MainWindow::saveLayout() {
    const QString path = layoutFilePath();
    if (path.isEmpty()) return;

    if (m_middleSplitter && m_bookmarksBar && m_bookmarksBar->isVisible()) {
        const QList<int> sizes = m_middleSplitter->sizes();
        if (!sizes.isEmpty() && sizes.front() > 0) {
            m_sidebarWidth = std::clamp(sizes.front(), 80, 800);
        }
    }
    const int sidebar = preferredSidebarWidth();

    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("layout/saved"), true);
    settings.setValue(QStringLiteral("layout/window_width"), width());
    settings.setValue(QStringLiteral("layout/window_height"), height());
    settings.setValue(QStringLiteral("layout/sidebar_width"), sidebar);
    settings.setValue(QStringLiteral("layout/show_bookmarks"), m_bookmarksEnabled);
    settings.setValue(QStringLiteral("layout/show_tabs"), m_tabBar && m_tabBar->isVisible());
    settings.setValue(QStringLiteral("layout/show_status_bar"),
                      m_statusBar && m_statusBar->isVisible());
    if (m_millerView) {
        settings.setValue(QStringLiteral("layout/show_parent_column"),
                          m_millerView->parentColumnEnabled());
        settings.setValue(QStringLiteral("layout/show_preview"),
                          m_millerView->previewColumnEnabled());
        settings.setValue(QStringLiteral("layout/parent_width"),
                          std::clamp(m_millerView->parentColumnWidth(), 80, 10000));
        settings.setValue(QStringLiteral("layout/preview_width"),
                          std::clamp(m_millerView->previewColumnWidth(), 80, 10000));
    }
    settings.sync();

    if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Layout saved"));
}

void MainWindow::resetLayout() {
    const QString path = layoutFilePath();
    if (!path.isEmpty()) QFile::remove(path);

    const auto& ui = Theme::uiConfig();
    m_sidebarWidth = std::clamp(ui.sidebarWidth, 80, 800);
    m_bookmarksEnabled = ui.showBookmarks;
    m_bookmarksAutoHidden = false;
    m_initialSplitterSizesApplied = false;

    if (!compositorManagesWindowSize()) resize(ui.windowWidth, ui.windowHeight);
    if (m_tabBar) m_tabBar->setVisible(ui.showTabBar);
    if (m_statusBar) m_statusBar->setVisible(ui.showStatusBar);
    if (m_bookmarksBar) {
        m_bookmarksBar->setMaximumWidth(std::max(380, preferredSidebarWidth()));
        m_bookmarksBar->setVisible(ui.showBookmarks);
    }
    if (m_millerView) {
        m_millerView->setColumnsConfig(ui.showParentColumn, ui.showPreview,
                                       ui.parentColumnWidth, ui.previewColumnWidth);
    }

    auto applySidebarSizes = [this]() {
        if (!m_middleSplitter || !m_bookmarksBar) return;
        const int avail = std::max(
            0, m_middleSplitter->width() - m_middleSplitter->handleWidth());
        if (avail <= 0) return;
        int bookW = 0;
        if (m_bookmarksEnabled && !m_bookmarksBar->isHidden()) {
            bookW = std::clamp(preferredSidebarWidth(), 80, std::max(80, avail / 2));
        }
        m_middleSplitter->setSizes({bookW, std::max(0, avail - bookW)});
        m_initialSplitterSizesApplied = true;
    };
    applySidebarSizes();
    // show/hide баров и resize() меняют геометрию не сразу; повторяем после
    // прохода раскладки иначе видны старые и новые панели одновременно
    QTimer::singleShot(0, this, applySidebarSizes);

    if (m_statusBar) m_statusBar->setProgressMessage(QStringLiteral("Layout default"));
}

void MainWindow::showExtractChooserOverlay() {
    m_extractArchiveOverride.clear();
    if (!m_fileChooserOverlay || !m_fileChooserPage) setupFileChooserOverlay();
    if (!m_fileChooserOverlay || !m_fileChooserPage || !m_millerView) return;
    // extract in…: тот же диалог «как open file», но в режиме каталогов
    // и с кнопкой extract, старт текущая папка; выбор подтверждает обработчик
    // filechosen (см. setupfilechooseroverlay), распаковка выделенного архива
    m_fileChooserForExtract = true;
    m_fileChooserForSshKey = false;
    m_fileChooserPage->setDirectoryMode(true);
    m_fileChooserPage->setAcceptLabel(QStringLiteral("Extract"));
    m_fileChooserPage->setDirectory(m_millerView->currentDirectory());
    positionFileChooserOverlay();
    m_fileChooserOverlay->show();
    m_fileChooserOverlay->raise();
    m_fileChooserPage->focusPrimaryInput();
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::showExtractChooserOverlayFor(const std::filesystem::path& archivePath) {
    // extract in… из папки превью: архив фиксирован, выставляем после
    // открытия окна (showextractchooseroverlay сбрасывает override)
    showExtractChooserOverlay();
    if (m_fileChooserForExtract) m_extractArchiveOverride = archivePath;
}

void MainWindow::setupShortcuts() {
    createKeybindShortcuts();

    // печатные и «одноклавишные» шорткаты (g, ?, del, ctrl+c/v/x, f2…) срабатывают
    // до keypressevent текстового поля, поэтому при фокусе в текстовых полях их гасим;
    // alwaysactive=true (esc, фокус на path/filter, ctrl+q) всегда активны
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget*, QWidget* now) { updateShortcutAvailability(now); });
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::createKeybindShortcuts() {
    // клавиатурная карта из core::keybinds: qshortcut на каждую назначенную
    // клавишу, пустой список = действие отключено; повторный вызов пересоздаёт
    // весь набор (reloadkeybinds, живые переназначения из cheat sheet)
    for (QShortcut* sc : std::as_const(m_keybindShortcuts))
        delete sc;
    m_keybindShortcuts.clear();
    m_contextSensitiveShortcuts.clear();

    QList<QShortcut*> alwaysActive;
    QHash<QString, QString> usedKeys; // канонический ключ в action id
    auto bind = [this, &alwaysActive, &usedKeys](const QString& action, std::function<void()> fn) {
        auto wrappedFn = [this, action, fn]() {
            if (m_millerView && m_millerView->tryPluginOverride(action)) return;
            fn();
        };
        const bool always = Core::Keybinds::isAlwaysActive(action);
        // path/filter остаются доступны из текстового поля, но уступают
        // оверлеям и inline prompt, иначе ctrl+l или ctrl+f уведут фокус
        // из открытого диалога
        const bool contextSensitive = action == QLatin1String("focus_path") ||
                                      action == QLatin1String("filter");
        for (const QString& key : Core::Keybinds::keysFor(action)) {
            const QString canonical = QKeySequence(key).toString(QKeySequence::PortableText);
            if (canonical.isEmpty()) continue;
            // два qshortcut на одну комбинацию qt считает неоднозначными и
            // может погасить оба. оставляем первый бинд и предупреждаем,
            // чтобы конфликт из plugin/config не отключал действие молча.
            const auto used = usedKeys.constFind(canonical);
            if (used != usedKeys.constEnd()) {
                // дубль внутри одного действия (alt+return и alt+enter)
                // безвреден а действие между разными командами грозит неоднозначным шорткатом
                if (used.value() != action) {
                    qWarning("[keybinds] duplicate shortcut %s for action %s skipped "
                             "(already used by %s)",
                             qUtf8Printable(canonical), qUtf8Printable(action),
                             qUtf8Printable(used.value()));
                }
                continue;
            }
            usedKeys.insert(canonical, action);
            auto* sc = new QShortcut(QKeySequence(key), this);
            connect(sc, &QShortcut::activated, this, wrappedFn);
            if (always) alwaysActive.append(sc);
            if (contextSensitive) m_contextSensitiveShortcuts.append(sc);
            m_keybindShortcuts.append(sc);
        }
    };

    // новая вкладка
    bind("new_tab", [this]() {
        const char* home = std::getenv("HOME");
        m_tabBar->addTab(home ? home : "/");
    });

    bind("open_in_new_tab", [this]() { m_millerView->openSelectionInNewTab(); });

    // закрыть вкладку (ctrl+w / ctrl+f4 по умолчанию)
    bind("close_tab", [this]() {
        onTabCloseRequested(m_tabBar->currentIndex());
    });

    // закрепить или открепить вкладку
    bind("pin_tab", [this]() {
        if (m_tabBar->count() > 0 && m_tabBar->currentIndex() >= 0) {
            auto& tab = m_tabBar->currentTabState();
            tab.isPinned = !tab.isPinned;
            m_tabBar->update();
        }
    });

    // переоткрыть закрытую вкладку (chrome / vscode / zed / explorer)
    bind("reopen_tab", [this]() {
        if (!m_closedTabsHistory.empty()) {
            auto path = m_closedTabsHistory.back();
            m_closedTabsHistory.pop_back();
            m_tabBar->addTab(path);
        }
    });

    // прямое переключение вкладок: tab_1 .. tab_9
    for (int i = 1; i <= 9; ++i) {
        bind(QString("tab_%1").arg(i), [this, idx = i - 1]() {
            if (idx < m_tabBar->count()) {
                m_tabBar->setCurrentIndex(idx);
            }
        });
    }

    // следующая вкладка (ctrl+tab / ctrl+pgdown по умолчанию)
    bind("next_tab", [this]() {
        if (m_tabBar->count() > 0) {
            int next = (m_tabBar->currentIndex() + 1) % m_tabBar->count();
            m_tabBar->setCurrentIndex(next);
        }
    });

    // предыдущая вкладка (ctrl+shift+tab / ctrl+pgup по умолчанию)
    bind("prev_tab", [this]() {
        if (m_tabBar->count() > 0) {
            int prev = (m_tabBar->currentIndex() - 1 + m_tabBar->count()) % m_tabBar->count();
            m_tabBar->setCurrentIndex(prev);
        }
    });

    // фокус на ввод пути (ctrl+l / alt+d / f4 по умолчанию)
    bind("focus_path", [this]() { m_statusBar->focusPathInput(); });

    // родительский каталог (alt+up по умолчанию)
    bind("parent_dir", [this]() {
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        const auto& cur = m_millerView->currentDirectory();
        if (cur.has_parent_path() && cur != cur.parent_path()) {
            m_millerView->setDirectory(cur.parent_path(), cur);
        }
    });

    // назад по истории
    bind("go_back", [this]() {
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoBack()) {
            auto exited = tab.currentPath;
            tab.goBack();
            m_millerView->setDirectory(tab.currentPath, exited);
        }
    });

    // вперёд по истории
    bind("go_forward", [this]() {
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoForward()) {
            tab.goForward();
            m_millerView->setDirectory(tab.currentPath);
        }
    });

    // домашний каталог
    bind("go_home", [this]() {
        const char* home = std::getenv("HOME");
        m_millerView->setDirectory(home ? home : "/");
    });

    // первый/последний элемент центральной колонки (home / end)
    bind("first_item", [this]() { m_millerView->selectFirst(); });
    bind("last_item", [this]() { m_millerView->selectLast(); });

    // переключить закладки / сайдбар (ctrl+b / ctrl+\ / f9 по умолчанию)
    bind("toggle_bookmarks", [this]() { toggleBookmarks(); });

    // быстрый переход
    bind("quick_jump", [this]() { showQuickJumpMenu(); });

    // обновить
    bind("refresh", [this]() { m_millerView->refresh(); });

    // поиск / фильтр
    bind("filter", [this]() {
        refreshFilterCommands();
        m_statusBar->activateFilter();
    });

    // показать или скрыть скрытые файлы (ctrl+h / ctrl+. по умолчанию)
    bind("toggle_hidden", [this]() { m_millerView->toggleHidden(); });

    // режимы сортировки
    bind("sort_name", [this]() { m_millerView->setSortMode(0); });
    bind("sort_size", [this]() { m_millerView->setSortMode(1); });
    bind("sort_date", [this]() { m_millerView->setSortMode(2); });
    bind("sort_type", [this]() { m_millerView->setSortMode(3); });

    // копировать путь / распаковать
    bind("copy_path", [this]() { m_millerView->copyPaths(0); });
    bind("copy_rel_path", [this]() { m_millerView->copyPaths(2); });
    bind("extract_here", [this]() { m_millerView->extractHere(); });

    // выделить всё
    bind("select_all", [this]() { m_millerView->selectAll(); });

    // отмена: escape снимает подсказку ввода / активную файловую операцию / оверлеи / выделение
    bind("cancel", [this]() {

        if (m_fileChooserOverlay && m_fileChooserOverlay->isVisible()) {
            m_fileChooserOverlay->hide();
            const bool wasSshKey = m_fileChooserForSshKey;
            m_fileChooserForExtract = false;
            m_fileChooserForSshKey = false;
            m_extractArchiveOverride.clear();
            if (wasSshKey) {
                if (m_connectionDialog) {
                    m_connectionDialog->show();
                    m_connectionDialog->raise();
                    m_connectionDialog->focusSshKey();
                }
            } else if (m_millerView) {
                m_millerView->setFocus();
            }
            updateShortcutAvailability(QApplication::focusWidget());
            return;
        }

        if (m_connectionDialog && m_connectionDialog->isVisible()) {
            if (m_connectionDialog->isBusy()) return;
            m_connectionDialog->hide();
            if (m_millerView) m_millerView->setFocus();
            updateShortcutAvailability(QApplication::focusWidget());
            return;
        }

        // массовое переименование: esc закрывает панель, ничего не меняя
        if (m_bulkRenameOverlay && m_bulkRenameOverlay->isVisible()) {
            hideBulkRenameOverlay();
            return;
        }

        // tasksoverlay: esc возвращает из лога к списку, иначе закрывает панель
        if (m_tasksOverlay && m_tasksOverlay->isVisible()) {
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(m_tasksOverlay, &esc);
            return;
        }

        // cheat sheet (f1/?) самый «верхний» оверлей: esc либо отменяет
        // захват клавиши, либо закрывает шпаргалку
        if (m_helpOverlay && m_helpOverlay->isVisible()) {
            if (m_helpPage && m_helpPage->isCapturing()) {
                m_helpPage->cancelCapture();
            } else {
                m_helpOverlay->hide();
                if (m_millerView) m_millerView->setFocus();
            }
            return;
        }

        if (m_statusBar && m_statusBar->isPromptActive()) {
            m_statusBar->cancelInlinePrompt();
            return;
        }

        if (m_statusBar && m_statusBar->isFilterActive() &&
            m_millerView && m_millerView->isPluginPreviewActive()) {
            if (m_statusBar->cancelFilterCommand()) return;
            m_statusBar->clearFilter();
            return;
        }

        // вывод команды плагина в три панели и esc возвращает к колонкам
        if (m_millerView && m_millerView->isPluginPanesVisible()) {
            m_millerView->closePluginPanes();
            return;
        }

        // оверлей вывода плагина (plugins) транзиентный, закрывается первым
        const auto outputs = findChildren<TuiOutputOverlay*>(Qt::FindDirectChildrenOnly);
        for (int i = outputs.size() - 1; i >= 0; --i) {
            if (outputs[i]->isVisible()) {
                outputs[i]->hide();
                return;
            }
        }

        // оверлей open with открыт: закрываем его, иначе qshortcut перехватит esc до обработчика диалога
        if (m_openWithOverlay && m_openWithOverlay->isVisible()) {
            m_openWithOverlay->hide();
            if (m_millerView) m_millerView->setFocus();
            return;
        }
        if (m_propsOverlay && m_propsOverlay->isVisible()) {
            m_propsOverlay->hide();
            if (m_millerView) m_millerView->setFocus();
            return;
        }
        if (Dnd::DragDropHandler::hasActiveOperation()) {
            Dnd::DragDropHandler::cancelActiveOperation();
            return;
        }
        // архив в центре: esc возвращает к каталогу, не трогая выделение под ним
        if (m_millerView->isArchiveViewActive()) {
            m_millerView->leaveArchive();
            return;
        }
        m_millerView->clearSelection();
        // фильтр транзиентный: esc снимает его вместе с редактором
        if (m_statusBar->isFilterActive()) {
            if (m_statusBar->cancelFilterCommand()) return;
            m_statusBar->clearFilter();
        }
        if (m_millerView->hasGlobalSearch()) {
            m_millerView->clearGlobalSearch();
        }
    });

    // вырезать / копировать / вставить
    bind("cut", [this]() { m_millerView->cutSelection(); });
    bind("copy", [this]() { m_millerView->copySelection(); });
    bind("paste", [this]() { m_millerView->pasteClipboard(); });

    // отмена: восстанавливает случайно перемещённые или сброшенные файлы
    bind("undo", [this]() {
        if (Dnd::DragDropHandler::hasActiveOperation()) {
            Dnd::DragDropHandler::cancelActiveOperation();
            if (m_statusBar) m_statusBar->setProgressMessage("Cancel");
            return;
        }
        QString msg;
        std::filesystem::path selectPath;
        if (Core::UndoManager::instance().undo(msg, &selectPath)) {
            if (!selectPath.empty()) m_millerView->selectPathAfterRefresh(selectPath);
            m_millerView->refresh();
            m_statusBar->setProgressMessage(msg);
        } else {
            m_statusBar->setProgressMessage(msg.isEmpty() ? "Nothing to undo" : msg);
        }
    });

    // redo (ctrl+y / ctrl+shift+z дефолт; qkeysequence::redo на linux
    // резолвится ровно в ctrl+y, поэтому оба варианта в таблице)
    auto doRedo = [this]() {
        QString msg;
        std::filesystem::path selectPath;
        if (Core::UndoManager::instance().redo(msg, &selectPath)) {
            if (!selectPath.empty()) m_millerView->selectPathAfterRefresh(selectPath);
            m_millerView->refresh();
            m_statusBar->setProgressMessage(msg);
        } else {
            m_statusBar->setProgressMessage(msg.isEmpty() ? "Nothing to redo" : msg);
        }
    };
    bind("redo", doRedo);

    // переименовать
    bind("rename", [this]() { m_millerView->renameCurrent(); });

    // в корзину
    bind("trash", [this]() { m_millerView->trashSelection(); });

    // удалить безвозвратно
    bind("delete_perm", [this]() { m_millerView->deletePermanentlySelection(); });

    // создать папку
    bind("new_folder", [this]() { m_millerView->createNewFolder(); });

    // создать файл
    bind("new_file", [this]() { m_millerView->createNewFile(); });

    // массовое создание файлов / папок
    bind("create_bulk", [this]() { m_millerView->createBulkItems(); });

    // свойства
    bind("properties", [this]() { m_millerView->showProperties(); });

    // диалог помощи
    bind("help", [this]() { showHelpDialog(); });

    // фоновые задачи: прогресс, отмена, лог
    bind("tasks", [this]() { toggleTasksOverlay(); });

    // раскладка: сохранить текущую / вернуть значения из theme.conf
    bind("save_layout", [this]() { saveLayout(); });
    bind("reset_layout", [this]() { resetLayout(); });

    // выход
    bind("quit", [this]() { close(); });

    // кастомные действия плагинов (action/keys в plugins/*.conf): как у пункта
    // меню plugins, выделение центра + текущий каталог
    {
        const auto pluginActions = Core::Keybinds::acceptedPluginActions();
        for (auto it = pluginActions.constBegin(); it != pluginActions.constEnd(); ++it) {
            const QString actionId = it.key();
            const QString pluginId = it.value();
            if (NativePluginHost::instance().hasAction(actionId)) {
                bind(actionId, [this, actionId]() { m_millerView->runNativePluginAction(actionId); });
            } else {
                bind(actionId, [this, pluginId]() { m_millerView->runPluginById(pluginId); });
            }
        }
    }

    // состав textsensitive: как в setupshortcuts, печатные шорткаты уступают
    // ввод в текстовых полях, alwaysactive остаются; пересчитывается при каждом
    // пересоздании (после живого переназначения биндов из cheat sheet)
    // только глобальные бинды этого окна, а не qshortcut внутри дочерних
    // диалогов: иначе esc и ctrl+l самого filechooserdialog тоже погаснут
    m_textSensitiveShortcuts = m_keybindShortcuts;
    for (QShortcut* sc : std::as_const(alwaysActive))
        m_textSensitiveShortcuts.removeAll(sc);
    for (QShortcut* sc : std::as_const(m_contextSensitiveShortcuts))
        m_textSensitiveShortcuts.removeAll(sc);
}

// живое применение keybinds.conf: перечитать конфиг, пересоздать шорткаты,
// восстановить доступность по фокусу. вызывается из cheat sheet после
// каждого rebind/reset/disable (сигнал keybindchanged)
void MainWindow::reloadKeybinds() {
    Core::Keybinds::reload();
    createKeybindShortcuts();
    updateShortcutAvailability(QApplication::focusWidget());
}

static bool isTextInputWidget(QWidget* w) {
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w) ||
           qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w);
}

void MainWindow::updateShortcutAvailability(QWidget* focus) {
    // модальный слой забирает клавиатуру себе чтобы del не удалил файл в корзину за шпаргалкой а ctrl+c не скопировал файл мимо диалога
    bool otherOverlayOpen = (m_tasksOverlay && m_tasksOverlay->isVisible()) ||
                            (m_helpOverlay && m_helpOverlay->isVisible()) ||
                            (m_openWithOverlay && m_openWithOverlay->isVisible()) ||
                            (m_fileChooserOverlay && m_fileChooserOverlay->isVisible()) ||
                            (m_propsOverlay && m_propsOverlay->isVisible()) ||
                            (m_connectionDialog && m_connectionDialog->isVisible()) ||
                            (m_bulkRenameOverlay && m_bulkRenameOverlay->isVisible()) ||
                            (m_millerView && m_millerView->isPluginPanesVisible());
    if (!otherOverlayOpen) {
        const auto outputs = findChildren<TuiOutputOverlay*>(Qt::FindDirectChildrenOnly);
        for (TuiOutputOverlay* ov : outputs) {
            if (ov && ov->isVisible()) {
                otherOverlayOpen = true;
                break;
            }
        }
    }
    const bool promptActive = m_statusBar && m_statusBar->isPromptActive();
    const bool typing = isTextInputWidget(focus) || otherOverlayOpen || promptActive;
    for (QShortcut* sc : std::as_const(m_textSensitiveShortcuts)) {
        if (sc) sc->setEnabled(!typing);
    }
    // esc и ctrl+q остаются глобальными а ctrl+l и ctrl+f не должны переключать фокус пока открыт оверлей или пароль
    for (QShortcut* sc : std::as_const(m_contextSensitiveShortcuts)) {
        if (sc) sc->setEnabled(!(otherOverlayOpen || promptActive));
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    QMainWindow::keyPressEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (!m_initialSplitterSizesApplied && m_middleSplitter) {
        // фиксируем факт первой раскладки даже в крошечном окне чтобы растягивание не применило дефолты повторно
        m_initialSplitterSizesApplied = true;
        const int avail = m_middleSplitter->width();
        if (avail >= 300) {
            const int kHandle = m_middleSplitter->handleWidth();
            if (m_bookmarksBar && m_bookmarksEnabled && !m_bookmarksBar->isHidden()) {
                const int bookW = std::clamp(preferredSidebarWidth(), 80,
                                             std::max(80, avail / 2));
                m_middleSplitter->setSizes({bookW, avail - bookW - kHandle});
            }
        }
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    positionPropertiesOverlay();
    positionOpenWithOverlay();
    positionFileChooserOverlay();
    positionHelpOverlay();
    positionTasksOverlay();
    positionConnectionOverlay();
    positionBulkRenameOverlay();

    // оверлеи вывода плагинов (plugins) держатся правого нижнего угла окна
    const auto outputOverlays = findChildren<TuiOutputOverlay*>(Qt::FindDirectChildrenOnly);
    for (TuiOutputOverlay* ov : outputOverlays) {
        if (ov && ov->isVisible()) ov->repositionWithinParent();
    }

    // компактная шапка 20px при высоте окна < 480, иначе высота enginetabbar::knormalheight (30px)
    const bool compactBars = event->size().height() < 480;
    const int barH = compactBars ? 20 : EngineTabBar::kNormalHeight;
    const int statusBarH = 25; // статус: контент 18px + 7px отступа
    if (m_tabBar && (m_tabBar->height() != barH || m_tabBar->maximumHeight() != barH)) m_tabBar->setFixedHeight(barH);
    if (m_statusBar && (m_statusBar->height() != statusBarH || m_statusBar->maximumHeight() != statusBarH)) m_statusBar->setFixedHeight(statusBarH);

    if (!m_middleSplitter || !m_bookmarksBar || !m_millerView) return;

    int avail = event->size().width();
    if (avail <= 0) return;
    const int kHandle = m_middleSplitter->handleWidth();

    // адаптивные минимумы: в узком окне жёсткие 100+200+2=302px превышали ширину
    // и рукоятка bookmarks не двигалась; совсем узкий режим (<250) даёт ход
    // вплоть до ~102px окна
    const bool tiny = avail < 250;
    const bool tight = avail < 400;
    const int kBookMin = tiny ? 36 : (tight ? 48 : 100);
    const int kBookPref = std::clamp(preferredSidebarWidth(), kBookMin,
                                     std::max(kBookMin, avail / 2));
    const int kMillerMin = tiny ? 64 : (tight ? 140 : 200);
    if (m_bookmarksBar->minimumWidth() != kBookMin) m_bookmarksBar->setMinimumWidth(kBookMin);
    if (m_millerView->minimumWidth() != kMillerMin) m_millerView->setMinimumWidth(kMillerMin);

    // на самом дне (<120px) рукоятке физически некуда двигаться (оба минимума
    // съедают всю ширину) прячем закладки совсем, как панели миллера
    if (avail < 120) {
        if (m_bookmarksBar->isVisible()) {
            m_bookmarksBar->hide();
            m_bookmarksAutoHidden = true;
        }
    } else if (m_bookmarksEnabled && m_bookmarksAutoHidden && !m_bookmarksBar->isVisible()) {
        m_bookmarksBar->show();
        m_bookmarksAutoHidden = false;
    }

    // задаём удобную начальную ширину панели закладок при первой раскладке
    if (!m_initialSplitterSizesApplied && avail >= 300) {
        m_initialSplitterSizesApplied = true;
        if (m_bookmarksEnabled && !m_bookmarksBar->isHidden()) {
            m_middleSplitter->setSizes({kBookPref, avail - kBookPref - kHandle});
            return;
        }
    }

    // в узком окне (<400px) закладки ужимаются первыми, не шире половины окна
    // и не в ущерб минимуму миллера, чтобы список файлов сохранял читаемую ширину;
    // совсем узко (<250): закладки сжимаются до иконок, панель ~¼ ширины, файлам ~¾
    if (m_bookmarksBar->isVisible() && avail < 400) {
        QList<int> cur = m_middleSplitter->sizes();
        if (cur.size() == 2) {
            const int halfCap = std::min(avail / 2, avail - kMillerMin - kHandle);
            const int cap = tiny ? std::max(kBookMin, std::min(avail / 4, halfCap))
                                 : std::max(kBookMin, halfCap);
            const int bookW = std::clamp(cur[0], kBookMin, cap);
            if (cur[0] != bookW) {
                m_middleSplitter->setSizes({bookW, avail - bookW - kHandle});
            }
        }
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // задачи закрываются настроенным биндом tasks и пока оверлей открыт обрабатываем клавишу здесь независимо от фокуса
    if (event->type() == QEvent::KeyPress && m_tasksOverlay && m_tasksOverlay->isVisible()) {
        auto* key = static_cast<QKeyEvent*>(event);
        const auto mods = key->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                              Qt::MetaModifier | Qt::ShiftModifier);
        bool closeKey = false;
        for (const QString& binding : Core::Keybinds::keysFor(QStringLiteral("tasks"))) {
            const QKeySequence seq(binding);
            if (seq.count() != 1) continue;
            if (key->key() == seq[0].key() && mods == seq[0].keyboardModifiers()) {
                closeKey = true;
                break;
            }
        }
        const bool higherOverlay =
            (m_helpOverlay && m_helpOverlay->isVisible()) ||
            (m_openWithOverlay && m_openWithOverlay->isVisible()) ||
            (m_fileChooserOverlay && m_fileChooserOverlay->isVisible()) ||
            (m_propsOverlay && m_propsOverlay->isVisible()) ||
            (m_bulkRenameOverlay && m_bulkRenameOverlay->isVisible());
        if (closeKey && !higherOverlay) {
            toggleTasksOverlay();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::BackButton) {
            if (m_millerView->tryPluginOverride(QStringLiteral("go_back"))) return true;
            auto& tab = m_tabBar->currentTabState();
            if (tab.canGoBack()) {
                auto exited = tab.currentPath;
                tab.goBack();
                m_millerView->setDirectory(tab.currentPath, exited);
            }
            return true;
        } else if (mouseEvent->button() == Qt::ForwardButton) {
            if (m_millerView->tryPluginOverride(QStringLiteral("go_forward"))) return true;
            auto& tab = m_tabBar->currentTabState();
            if (tab.canGoForward()) {
                tab.goForward();
                m_millerView->setDirectory(tab.currentPath);
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::toggleBookmarks() {
    if (!m_bookmarksBar) return;
    m_bookmarksAutoHidden = false;
    m_bookmarksEnabled = !m_bookmarksBar->isVisible();
    m_bookmarksBar->setVisible(m_bookmarksEnabled);

    // если запустились без закладок то ширина могла остаться нулевой и при включении возвращаем настроенный размер
    if (m_bookmarksEnabled && m_middleSplitter) {
        const QList<int> sizes = m_middleSplitter->sizes();
        if (sizes.size() == 2 && sizes[0] < 40) {
            const int avail = m_middleSplitter->width() - m_middleSplitter->handleWidth();
            const int bookW = std::clamp(preferredSidebarWidth(), 80,
                                         std::max(80, avail / 2));
            m_middleSplitter->setSizes({bookW, std::max(0, avail - bookW)});
        }
    }
}

void MainWindow::showQuickJumpMenu() {
    if (m_statusBar && m_statusBar->isPromptActive()) return;
    QMenu menu(this);
    menu.setFont(Theme::getMonospaceFont(9));
    menu.setStyleSheet(Theme::getApplicationStyleSheet());

    auto addJump = [this, &menu](const QString& title, const QString& pathStr) {
        auto act = menu.addAction(title);
        connect(act, &QAction::triggered, this, [this, pathStr]() {
            m_millerView->setDirectory(std::filesystem::path(pathStr.toUtf8().toStdString()));
        });
    };

    const char* home = std::getenv("HOME");
    QString h = home ? QString::fromUtf8(home) : "/";

    addJump("~  Home", h);
    addJump("/  Root", "/");
    addJump("󰉋  Documents", h + "/Documents");
    addJump("󰉍  Downloads", h + "/Downloads");
    addJump("󰉏  Desktop", h + "/Desktop");
    addJump("󰉏  Pictures", h + "/Pictures");
    addJump("󱂛  Music", h + "/Music");
    addJump("󰈰  Videos", h + "/Videos");

    menu.exec(QCursor::pos());
}

void MainWindow::showHelpDialog() {
    if (m_statusBar && m_statusBar->isPromptActive()) return;
    if (!m_helpOverlay || !m_helpPage) setupHelpOverlay();
    if (!m_helpOverlay || !m_helpPage) return;
    // пересобрать строки: таблица биндов и плагины могли измениться
    if (m_helpPage->isCapturing()) m_helpPage->cancelCapture();
    m_helpPage->rebuild();
    positionHelpOverlay();
    m_helpOverlay->show();
    m_helpOverlay->raise();
    m_helpPage->setFocus();
}

void MainWindow::saveCurrentTabState() {
    if (m_lastTabIndex >= 0 && m_lastTabIndex < m_tabBar->count() && m_millerView) {
        auto& tab = m_tabBar->tabState(m_lastTabIndex);
        tab.currentPath = m_millerView->currentDirectory();
        tab.isSudo = m_millerView->isCurrentSudo();
        tab.sudoRoot = m_millerView->sudoRoot();
        tab.selectedItem = m_millerView->focusedPath();
        tab.selectedPaths = m_millerView->multiSelectedPaths();
    }
}

void MainWindow::restoreTabState(int index) {
    if (index < 0 || index >= m_tabBar->count() || !m_millerView) return;
    const auto& curTab = m_tabBar->tabState(index);
    // корень sudo у каждой вкладки свой: индикатор загорится только в каталоге, открытом через sudo
    m_millerView->setSudoRoot(curTab.sudoRoot);
    m_millerView->setCurrentSudo(curTab.isSudo);
    if (m_statusBar) {
        m_statusBar->setSudoBadge(curTab.isSudo);
    }

    if (m_millerView->currentDirectory() != curTab.currentPath) {
        m_millerView->setDirectory(curTab.currentPath, curTab.selectedItem);
        if (!curTab.selectedPaths.empty()) {
            m_millerView->restoreMultiSelection(curTab.selectedPaths, curTab.selectedItem);
        }
    } else {
        if (!curTab.selectedPaths.empty()) {
            m_millerView->restoreMultiSelection(curTab.selectedPaths, curTab.selectedItem);
        } else if (!curTab.selectedItem.empty()) {
            m_millerView->selectAndFocusPath(curTab.selectedItem);
        }
    }

    m_millerView->focusCenter();

    if (m_bookmarksBar) {
        m_bookmarksBar->setActiveDirectory(curTab.currentPath);
    }
    m_lastTabIndex = index;
}

void MainWindow::onTabChanged(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    m_isSwitchingTab = true;
    // архив живёт только в текущей вкладке; при переключении возвращаемся в каталог
    if (m_millerView && m_millerView->isArchiveViewActive()) m_millerView->leaveArchive();
    saveCurrentTabState();
    restoreTabState(index);
    // archiveleft мог обновить заголовок уже выбранной вкладки: синхронизируем с её каталогом
    if (m_millerView && !m_millerView->currentDirectory().empty()) {
        onDirectoryChanged(m_millerView->currentDirectory());
    }
    m_isSwitchingTab = false;
}

void MainWindow::onTabCloseRequested(int index) {
    // крестик или средняя кнопка на вкладке тоже точка переопределения как закрытие вкладки
    if (m_millerView && m_millerView->tryPluginOverride(QStringLiteral("close_tab"))) return;
    if (m_tabBar->count() <= 1) {
        const char* home = std::getenv("HOME");
        std::filesystem::path homePath = home ? home : "/";
        m_millerView->setDirectory(homePath);
        if (m_bookmarksBar) {
            m_bookmarksBar->setActiveDirectory(homePath);
        }
        m_tabBar->updateTabTitle(0, "~");
        m_tabBar->tabState(0).navigateTo(homePath);
        return;
    }

    if (index >= 0 && index < m_tabBar->count()) {
        if (!m_tabBar->tabState(index).isPinned) {
            m_closedTabsHistory.push_back(m_tabBar->tabState(index).currentPath);
            m_lastTabIndex = -1;
            m_tabBar->removeTab(index);
        }
    }
}

void MainWindow::onNewTabRequested(const std::filesystem::path& path) {
    // «+» и двойной клик по пустому месту шапки (клавиатурный new_tab уже перехвачен в bind)
    if (m_millerView && m_millerView->tryPluginOverride(QStringLiteral("new_tab"))) return;
    std::filesystem::path target = path;
    if (target.empty()) {
        const char* home = std::getenv("HOME");
        target = home ? home : "/";
    }
    m_tabBar->addTab(target);
}

void MainWindow::onDuplicateTabRequested(int index) {
    if (m_millerView && m_millerView->tryPluginOverride(QStringLiteral("duplicate_tab"))) return;
    if (index >= 0 && index < m_tabBar->count()) {
        m_tabBar->addTab(m_tabBar->tabState(index).currentPath);
    }
}

void MainWindow::onPinTabRequested(int index) {
    if (m_millerView && m_millerView->tryPluginOverride(QStringLiteral("pin_tab"))) return;
    if (index >= 0 && index < m_tabBar->count()) {
        m_tabBar->tabState(index).isPinned = !m_tabBar->tabState(index).isPinned;
        m_tabBar->update();
    }
}

void MainWindow::onCloseOtherTabsRequested(int index) {
    if (m_millerView && m_millerView->tryPluginOverride(QStringLiteral("close_others"))) return;
    m_tabBar->closeOtherTabs(index);
}

void MainWindow::onDirectoryChanged(const std::filesystem::path& path) {
    QString pathQ = QString::fromUtf8(path.c_str());
    if (m_tabBar->count() > 0 && m_tabBar->currentIndex() >= 0) {
        auto& tab = m_tabBar->currentTabState();
        if (!m_isSwitchingTab && tab.currentPath != path) {
            tab.navigateTo(path);
        }
        QString fnameQ = QString::fromUtf8(path.filename().c_str());
        m_tabBar->updateTabTitle(m_tabBar->currentIndex(), fnameQ.isEmpty() ? pathQ : fnameQ);
    }

    if (m_bookmarksBar) {
        m_bookmarksBar->setActiveDirectory(path);
    }

    m_statusBar->setPath(path);
    setWindowTitle(QString("atlifm: %1").arg(pathQ));
}

void MainWindow::onStatusUpdated(int currentIndex, int totalItems, int selectedItems, const QString& perms) {
    m_statusBar->setItemPerms(perms);
    m_statusBar->setItemStats(currentIndex, totalItems, selectedItems);
}

void MainWindow::refreshFilterCommands() {
    QStringList tokens{QStringLiteral("fd"), QStringLiteral("rg")};
    m_filterPluginCommands.clear();

    auto addCommand = [this, &tokens](const Core::Plugins::Plugin& plugin) {
        const QString token = plugin.command.trimmed().toLower();
        if (token.isEmpty() || tokens.contains(token)) return;
        FilterPluginCommand command;
        command.pluginId = plugin.id;
        command.actionId = plugin.action;
        command.title = plugin.commandTitle.isEmpty() ? plugin.name : plugin.commandTitle;
        command.mode = plugin.commandMode.isEmpty() ? QStringLiteral("action") : plugin.commandMode;
        command.native = plugin.native;
        m_filterPluginCommands.insert(token, command);
        tokens << token;
    };

    for (const auto& plugin : Core::Plugins::load()) addCommand(plugin);
    for (const auto& plugin : NativePluginHost::instance().manifests()) addCommand(plugin);
    if (m_statusBar) m_statusBar->setFilterCommands(tokens);
}

void MainWindow::runFilterPluginCommand(const FilterPluginCommand& command, const QString& query) {
    if (!m_millerView) return;
    if (command.native) {
        m_millerView->runNativePluginCommand(command.actionId, query);
    } else {
        m_millerView->runPluginCommand(command.pluginId, query);
    }
}

void MainWindow::beginPluginPanes(const QString& title) {
    if (m_millerView) m_millerView->beginPluginPanes(title);
}

void MainWindow::appendPluginPanesOutput(const QByteArray& data) {
    if (m_millerView) m_millerView->appendPluginPanesOutput(data);
}

void MainWindow::appendPluginPaneOutput(int pane, const QByteArray& data) {
    if (m_millerView) m_millerView->appendPluginPaneOutput(pane, data);
}

void MainWindow::finishPluginPanes(int exitCode) {
    if (m_millerView) m_millerView->finishPluginPanes(exitCode);
}

void MainWindow::closePluginPanes() {
    if (m_millerView) m_millerView->closePluginPanes();
}

void MainWindow::onFilterChanged(const QString& filter) {
    if (m_millerView) {
        m_millerView->cancelPluginPreview(
            true, !(m_statusBar && m_statusBar->isFilterActive()));
    }
    if (m_filterSearchTimer) m_filterSearchTimer->stop();
    m_keepGlobalResultsOnDismiss = false;
    m_millerView->clearGlobalSearch();
    m_millerView->setFilter(filter);
}

void MainWindow::onFilterCommandChanged(const QString& token, const QString& query) {
    if (token == QLatin1String("fd") || token == QLatin1String("rg")) {
        if (m_millerView) m_millerView->cancelPluginPreview(true, false);
        m_pendingSearchToken = token;
        m_pendingSearchQuery = query;
        if (m_filterSearchTimer) m_filterSearchTimer->stop();
        if (query.trimmed().isEmpty()) {
            m_millerView->clearGlobalSearch();
            return;
        }
        if (m_filterSearchTimer) m_filterSearchTimer->start();
        return;
    }

    const auto it = m_filterPluginCommands.constFind(token);
    if (it != m_filterPluginCommands.constEnd() &&
        it->mode == QLatin1String("query") && !it->native) {
        m_millerView->schedulePluginPreview(it->pluginId, query);
    } else if (m_millerView) {
        m_millerView->cancelPluginPreview(true, false);
    }
}

void MainWindow::onFilterSubmitted() {
    if (!m_statusBar) return;
    const QString token = m_statusBar->filterCommand();
    const QString query = m_statusBar->filterQuery();
    if (m_millerView) m_millerView->cancelPluginPreview(true);
    if (token.isEmpty()) {
        m_statusBar->deactivateFilter();
        return;
    }

    if (token == QLatin1String("fd") || token == QLatin1String("rg")) {
        if (m_filterSearchTimer) m_filterSearchTimer->stop();
        if (query.trimmed().isEmpty()) {
            m_millerView->clearGlobalSearch();
            m_statusBar->deactivateFilter();
            return;
        }
        if (token == QLatin1String("fd")) {
            m_millerView->searchEverywhere(query);
        } else {
            m_millerView->searchContents(query);
        }
        m_keepGlobalResultsOnDismiss = true;
        m_statusBar->deactivateFilter();
        return;
    }

    const auto it = m_filterPluginCommands.constFind(token);
    if (it != m_filterPluginCommands.constEnd()) {
        runFilterPluginCommand(*it, it->mode == QLatin1String("query") ? query : QString());
    }
    m_statusBar->deactivateFilter();
}

}
