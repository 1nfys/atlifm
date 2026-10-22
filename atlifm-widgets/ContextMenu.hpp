#pragma once

#include <QMenu>
#include <vector>
#include <filesystem>

namespace Widgets {

class TuiContextMenu : public QMenu {
    Q_OBJECT

public:
    explicit TuiContextMenu(const std::vector<std::filesystem::path>& selectedPaths,
                           const std::filesystem::path& currentDir,
                           QWidget* parent = nullptr);

signals:
    void openRequested();
    void openInNewTabRequested();
    void openWithAppRequested(const QString& execCmd);
    // «Open With...» открывает полноразмерный диалог выбора приложения в центре окна, а не подменю
    void openWithRequested();
    void clearTrashRequested();
    void cutRequested();
    void copyRequested();
    void pasteRequested();
    void copyPathRequested(int mode); // 0 = абсолютный путь, 1 = имя файла, 2 = относительный путь
    void renameRequested();
    void trashRequested();
    void deletePermanentlyRequested();
    void compressRequested(const QString& format = "");
    // Extract Here сразу в текущий каталог, Extract In с выбором каталога через FileChooserDialog
    void extractHereRequested();
    void extractInRequested();
    void openTerminalRequested();
    void newFolderRequested();
    void newFileRequested();
    void bookmarkRequested();
    void propertiesRequested();
    void sortModeRequested(int mode); // 0 = имя, 1 = размер, 2 = дата, 3 = тип
    // сторонний плагин из подменю Plugins, id это имя файла конфига без .conf (см. Core::Plugins)
    void pluginRequested(const QString& pluginId);
    // действие плагина C++ (NativePluginHost, *.so)
    void nativeActionRequested(const QString& actionId);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void populateMenu();
    void addPluginsSubmenu();

    std::vector<std::filesystem::path> m_selectedPaths;
    std::filesystem::path m_currentDir;
};

}
