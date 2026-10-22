#pragma once

#include <QWidget>
#include <QString>
#include <QByteArray>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QKeyEvent;

namespace Widgets {

// три текстовые панели поверх колонок миллера для вывода команды плагина
// внешние плагины разделяют вывод строками @@pane:0 и @@pane:1 и @@pane:2
// нативные пишут напрямую через output_write_pane а esc или close скрывают
class TuiPanesOverlay : public QWidget {
    Q_OBJECT
public:
    explicit TuiPanesOverlay(QWidget* parent);

    void begin(const QString& title);
    void appendOutput(const QByteArray& chunk); // внешний вывод со строками @@pane:N
    void appendPane(int pane, const QByteArray& chunk);
    void finish(int exitCode);
    void closeOverlay();

    bool isRunning() const { return m_running; }
    QString paneText(int pane) const; // чтение текста панели

    // растягивается на всего родителя в область трех колонок
    void repositionWithinParent();

signals:
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void appendToPane(int pane, const QByteArray& chunk);
    void flushPending();
    static bool parseMarker(const QByteArray& line, int* pane);

    QLabel* m_title{nullptr};
    QPlainTextEdit* m_views[3]{};
    QPushButton* m_close{nullptr};
    bool m_running{false};
    int m_currentPane{1};
    // хвост неполной строки потому что чанки процесса могут рвать utf8 и строки
    QByteArray m_pending;
};

}
