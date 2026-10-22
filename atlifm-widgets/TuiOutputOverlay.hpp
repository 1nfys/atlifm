#pragma once

#include <QWidget>
#include <QString>
#include <QByteArray>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace Widgets {

// оверлей вывода внешнего плагина в правом нижнем углу окна, рамка accent 2px
// фокус не забирает, колонки остаются рабочими, пока плагин печатает
// esc (глобальный cancel в MainWindow) или close скрывают, процесс не убивается
class TuiOutputOverlay : public QWidget {
    Q_OBJECT
public:
    explicit TuiOutputOverlay(QWidget* parent);

    void begin(const QString& title);           // показать и начать сессию
    void appendOutput(const QByteArray& chunk);  // дописать вывод (потоково)
    void finish(int exitCode);                   // сессия завершена

    bool isRunning() const { return m_running; }
    QString outputText() const;                  // для тестов

    // прижимается к правому нижнему углу родителя, вызывается при показе и ресайзе
    void repositionWithinParent();

private:
    QLabel* m_title{nullptr};
    QPlainTextEdit* m_view{nullptr};
    QPushButton* m_close{nullptr};
    bool m_running{false};
    // хвост неполной строки: чанки процесса могут рвать utf8 и строки
    QByteArray m_pending;
};

}
