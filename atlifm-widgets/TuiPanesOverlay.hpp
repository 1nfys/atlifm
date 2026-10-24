#pragma once

#include <QWidget>
#include <QByteArray>
#include <QList>
#include <QString>

class QEvent;
class QKeyEvent;
class QPlainTextEdit;
class QSplitter;

namespace Widgets {

class TuiPanesOverlay : public QWidget {
    Q_OBJECT
public:
    explicit TuiPanesOverlay(QWidget* parent);

    void begin();
    void appendOutput(const QByteArray& chunk);
    void appendPane(int pane, const QByteArray& chunk);
    void finish(int exitCode);
    void closeOverlay();
    void reset();
    void setPaneSizes(const QList<int>& sizes);
    void focusCenterPane();

    bool isRunning() const { return m_running; }
    QString paneText(int pane) const;

signals:
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void appendToPane(int pane, const QByteArray& chunk);
    void flushPending();
    static bool parseMarker(const QByteArray& line, int* pane);

    QSplitter* m_splitter{nullptr};
    QPlainTextEdit* m_views[3]{};
    bool m_running{false};
    int m_currentPane{1};
    QByteArray m_pending;
};

}
