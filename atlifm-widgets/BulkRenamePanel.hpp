#pragma once

#include <QVector>
#include <QWidget>
#include <filesystem>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace Widgets {

// встроенная панель массового переименования поверх колонок в рамке accent без затемнения и файлы не меняются до нажатия rename
class BulkRenamePanel : public QWidget {
    Q_OBJECT

public:
    explicit BulkRenamePanel(QWidget* parent = nullptr);

    void setPaths(const std::vector<std::filesystem::path>& paths);
    void focusFirstEdit();

    const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& renames() const {
        return m_renames;
    }

signals:
    void accepted();
    void cancelled();
    void statusMessage(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuildRows();
    void apply();
    void updateStatus();
    void focusEdit(int index);
    void showError(const QString& message, int row = -1);
    void clearError();

    QScrollArea* m_scroll{nullptr};
    QWidget* m_rows{nullptr};
    QVBoxLayout* m_rowsLayout{nullptr};
    QLabel* m_title{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_error{nullptr};
    QVector<QLineEdit*> m_edits;
    std::vector<std::filesystem::path> m_paths;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> m_renames;
};

} // namespace Widgets
