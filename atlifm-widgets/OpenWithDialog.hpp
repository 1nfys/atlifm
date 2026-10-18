#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <filesystem>
#include <vector>
#include "../atlifm-core/XdgUtils.hpp"

namespace Widgets {

// Open With полноразмерный диалог в стиле TUI (не подменю, не отдельное окно):
// поиск по приложениям, секции DEFAULT APP / OTHER APPS, «Always use for this file type», кнопки Cancel/Open
// живёт оверлеем в центре MainWindow и запускает файл сам
class OpenWithDialog : public QWidget {
    Q_OBJECT

public:
    explicit OpenWithDialog(QWidget* parent = nullptr);

    // заполняет диалог приложениями для файла, асинхронность не нужна: чтение каталогов .desktop быстрое
    void setFile(const std::filesystem::path& path);
    void updateTheme();

    // *.desktop (разбор Desktop Entry) или исполняемый файл запускаются напрямую,
    // иначе TUI алерт и false; вызывается MainWindow по сигналу FileChooserDialog::fileChosen
    bool handleChosenApplication(const std::filesystem::path& path);

signals:
    void closeRequested();
    // «Choose application…»: MainWindow открывает свой FileChooserDialog вместо системного QFileDialog
    void browseRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct AppRow {
        QWidget* widget{nullptr};
        Core::DesktopApp app;
        bool isDefault{false};
        bool isCustom{false}; // «Choose application…» браузер вместо запуска
    };

    void setupUi();
    void rebuildList();
    void addRow(const Core::DesktopApp& app, bool isDefault, bool custom = false);
    void setSelected(int index);
    void moveSelection(int delta);
    void activateSelected();
    void launch(const Core::DesktopApp& app);

    std::filesystem::path m_path;
    std::vector<Core::DesktopApp> m_apps;
    Core::DesktopApp m_defaultApp;

    QLabel* m_headerLabel{nullptr};
    QLabel* m_fileLabel{nullptr};
    QLineEdit* m_searchEdit{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget* m_listContainer{nullptr};
    QVBoxLayout* m_listLayout{nullptr};
    QCheckBox* m_alwaysUse{nullptr};
    QPushButton* m_cancelBtn{nullptr};
    QPushButton* m_openBtn{nullptr};

    std::vector<AppRow> m_rows;
    int m_selected{-1};
};

}
