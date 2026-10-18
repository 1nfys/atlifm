#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPoint>

namespace Widgets {

class TuiInputPopup : public QDialog {
    Q_OBJECT
public:
    explicit TuiInputPopup(const QString& title, const QString& labelText,
                           const QString& initialText, QWidget* parent = nullptr,
                           bool selectStemOnly = false, bool isPassword = false);

    QString text() const;

    static QString getText(QWidget* parent, const QString& title, const QString& labelText,
                           const QString& initialText = QString(), bool* ok = nullptr,
                           const QPoint& targetPos = QPoint(), bool selectStemOnly = false, bool isPassword = false);

    int exec() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QLineEdit* m_lineEdit{nullptr};
    QPushButton* m_btnOk{nullptr};
    QPushButton* m_btnCancel{nullptr};
};

class TuiConfirmPopup : public QDialog {
    Q_OBJECT
public:
    explicit TuiConfirmPopup(const QString& title, const QString& message,
                             const QString& confirmText = "Confirm",
                             const QString& cancelText = "Cancel",
                             bool isDanger = false,
                             QWidget* parent = nullptr);

    static bool ask(QWidget* parent, const QString& title, const QString& message,
                    const QString& confirmText = "Confirm",
                    const QString& cancelText = "Cancel",
                    bool isDanger = false);

    static void alert(QWidget* parent, const QString& title, const QString& message);

    int exec() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QPushButton* m_btnConfirm{nullptr};
    QPushButton* m_btnCancel{nullptr};
};

}
