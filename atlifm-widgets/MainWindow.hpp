#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QList>
#include <QHash>
#include <QStringList>
#include <QByteArray>
#include <QTimer>
#include <memory>
#include <filesystem>
#include <vector>
#include "TabBar.hpp"
#include "BookmarksBar.hpp"
#include "MillerColumns.hpp"
#include "StatusBar.hpp"
#include "../atlifm-core/Network.hpp"

class QShortcut;

namespace Core::Network {
class NetworkMount;
}

namespace Widgets {

class PropertiesDialog;
class OpenWithDialog;
class FileChooserDialog;
class HelpDialog;
class TasksOverlay;
class BulkRenamePanel;
class ConnectionDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const std::filesystem::path& initialPath = "", QWidget* parent = nullptr);
    ~MainWindow() override;

    void applySystemTheme();

    // оверлей свойств: «как внешнее окно», но часть этого же окна, панель с
    // рамкой Accent поверх остальных элементов. Без затемнений и анимаций
    // (гайдлайн §3), Esc/Close скрывают. Компактные свойства (ПКМ→Properties,
    // Alt+Enter, закладки) показывают только вес папки; для корня диска/ФС
    // в том же оверлее автоматически раскрывается консоль Disk Usage
    void showPropertiesOverlay(const std::filesystem::path& path);

    // Open With: полноразмерный оверлей диалога (поиск, Default/Other apps,
    // «Always use for this file type») вместо подменю
    void showOpenWithOverlay(const std::filesystem::path& path);

    // Extract In…: тот же оверлей FileChooserDialog, но в режиме каталогов
    // и с кнопкой Extract, выбор папки назначения для распаковки архива
    void showExtractChooserOverlay();
    void showExtractChooserOverlayFor(const std::filesystem::path& archivePath);

    // кастомный вывод команды плагина в три панели поверх колонок
    void beginPluginPanes(const QString& title);
    void appendPluginPanesOutput(const QByteArray& data);
    void appendPluginPaneOutput(int pane, const QByteArray& data);
    void finishPluginPanes(int exitCode);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onNewTabRequested(const std::filesystem::path& path);
    void onDuplicateTabRequested(int index);
    void onPinTabRequested(int index);
    void onCloseOtherTabsRequested(int index);

    void onDirectoryChanged(const std::filesystem::path& path);
    void onStatusUpdated(int currentIndex, int totalItems, int selectedItems, const QString& perms);
    void onFilterChanged(const QString& filter);
    void onFilterCommandChanged(const QString& token, const QString& query);
    void onFilterSubmitted();
    void toggleBookmarks();
    void showQuickJumpMenu();
    void showHelpDialog();
    void toggleTasksOverlay();

private:
    struct FilterPluginCommand {
        QString pluginId;
        QString actionId;
        QString title;
        QString mode;
        bool native{false};
    };

    void setupUi();
    void refreshFilterCommands();
    void runFilterPluginCommand(const FilterPluginCommand& command, const QString& query);
    void setupShortcuts();
    // пересоздание QShortcut'ов по Core::Keybinds (живое применение после
    // переназначения клавиш в Cheat Sheet); setupShortcuts зовёт её один раз
    // и вешает focusChanged, при reloadKeybinds() пересоздание на месте
    void createKeybindShortcuts();
    void reloadKeybinds();
    void updateShortcutAvailability(QWidget* focus);
    void setupPropertiesOverlay();
    void positionPropertiesOverlay();
    void setupOpenWithOverlay();
    void positionOpenWithOverlay();
    void setupFileChooserOverlay();
    void positionFileChooserOverlay();
    void showSshKeyChooserOverlay(const QString& startPath);
    void setupHelpOverlay();
    void positionHelpOverlay();
    void setupTasksOverlay();
    void positionTasksOverlay();
    void setupConnectionOverlay();
    void positionConnectionOverlay();
    void showConnectionOverlay(const QString& location);
    void connectNetwork(const Core::Network::Location& location);
    // массовое переименование: встроенная панель поверх колонок в этой же
    // рамке Accent, без отдельного окна и без затемнения
    void setupBulkRenameOverlay();
    void positionBulkRenameOverlay();
    void showBulkRenameOverlay(const std::vector<std::filesystem::path>& paths);
    void hideBulkRenameOverlay(bool restoreFocus = true);
    void updateTaskSummary();
    // раскладка: layout.ini перекрывает theme.conf и удаляется по reset
    void loadSavedLayout();
    void saveLayout();
    void resetLayout();
    QString layoutFilePath() const;
    int preferredSidebarWidth() const;
    void saveCurrentTabState();
    void restoreTabState(int index);

    EngineTabBar* m_tabBar{nullptr};
    QSplitter* m_middleSplitter{nullptr};
    BookmarksBar* m_bookmarksBar{nullptr};
    MillerViewContainer* m_millerView{nullptr};
    TuiStatusBar* m_statusBar{nullptr};

    QWidget* m_propsOverlay{nullptr};      // оверлей панели с рамкой
    PropertiesDialog* m_propsPage{nullptr}; // свойства; корень диска с консолью

    QWidget* m_openWithOverlay{nullptr};   // панель Open With (та же геометрия)
    OpenWithDialog* m_openWithPage{nullptr};

    // выбор файла в стиле tui вместо системного qfiledialog сайдбар мест и список файлов с иконками для extract in и выбора ssh ключа
    QWidget* m_fileChooserOverlay{nullptr};
    FileChooserDialog* m_fileChooserPage{nullptr};
    // назначение оверлея: true выбор каталога для Extract In…
    bool m_fileChooserForExtract{false};
    bool m_fileChooserForSshKey{false};
    // архив для Extract In… из папки превью (выделение колонок при этом пустое)
    std::filesystem::path m_extractArchiveOverride;

    // Cheat Sheet (F1/?) оверлей поверх интерфейса «как Disk Usage»:
    // внутри можно переназначить клавиши (пишет keybinds.conf и применяет
    // бинды на лету через reloadKeybinds)
    QWidget* m_helpOverlay{nullptr};
    HelpDialog* m_helpPage{nullptr};

    QWidget* m_bulkRenameOverlay{nullptr};
    BulkRenamePanel* m_bulkRenamePage{nullptr};

    // фоновые задачи (TaskManager): список с прогрессом, отменой и логом
    TasksOverlay* m_tasksOverlay{nullptr};
    ConnectionDialog* m_connectionDialog{nullptr};
    Core::Network::NetworkMount* m_networkMount{nullptr};
    QHash<QString, FilterPluginCommand> m_filterPluginCommands;
    QTimer* m_filterSearchTimer{nullptr};
    QString m_pendingSearchToken;
    QString m_pendingSearchQuery;
    bool m_keepGlobalResultsOnDismiss{false};
    // все QShortcut'ы, созданные createKeybindShortcuts (для пересоздания и
    // временного отключения на время захвата клавиш в Cheat Sheet)
    QList<QShortcut*> m_keybindShortcuts;

    bool m_bookmarksAutoHidden{false};
    bool m_bookmarksEnabled{true};
    bool m_initialSplitterSizesApplied{false};
    // минус один означает брать sidebar_width из конфига иначе сохраненный layout.ini
    int m_sidebarWidth{-1};
    int m_lastTabIndex{0};
    std::vector<std::filesystem::path> m_closedTabsHistory;
    // шорткаты, которые обязаны уступать ввод в текстовые поля этого же окна
    // (filter/path/prompt статусбара, оверлеи) см. updateShortcutAvailability
    QList<QShortcut*> m_textSensitiveShortcuts;
    // глобальные переходы к path/filter: внутри текстового поля они полезны,
    // но при открытом оверлее или prompt не должны срабатывать поверх него
    QList<QShortcut*> m_contextSensitiveShortcuts;
};

}
