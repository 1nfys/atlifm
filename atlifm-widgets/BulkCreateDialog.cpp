#include "BulkCreateDialog.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSet>
#include <QVBoxLayout>

namespace Widgets {

namespace {

QString validateName(const QString& name) {
    if (name.contains(QLatin1Char('/'))) return QStringLiteral("Name cannot contain '/'");
    if (name.contains(QChar(u'\0'))) return QStringLiteral("Name contains an invalid character");
    if (name == QLatin1String(".") || name == QLatin1String("..")) return QStringLiteral("Name is reserved");
    return {};
}

} // namespace

BulkCreateDialog::BulkCreateDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Multiple Items"));
    setModal(true);
    setMinimumSize(560, 400);
    setStyleSheet(Theme::getApplicationStyleSheet());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("Create multiple files / folders"), this);
    title->setFont(Theme::getMonospaceFont(10, QFont::Bold));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Accent().name()));
    layout->addWidget(title);

    auto* kindRow = new QHBoxLayout();
    auto* kindLabel = new QLabel(QStringLiteral("Type:"), this);
    kindLabel->setFont(Theme::getMonospaceFont(9));
    m_kind = new QComboBox(this);
    m_kind->addItems({QStringLiteral("Files"), QStringLiteral("Folders")});
    m_kind->setFont(Theme::getMonospaceFont(9));
    kindRow->addWidget(kindLabel);
    kindRow->addWidget(m_kind);
    kindRow->addStretch(1);
    layout->addLayout(kindRow);

    auto* hint = new QLabel(QStringLiteral("One name per line. Empty lines are ignored. '/' and '.'/'..' are rejected."), this);
    hint->setFont(Theme::getMonospaceFont(8));
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Dim().name()));
    layout->addWidget(hint);

    m_input = new QPlainTextEdit(this);
    m_input->setFont(Theme::getMonospaceFont(9));
    m_input->setPlaceholderText(QStringLiteral("notes.txt\nphoto.jpg\nnew folder"));
    m_input->setStyleSheet(QString(
        "QPlainTextEdit { background-color: %1; color: %2; border: 1px solid %3; padding: 6px; }")
        .arg(Theme::Colors::Base().name(), Theme::Colors::Text().name(),
             Theme::Colors::Surface1().name()));
    layout->addWidget(m_input, 1);

    m_error = new QLabel(this);
    m_error->setFont(Theme::getMonospaceFont(8));
    m_error->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Red().name()));
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    auto* create = new QPushButton(QStringLiteral("Create"), this);
    cancel->setFont(Theme::getMonospaceFont(9));
    create->setFont(Theme::getMonospaceFont(9));
    create->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(create, &QPushButton::clicked, this, &BulkCreateDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(create);
    layout->addLayout(buttons);

    m_input->setFocus();
}

bool BulkCreateDialog::folders() const {
    return m_kind->currentIndex() == 1;
}

void BulkCreateDialog::accept() {
    m_names.clear();
    m_error->clear();

    QSet<QString> seen;
    const QStringList lines = m_input->toPlainText().split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString name = raw.trimmed();
        if (name.isEmpty()) continue;
        const QString error = validateName(name);
        if (!error.isEmpty()) {
            m_error->setText(QStringLiteral("%1: %2").arg(name, error));
            return;
        }
        if (seen.contains(name)) {
            m_error->setText(QStringLiteral("Duplicate name: %1").arg(name));
            return;
        }
        seen.insert(name);
        m_names.append(name);
    }
    if (m_names.isEmpty()) {
        m_error->setText(QStringLiteral("Enter at least one name"));
        return;
    }
    QDialog::accept();
}

} // namespace Widgets
