#pragma once

// оверлей списка фоновых задач с прогрессом скоростью отменой по del и логом по enter
// геометрия в правом нижнем углу без анимаций и скруглений

#include <QWidget>
#include <QVector>
#include "../atlifm-core/tasks/TaskManager.hpp"

class QPlainTextEdit;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

namespace Widgets {

class TasksOverlay : public QWidget {
    Q_OBJECT
public:
    explicit TasksOverlay(QWidget* parent);

    void refresh();
    void repositionWithinParent();
    bool isLogMode() const { return m_logMode; }

signals:
    void closeRequested();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QRect headerRect() const;
    QRect bodyRect() const;
    QRect footerRect() const;
    int rowAt(int y) const;
    void moveSelection(int delta);
    void ensureSelectionVisible();
    int visibleRows() const;
    void showLog(Core::Tasks::TaskId id);
    void leaveLog();
    void updateLogView();

    QVector<Core::Tasks::TaskInfo> m_tasks;
    Core::Tasks::TaskId m_selectedId{0};
    Core::Tasks::TaskId m_logTask{0};
    bool m_logMode{false};
    int m_scroll{0};

    QPlainTextEdit* m_logView{nullptr};
};

} // namespace Widgets
