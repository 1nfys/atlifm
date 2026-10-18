#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCompleter>
#include <QFileSystemModel>
#include <QTimer>
#include <QPointer>
#include <QEventLoop>
#include <filesystem>

class QHBoxLayout;

namespace Widgets {

class TuiStatusBar : public QWidget {
    Q_OBJECT

public:
    explicit TuiStatusBar(QWidget* parent = nullptr);

    void setPath(const std::filesystem::path& path);
    void setItemStats(int currentIndex, int totalItems, int selectedItems);
    // права текущего элемента в статусбаре в стиле yazi, в строках списка их нет (FileItemDelegate)
    void setItemPerms(const QString& perms);
    // скрыть счётчик файлов, в open files вместо него кнопки cancel/open (см. addTrailingWidget)
    void setItemStatsVisible(bool visible);
    // виджет в конец строки статусбара, правее всех бейджей
    void addTrailingWidget(QWidget* widget);
    void setMountStats(const std::filesystem::path& path);
    void setFilterBuffer(const QString& filterText, bool isEditing = false);
    void setProgressMessage(const QString& msg, bool isFinished = true);
    void setSudoBadge(bool visible);
    bool isSudoBadgeVisible() const;
    void updateTheme();

    bool isFilterActive() const { return m_isFilterActive; }
    void activateFilter();
    void activateFilterWithText(const QString& text);
    // enter/esc в поле фильтра: редактор скрывается, фильтр снимается (транзиентный, без сохранения по папкам)
    void deactivateFilter();
    void clearFilter() { deactivateFilter(); }

    void focusPathInput();
    bool isPromptActive() const { return m_promptActive; }
    void cancelInlinePrompt() { finishInlinePrompt(0); }

    // режим одно окно: ввод, подтверждение и уведомление прямо в строке статусбара,
    // отдельных окон нет, вызов блокируется локальным QEventLoop (как модальный)
    QString askTextInline(const QString& label, const QString& initial,
                          bool selectStemOnly, bool* ok, bool isPassword = false);
    bool askConfirmInline(const QString& title, const QString& message,
                          const QString& confirmText, const QString& cancelText,
                          bool isDanger);
    void showAlertInline(const QString& title, const QString& message);

signals:
    void pathEntered(const std::filesystem::path& path);
    void filterChanged(const QString& filter);
    void filterDismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onPathInputReturnPressed();

private:
    void setupUi();

    QLabel* m_badgeLabel{nullptr};
    QLineEdit* m_pathInput{nullptr};
    QLabel* m_pathSuffixLabel{nullptr};
    QCompleter* m_pathCompleter{nullptr};
    QFileSystemModel* m_completerModel{nullptr};

    QLabel* m_sudoBadgeLabel{nullptr};
    QLabel* m_statusMessageLabel{nullptr};
    QLineEdit* m_filterInput{nullptr};
    QLabel* m_permsLabel{nullptr};
    QLabel* m_statsLabel{nullptr};
    QLabel* m_diskLabel{nullptr};

    // инлайн промпт (скрыт, пока не нужен)
    QLabel* m_promptLabel{nullptr};
    QLineEdit* m_promptInput{nullptr};
    QPushButton* m_promptOkBtn{nullptr};
    QPushButton* m_promptCancelBtn{nullptr};
    QEventLoop* m_promptLoop{nullptr};
    int m_promptResult{0}; // 0 = отклонено, 1 = принято
    bool m_promptActive{false};
    bool m_promptWasFilterVisible{false};
    bool m_promptHidesPath{false};
    QPointer<QWidget> m_prevFocusWidget;

    void beginInlinePrompt(bool hidePath);
    void endInlinePrompt();
    void finishInlinePrompt(int res);
    // автоскрытие бейджа сообщения через 1.5 с, единый путь для показа
    // и повтора после инлайн промпта, который останавливал таймер
    void scheduleMessageDismiss();

    std::filesystem::path m_currentPath;
    bool m_isFilterActive{false};
    bool m_statsEnabled{true};
    QTimer* m_messageTimer{nullptr};
    QHBoxLayout* m_layout{nullptr};
};

}
