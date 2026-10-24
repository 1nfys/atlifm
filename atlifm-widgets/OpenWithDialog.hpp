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

// open with полноразмерный диалог в стиле tui (не подменю, не отдельное окно):
// поиск по приложениям, секции default app / other apps, «always use for this file type», кнопки cancel/open
// живёт оверлеем в центре mainwindow и запускает файл сам
class OpenWithDialog : public QWidget {
    Q_OBJECT

public:
    explicit OpenWithDialog(QWidget* parent = nullptr);

    // заполняет диалог приложениями для файла, асинхронность не нужна: чтение каталогов .desktop быстрое
    void setFile(const std::filesystem::path& path);
    void updateTheme();

signals:
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct AppRow {
        QWidget* widget{nullptr};
        Core::DesktopApp app;
        bool isDefault{false};
    };

    void setupUi();
    void rebuildList();
    void addRow(const Core::DesktopApp& app, bool isDefault);
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
