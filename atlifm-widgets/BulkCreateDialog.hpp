#pragma once

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLabel;
class QPlainTextEdit;

namespace Widgets {

// многострочное создание где пустые строки пропускаем а конфликты решает вызывающий код без перезаписи без подтверждения
class BulkCreateDialog : public QDialog {
    Q_OBJECT

public:
    explicit BulkCreateDialog(QWidget* parent = nullptr);

    bool folders() const;
    QStringList names() const { return m_names; }

protected:
    void accept() override;

private:
    QComboBox* m_kind{nullptr};
    QPlainTextEdit* m_input{nullptr};
    QLabel* m_error{nullptr};
    QStringList m_names;
};

} // namespace Widgets
