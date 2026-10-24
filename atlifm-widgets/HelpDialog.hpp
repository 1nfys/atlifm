#pragma once

// шпаргалка: оверлей поверх интерфейса, а не отдельное окно
// из core::keybinds::actiontable() и плагинов, бинды переназначаются на месте,
// пкм reset/disable; на время захвата mainwindow гасит свои qshortcut'ы (capturechanged)

#include <QWidget>
#include <QString>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace Widgets {

class HelpDialog : public QWidget {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget* parent = nullptr);

    // пересобрать таблицу (после reload конфига, смены плагинов)
    void rebuild();
    bool isCapturing() const { return !m_captureAction.isEmpty(); }
    // «new_tab» и человекочитаемое описание действия
    static QString actionLabel(const QString& actionId);

public slots:
    void beginCapture(const QString& actionId);
    void cancelCapture();
    void resetBinding(const QString& actionId);
    void disableBinding(const QString& actionId);

signals:
    void closeRequested();
    void keybindChanged();
    void captureChanged(bool capturing);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void addSectionHeader(const QString& title);
    void addStaticRow(const QString& text);
    void addActionRow(const QString& actionId, const QString& label, int keyColumnWidth);
    QString keyDisplay(const QStringList& keys) const;
    // id действия, уже занимающего key (кроме actionid): qt глушит неоднозначные qshortcut и оба молчат
    QString conflictForAction(const QString& actionId, const QString& key) const;
    void saveAndFinish(const QStringList& keys, const QString& status);
    void finishCapture(const QString& status);

    QScrollArea* m_scroll{nullptr};
    QWidget* m_content{nullptr};
    QVBoxLayout* m_contentLayout{nullptr};
    QLabel* m_captureHint{nullptr};
    QLabel* m_status{nullptr};
    QString m_captureAction;
};

}
