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
#include "HelpDialog.hpp"
#include "TuiOutputOverlay.hpp"
#include <QVBoxLayout>
#include <QShortcut>
#include <QKeySequence>
#include <QKeyEvent>
#include <QResizeEvent>
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
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

namespace Widgets {

MainWindow::MainWindow(const std::filesystem::path& initialPath, QWidget* parent)
    : QMainWindow(parent) {
    // плагины C++ (~/.config/atlifm/plugins/*.so) загружаются до первого обращения
    // к Keybinds/меню: их действия участвуют в биндингах, подменю Plugins, override/hooks
    NativePluginHost::instance().setWindow(this);
    NativePluginHost::instance().initialize();
    Core::Plugins::setNativeProvider([]() {
        return Widgets::NativePluginHost::instance().manifests();
    });

    setWindowTitle("atlifm");
    const auto& ui = Theme::uiConfig();
    resize(ui.windowWidth, ui.windowHeight);
    // иконка вшита в resources.qrc; бары Wayland сопоставляют её по app-id,
    // X11 напрямую; ставим на уровне окна для любого способа запуска
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
        // Open In: передан файл (не папка), открываем его каталог, файл
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
    NativePluginHost::instance().setWindow(nullptr);
    NativePluginHost::instance().shutdownAll();
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

    // 2. средний ряд: разделитель закладок (110..220) + колонки Miller;
    // левую панель можно сжать, при нехватке места она сжимается сама (см. resizeEvent)
    m_middleSplitter = new QSplitter(Qt::Horizontal, this);
    m_middleSplitter->setHandleWidth(2);
    m_middleSplitter->setChildrenCollapsible(false);
    m_middleSplitter->setOpaqueResize(true);

    // левая панель закладок (доступна всегда, постоянная)
    m_bookmarksBar = new BookmarksBar(m_middleSplitter);
    m_bookmarksBar->show(); // доступна всегда
    m_bookmarksBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    // минимумы адаптивные (см. resizeEvent): в узком окне (<400px) снижаются до 48/140,
    // иначе сумма минимумов (302px) превышала ширину окна и рукоятка не двигалась
    m_bookmarksBar->setMinimumWidth(100);
    m_bookmarksBar->setMaximumWidth(380);

    // движок Miller из трёх колонок
    m_millerView = new MillerViewContainer(m_middleSplitter);
    // минимум адаптивный (resizeEvent): в узком окне снижается до 140, чтобы
    // рукоятка bookmarks сохраняла реальный ход
    m_millerView->setMinimumWidth(200);

    m_middleSplitter->addWidget(m_bookmarksBar);
    m_middleSplitter->addWidget(m_millerView);
    m_middleSplitter->setStretchFactor(0, 0);
    m_middleSplitter->setStretchFactor(1, 1);
    m_middleSplitter->setSizes({Theme::uiConfig().sidebarWidth, std::max(200, width() - Theme::uiConfig().sidebarWidth)});

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

    // свойства (ПКМ→Properties, Alt+Enter, закладки) оверлей поверх всего окна:
    // «как внешнее окно», но часть одного окна; для корня диска/ФС внутри
    // автоматически раскрывается консоль Disk Usage
    setupPropertiesOverlay();
    connect(m_millerView, &MillerViewContainer::propertiesRequested, this, &MainWindow::showPropertiesOverlay);

    // Open With… полноразмерный оверлей диалога (не подменю и не окно)
    setupOpenWithOverlay();

    // «Open In»: выбор файла приложения в собственном дизайне TUI поверх
    // Open With (системный QFileDialog не используется)
    setupFileChooserOverlay();
    connect(m_millerView, &MillerViewContainer::openWithDialogRequested, this, &MainWindow::showOpenWithOverlay);
    // Extract In…: меню просит выбор каталога, открываем тот же оверлей
    connect(m_millerView, &MillerViewContainer::extractInRequested, this, &MainWindow::showExtractChooserOverlay);
    connect(m_millerView, &MillerViewContainer::extractInPathRequested, this,
            &MainWindow::showExtractChooserOverlayFor);

    // Cheat Sheet (F1/?) внутренний оверлей «как Disk Usage» с живым переназначением клавиш
    setupHelpOverlay();

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
    // пункты меню закладок проходят через оверлей-диспетчер как одноимённые
    // действия Miller (copy_path, remove_bookmark, clear_trash, properties)
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
    // свойства из закладок/дисков тот же оверлей, что и ПКМ→Properties
    connect(m_bookmarksBar, &BookmarksBar::propertiesRequested, this, &MainWindow::showPropertiesOverlay);
    connect(m_millerView, &MillerViewContainer::itemSelected, this, [this](const std::filesystem::path& p) {
        if (m_tabBar && m_tabBar->currentIndex() >= 0 && m_tabBar->currentIndex() < m_tabBar->count()) {
            m_tabBar->currentTabState().selectedItem = p;
        }
    });

    // подключения Miller View
    connect(m_millerView, &MillerViewContainer::directoryChanged, this, &MainWindow::onDirectoryChanged);
    connect(m_millerView, &MillerViewContainer::statusUpdated, this, &MainWindow::onStatusUpdated);
    connect(m_millerView, &MillerViewContainer::messageEmitted, this, [this](const QString& msg) {
        m_statusBar->setProgressMessage(msg);
    });
    // плагины C++: notify() и refresh() из host API
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

    // аппаратные боковые кнопки мыши из Miller View (override как у go_back/go_forward)
    connect(m_millerView, &MillerViewContainer::navigateHistoryBackRequested, this, [this]() {
        if (m_millerView->tryPluginOverride(QStringLiteral("go_back"))) return;
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoBack()) {
            tab.goBack();
            m_millerView->setDirectory(tab.currentPath);
        }
    });
    connect(m_millerView, &MillerViewContainer::navigateHistoryForwardRequested, this, [this]() {
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
    connect(m_statusBar, &TuiStatusBar::filterChanged, this, &MainWindow::onFilterChanged);
    connect(m_statusBar, &TuiStatusBar::filterDismissed, this, [this]() {
        m_millerView->setFilter("");
    });
}

// оверлей свойств: «как внешнее окно», но часть этого же окна, плавающая панель
// с рамкой Accent по центру, перекрывает только часть колонок (как окно свойств
// Nautilus); без затемнений и анимаций, radius 0. Внутри PropertiesDialog во
// встроенном режиме; для корня диска/ФС он сам раскрывает консоль Disk Usage
// (единственный путь для ПКМ→Properties, Alt+Enter и закладок)
void MainWindow::setupPropertiesOverlay() {
    auto* central = centralWidget();
    if (!central) return;

    m_propsOverlay = new QWidget(central);
    m_propsOverlay->setObjectName(QStringLiteral("propsOverlay"));
    m_propsOverlay->setAttribute(Qt::WA_StyledBackground, true);
    // обводка в несколько пикселей, чтобы оверлей не сливался с окнами менеджера:
    // 2px рамка Accent и такие же отступы лэйаута, иначе содержимое перекрывает рамку
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

    // Close/Esc внутри диалога: скрыть панель, фокус назад списку файлов
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

    // по центру; для корня диска PropertiesDialog сам раскрывает консоль Disk Usage,
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
    // вызов из закладок или списка дисков не проходит через Miller, ещё одна точка override диспетчера
    if (!path.empty() && m_millerView->tryPluginOverrideWith(QStringLiteral("properties"), {path}, path))
        return;
    if (!m_propsOverlay || !m_propsPage || path.empty()) return;
    // порядок важен: setPath пересчитывает режим консоли Disk Usage (корень диска/ФС),
    // positionPropertiesOverlay знает итоговую геометрию
    m_propsPage->setPath(path);
    positionPropertiesOverlay();
    m_propsOverlay->show();
    m_propsOverlay->raise();
    m_propsPage->setFocus();
}

// Open With: та же механика оверлея, что и у свойств, содержимое другой диалог (поиск, DEFAULT APP / OTHER APPS, Always use)
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

    // «Choose application…»: свой выбор файла в стиле TUI вместо QFileDialog
    connect(m_openWithPage, &OpenWithDialog::browseRequested, this, &MainWindow::showFileChooserOverlay);

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
    if (!m_openWithOverlay || !m_openWithPage || path.empty()) return;
    m_openWithPage->setFile(path);
    positionOpenWithOverlay();
    m_openWithOverlay->show();
    m_openWithOverlay->raise();
    m_openWithPage->setFocus();
}

// «Open In»: свой диалог выбора файла в стиле TUI (сайдбар PLACES + список
// в стиле колонок Miller), открывается поверх Open With по «Choose application…»;
// Esc возвращает к списку приложений, выбор файла запускает его
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
        // возврат к списку приложений, из которого открыли выбор файла
        if (m_fileChooserForExtract) {
            if (m_millerView) m_millerView->setFocus();
        } else if (m_openWithPage && m_openWithOverlay && m_openWithOverlay->isVisible()) {
            m_openWithPage->setFocus();
        } else if (m_millerView) {
            m_millerView->setFocus();
        }
        m_fileChooserForExtract = false;
        m_extractArchiveOverride.clear();
    });
    // выбор подтверждён: приложение валидирует OpenWithDialog; каталог Extract In…
    // запускает распаковку выделенного архива в выбранную папку
    connect(m_fileChooserPage, &FileChooserDialog::fileChosen, this, [this](const QString& path) {
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
        if (!m_openWithPage) return;
        if (!m_openWithPage->handleChosenApplication(path.toUtf8().toStdString())) return;
        if (m_fileChooserOverlay) m_fileChooserOverlay->hide();
    });

    m_fileChooserOverlay->hide();
    positionFileChooserOverlay();
}

void MainWindow::positionFileChooserOverlay() {
    if (!m_fileChooserOverlay) return;
    auto* central = centralWidget();
    if (!central) return;

    // заметно крупнее Open With: сайдбар + широкий список файлов с
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

void MainWindow::showFileChooserOverlay() {
    if (!m_fileChooserOverlay || !m_fileChooserPage) return;
    // режим «выбор приложения» (Open With → Choose application…)
    m_fileChooserForExtract = false;
    m_extractArchiveOverride.clear();
    m_fileChooserPage->setDirectoryMode(false);
    m_fileChooserPage->setAcceptLabel(QStringLiteral("Open"));
    // старт каталог системных приложений .desktop (главная цель «open in»);
    // фолбэк домашний каталог
    std::filesystem::path start = "/usr/share/applications";
    std::error_code ec;
    if (!std::filesystem::is_directory(start, ec) || ec) {
        const char* home = std::getenv("HOME");
        start = home && *home ? home : "/";
    }
    m_fileChooserPage->setDirectory(start);
    positionFileChooserOverlay();
    m_fileChooserOverlay->show();
    m_fileChooserOverlay->raise();
    m_fileChooserPage->setFocus();
}

// Cheat Sheet: та же механика оверлея, что у свойств, панель с 2px рамкой
// Accent по центру поверх колонок (не отдельное окно и не страница превью);
// внутри таблица биндов HelpDialog, клик по клавише переназначает действие,
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
        if (m_helpOverlay) m_helpOverlay->hide();
        if (m_millerView) m_millerView->setFocus();
    });
    // живое применение переназначенных биндов (keybinds.conf пишет сам
    // HelpDialog через Core::Keybinds::saveUserBinding)
    connect(m_helpPage, &HelpDialog::keybindChanged, this, &MainWindow::reloadKeybinds);
    // захват клавиш: гасим все шорткаты биндов, иначе комбинация сработает
    // как действие вместо назначения; Esc/Backspace обрабатывает сам диалог
    connect(m_helpPage, &HelpDialog::captureChanged, this, [this](bool capturing) {
        if (capturing) {
            for (QShortcut* sc : std::as_const(m_keybindShortcuts)) {
                if (sc) sc->setEnabled(false);
            }
        } else {
            updateShortcutAvailability(QApplication::focusWidget());
        }
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

void MainWindow::showExtractChooserOverlay() {
    m_extractArchiveOverride.clear();
    if (!m_fileChooserOverlay || !m_fileChooserPage || !m_millerView) return;
    // Extract In…: тот же диалог «как open file», но в режиме каталогов
    // и с кнопкой Extract, старт текущая папка; выбор подтверждает обработчик
    // fileChosen (см. setupFileChooserOverlay), распаковка выделенного архива
    m_fileChooserForExtract = true;
    m_fileChooserPage->setDirectoryMode(true);
    m_fileChooserPage->setAcceptLabel(QStringLiteral("Extract"));
    m_fileChooserPage->setDirectory(m_millerView->currentDirectory());
    positionFileChooserOverlay();
    m_fileChooserOverlay->show();
    m_fileChooserOverlay->raise();
    m_fileChooserPage->setFocus();
}

void MainWindow::showExtractChooserOverlayFor(const std::filesystem::path& archivePath) {
    // Extract In… из папки превью: архив фиксирован, выставляем после
    // открытия окна (showExtractChooserOverlay сбрасывает override)
    showExtractChooserOverlay();
    if (m_fileChooserForExtract) m_extractArchiveOverride = archivePath;
}

void MainWindow::setupShortcuts() {
    createKeybindShortcuts();

    // печатные и «одноклавишные» шорткаты (g, ?, Del, Ctrl+C/V/X, F2…) срабатывают
    // до keyPressEvent текстового поля, поэтому при фокусе в текстовых полях их гасим;
    // alwaysActive=true (Esc, фокус на path/filter, Ctrl+Q) всегда активны
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget*, QWidget* now) { updateShortcutAvailability(now); });
    updateShortcutAvailability(QApplication::focusWidget());
}

void MainWindow::createKeybindShortcuts() {
    // клавиатурная карта из Core::Keybinds: QShortcut на каждую назначенную
    // клавишу, пустой список = действие отключено; повторный вызов пересоздаёт
    // весь набор (reloadKeybinds, живые переназначения из Cheat Sheet)
    for (QShortcut* sc : std::as_const(m_keybindShortcuts))
        delete sc;
    m_keybindShortcuts.clear();

    QList<QShortcut*> alwaysActive;
    auto bind = [this, &alwaysActive](const QString& action, std::function<void()> fn) {
        auto wrappedFn = [this, action, fn]() {
            if (m_millerView && m_millerView->tryPluginOverride(action)) return;
            fn();
        };
        const bool always = Core::Keybinds::isAlwaysActive(action);
        for (const QString& key : Core::Keybinds::keysFor(action)) {
            auto* sc = new QShortcut(QKeySequence(key), this);
            connect(sc, &QShortcut::activated, this, wrappedFn);
            if (always) alwaysActive.append(sc);
            m_keybindShortcuts.append(sc);
        }
    };

    // новая вкладка
    bind("new_tab", [this]() {
        const char* home = std::getenv("HOME");
        m_tabBar->addTab(home ? home : "/");
    });

    // закрыть вкладку (Ctrl+W / Ctrl+F4 по умолчанию)
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

    // переоткрыть закрытую вкладку (Chrome / VSCode / Zed / Explorer)
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

    // следующая вкладка (Ctrl+Tab / Ctrl+PgDown по умолчанию)
    bind("next_tab", [this]() {
        if (m_tabBar->count() > 0) {
            int next = (m_tabBar->currentIndex() + 1) % m_tabBar->count();
            m_tabBar->setCurrentIndex(next);
        }
    });

    // предыдущая вкладка (Ctrl+Shift+Tab / Ctrl+PgUp по умолчанию)
    bind("prev_tab", [this]() {
        if (m_tabBar->count() > 0) {
            int prev = (m_tabBar->currentIndex() - 1 + m_tabBar->count()) % m_tabBar->count();
            m_tabBar->setCurrentIndex(prev);
        }
    });

    // фокус на ввод пути (Ctrl+L / Alt+D / F4 по умолчанию)
    bind("focus_path", [this]() { m_statusBar->focusPathInput(); });

    // родительский каталог (Alt+Up по умолчанию)
    bind("parent_dir", [this]() {
        const auto& cur = m_millerView->currentDirectory();
        if (cur.has_parent_path() && cur != cur.parent_path()) {
            m_millerView->setDirectory(cur.parent_path());
        }
    });

    // назад по истории
    bind("go_back", [this]() {
        auto& tab = m_tabBar->currentTabState();
        if (tab.canGoBack()) {
            tab.goBack();
            m_millerView->setDirectory(tab.currentPath);
        }
    });

    // вперёд по истории
    bind("go_forward", [this]() {
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

    // переключить закладки / сайдбар (Ctrl+B / Ctrl+\ / F9 по умолчанию)
    bind("toggle_bookmarks", [this]() { toggleBookmarks(); });

    // быстрый переход
    bind("quick_jump", [this]() { showQuickJumpMenu(); });

    // обновить
    bind("refresh", [this]() { m_millerView->refresh(); });

    // поиск / фильтр
    bind("filter", [this]() { m_statusBar->activateFilter(); });

    // показать или скрыть скрытые файлы (Ctrl+H / Ctrl+. по умолчанию)
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

    // отмена: Escape снимает подсказку ввода / активную файловую операцию / оверлеи / выделение
    bind("cancel", [this]() {

        // Cheat Sheet (F1/?) самый «верхний» оверлей: Esc либо отменяет
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

        // оверлей вывода плагина (Plugins) транзиентный, закрывается первым
        const auto outputs = findChildren<TuiOutputOverlay*>(Qt::FindDirectChildrenOnly);
        for (int i = outputs.size() - 1; i >= 0; --i) {
            if (outputs[i]->isVisible()) {
                outputs[i]->hide();
                return;
            }
        }

        // «Open In» поверх Open With: Esc закрывает выбор файла и возвращает
        // к списку приложений (Extract In… просто к колонкам)
        if (m_fileChooserOverlay && m_fileChooserOverlay->isVisible()) {
            m_fileChooserOverlay->hide();
            const bool wasExtract = m_fileChooserForExtract;
            m_fileChooserForExtract = false;
            m_extractArchiveOverride.clear();
            if (!wasExtract && m_openWithPage) m_openWithPage->setFocus();
            return;
        }
        // оверлей Open With открыт: закрываем его, иначе QShortcut перехватит Esc до обработчика диалога
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
        if (m_statusBar && m_statusBar->isPromptActive()) {
            m_statusBar->cancelInlinePrompt();
            return;
        }
        if (Dnd::DragDropHandler::hasActiveOperation()) {
            Dnd::DragDropHandler::cancelActiveOperation();
            return;
        }
        m_millerView->clearSelection();
        // фильтр транзиентный: Esc снимает его вместе с редактором
        if (m_statusBar->isFilterActive()) {
            m_statusBar->clearFilter();
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

    // Redo (Ctrl+Y / Ctrl+Shift+Z дефолт; QKeySequence::Redo на Linux
    // резолвится ровно в Ctrl+Y, поэтому оба варианта в таблице)
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

    // свойства
    bind("properties", [this]() { m_millerView->showProperties(); });

    // диалог помощи
    bind("help", [this]() { showHelpDialog(); });

    // выход
    bind("quit", [this]() { close(); });

    // кастомные действия плагинов (Action/Keys в plugins/*.conf): как у пункта
    // меню Plugins, выделение центра + текущий каталог
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

    // состав textSensitive: как в setupShortcuts, печатные шорткаты уступают
    // ввод в текстовых полях, alwaysActive остаются; пересчитывается при каждом
    // пересоздании (после живого переназначения биндов из Cheat Sheet)
    m_textSensitiveShortcuts = findChildren<QShortcut*>();
    for (QShortcut* sc : std::as_const(alwaysActive))
        m_textSensitiveShortcuts.removeAll(sc);
}

// живое применение keybinds.conf: перечитать конфиг, пересоздать шорткаты,
// восстановить доступность по фокусу. Вызывается из Cheat Sheet после
// каждого rebind/reset/disable (сигнал keybindChanged)
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
    const bool typing = isTextInputWidget(focus);
    for (QShortcut* sc : std::as_const(m_textSensitiveShortcuts)) {
        if (sc) sc->setEnabled(!typing);
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    QMainWindow::keyPressEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (!m_initialSplitterSizesApplied && m_middleSplitter) {
        int avail = m_middleSplitter->width();
        if (avail >= 300) {
            m_initialSplitterSizesApplied = true;
            const int kHandle = m_middleSplitter->handleWidth();
            const int bookW = Theme::uiConfig().sidebarWidth;
            m_middleSplitter->setSizes({bookW, avail - bookW - kHandle});
        }
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    positionPropertiesOverlay();
    positionOpenWithOverlay();
    positionFileChooserOverlay();
    positionHelpOverlay();

    // оверлеи вывода плагинов (Plugins) держатся правого нижнего угла окна
    const auto outputOverlays = findChildren<TuiOutputOverlay*>(Qt::FindDirectChildrenOnly);
    for (TuiOutputOverlay* ov : outputOverlays) {
        if (ov && ov->isVisible()) ov->repositionWithinParent();
    }

    // компактная шапка 20px при высоте окна < 480, иначе высота EngineTabBar::kNormalHeight (30px)
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
    const int kBookPref = 160;
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
    } else if (m_bookmarksAutoHidden && !m_bookmarksBar->isVisible()) {
        m_bookmarksBar->show();
        m_bookmarksAutoHidden = false;
    }

    // задаём удобную начальную ширину (160px для панели закладок) при первой раскладке
    if (!m_initialSplitterSizesApplied && avail >= 300) {
        m_initialSplitterSizesApplied = true;
        m_middleSplitter->setSizes({kBookPref, avail - kBookPref - kHandle});
        return;
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
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::BackButton) {
            if (m_millerView->tryPluginOverride(QStringLiteral("go_back"))) return true;
            auto& tab = m_tabBar->currentTabState();
            if (tab.canGoBack()) {
                tab.goBack();
                m_millerView->setDirectory(tab.currentPath);
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
    if (m_bookmarksBar) {
        m_bookmarksAutoHidden = false;
        m_bookmarksBar->setVisible(!m_bookmarksBar->isVisible());
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
        auto sel = m_millerView->selectedPaths();
        tab.selectedPaths.clear();
        if (!sel.empty()) {
            tab.selectedItem = sel.front();
            for (const auto& p : sel) {
                tab.selectedPaths.insert(p.string());
            }
        } else {
            tab.selectedItem.clear();
        }
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

    if (!curTab.selectedPaths.empty()) {
        m_millerView->restoreMultiSelection(curTab.selectedPaths, curTab.selectedItem);
    } else if (!curTab.selectedItem.empty()) {
        m_millerView->selectPathAfterRefresh(curTab.selectedItem);
    }

    if (m_millerView->currentDirectory() != curTab.currentPath) {
        m_millerView->setDirectory(curTab.currentPath);
    } else if (!curTab.selectedItem.empty()) {
        m_millerView->selectPathAfterRefresh(curTab.selectedItem);
        m_millerView->refresh();
    }

    if (!curTab.selectedPaths.empty()) {
        m_millerView->restoreMultiSelection(curTab.selectedPaths, curTab.selectedItem);
    }

    if (m_bookmarksBar) {
        m_bookmarksBar->setActiveDirectory(curTab.currentPath);
    }
    m_lastTabIndex = index;
}

void MainWindow::onTabChanged(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    saveCurrentTabState();
    restoreTabState(index);
}

void MainWindow::onTabCloseRequested(int index) {
    // крестик/средняя кнопка на вкладке тоже override-точка (как Ctrl+W)
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
        if (tab.currentPath != path) {
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

void MainWindow::onFilterChanged(const QString& filter) {
    m_millerView->setFilter(filter);
}

}
