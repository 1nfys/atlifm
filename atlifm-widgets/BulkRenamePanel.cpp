#include "BulkRenamePanel.hpp"
#include "../atlifm-theme/Theme.hpp"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>

namespace Widgets {

namespace {

// то же правило что для одиночного переименования чтобы имя не уводило файл в другой каталог и не превращалось в точку
QString validateName(const QString& rawName) {
    const QString name = rawName.trimmed();
    if (name.isEmpty()) return QStringLiteral("Name cannot be empty");
    if (name.contains(QLatin1Char('/'))) return QStringLiteral("Name cannot contain '/'");
    if (name.contains(QChar(u'\0'))) return QStringLiteral("Name contains an invalid character");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return QStringLiteral("Name is reserved");
    return {};
}

QString styleEdit() {
    using namespace Theme::Colors;
    return QString(R"(
        QLineEdit {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: 0px;
            padding: 3px 6px;
            selection-background-color: %4;
            selection-color: %5;
        }
        QLineEdit:focus {
            border: 1px solid %6;
        }
    )").arg(Surface0().name(), Text().name(), Surface1().name(),
            SelectionBg().name(), SelectionFg().name(), Accent().name());
}

} // namespace

BulkRenamePanel::BulkRenamePanel(QWidget* parent)
    : QWidget(parent) {
    using namespace Theme::Colors;

    setObjectName(QStringLiteral("bulkRenamePanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("QWidget#bulkRenamePanel { background: transparent; }"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setFont(Theme::getMonospaceFont(10, QFont::Bold));
    m_title->setStyleSheet(QStringLiteral("color: %1;").arg(Accent().name()));
    header->addWidget(m_title);
    header->addStretch(1);

    auto* hint = new QLabel(QStringLiteral("Enter rename  ·  Esc cancel"), this);
    hint->setFont(Theme::getMonospaceFont(8));
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    header->addWidget(hint);
    root->addLayout(header);

    auto* columns = new QWidget(this);
    auto* columnsLayout = new QHBoxLayout(columns);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(8);

    auto* indexHeader = new QLabel(QStringLiteral("#"), columns);
    indexHeader->setFixedWidth(32);
    indexHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    indexHeader->setFont(Theme::getMonospaceFont(8, QFont::Bold));
    indexHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    columnsLayout->addWidget(indexHeader);

    auto* oldHeader = new QLabel(QStringLiteral("CURRENT NAME"), columns);
    oldHeader->setFont(Theme::getMonospaceFont(8, QFont::Bold));
    oldHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    oldHeader->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    columnsLayout->addWidget(oldHeader, 1);

    auto* arrowHeader = new QLabel(QStringLiteral("→"), columns);
    arrowHeader->setFont(Theme::getMonospaceFont(8, QFont::Bold));
    arrowHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    columnsLayout->addWidget(arrowHeader);

    auto* newHeader = new QLabel(QStringLiteral("NEW NAME"), columns);
    newHeader->setFont(Theme::getMonospaceFont(8, QFont::Bold));
    newHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    newHeader->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    columnsLayout->addWidget(newHeader, 1);
    root->addWidget(columns);

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setStyleSheet(QString(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));
    m_rows = new QWidget(m_scroll);
    m_rows->setStyleSheet(QStringLiteral("background: transparent;"));
    m_rowsLayout = new QVBoxLayout(m_rows);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(3);
    m_rowsLayout->addStretch(1);
    m_scroll->setWidget(m_rows);
    root->addWidget(m_scroll, 1);

    m_error = new QLabel(this);
    m_error->setFont(Theme::getMonospaceFont(8));
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: %1;").arg(Red().name()));
    m_error->hide();
    root->addWidget(m_error);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);

    m_status = new QLabel(this);
    m_status->setFont(Theme::getMonospaceFont(8));
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(Dim().name()));
    footer->addWidget(m_status);
    footer->addStretch(1);

    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setFont(Theme::getMonospaceFont(9));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: 0px;
            padding: 4px 14px;
            font-weight: bold;
        }
        QPushButton:hover { background-color: %3; }
    )").arg(Surface0().name(), Text().name(), Surface1().name()));
    connect(cancel, &QPushButton::clicked, this, &BulkRenamePanel::cancelled);
    footer->addWidget(cancel);

    auto* apply = new QPushButton(QStringLiteral("Rename"), this);
    apply->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    apply->setCursor(Qt::PointingHandCursor);
    apply->setDefault(true);
    apply->setStyleSheet(QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border: none;
            border-radius: 0px;
            padding: 4px 16px;
            font-weight: bold;
        }
        QPushButton:hover { border: 1px solid %2; }
    )").arg(Accent().name(), AccentFg().name()));
    connect(apply, &QPushButton::clicked, this, &BulkRenamePanel::apply);
    footer->addWidget(apply);

    root->addLayout(footer);
}

void BulkRenamePanel::setPaths(const std::vector<std::filesystem::path>& paths) {
    m_paths = paths;
    m_renames.clear();
    clearError();
    rebuildRows();
    focusFirstEdit();
}

void BulkRenamePanel::focusFirstEdit() {
    if (!m_edits.isEmpty()) {
        focusEdit(0);
    } else {
        setFocus(Qt::OtherFocusReason);
    }
}

void BulkRenamePanel::rebuildRows() {
    if (!m_rowsLayout) return;

    while (QLayoutItem* item = m_rowsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_edits.clear();

    for (int i = 0; i < static_cast<int>(m_paths.size()); ++i) {
        const auto& path = m_paths[static_cast<size_t>(i)];
        const QString oldName = QString::fromUtf8(path.filename().c_str());

        auto* row = new QWidget(m_rows);
        row->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* index = new QLabel(QString::number(i + 1), row);
        index->setFixedWidth(32);
        index->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        index->setFont(Theme::getMonospaceFont(8));
        index->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Dim().name()));
        rowLayout->addWidget(index);

        auto* oldLabel = new QLabel(oldName, row);
        oldLabel->setFont(Theme::getMonospaceFont(9));
        oldLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Subtext().name()));
        oldLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        oldLabel->setMinimumWidth(120);
        oldLabel->setToolTip(QString::fromUtf8(path.c_str()));
        rowLayout->addWidget(oldLabel, 1);

        auto* arrow = new QLabel(QStringLiteral("→"), row);
        arrow->setFont(Theme::getMonospaceFont(9));
        arrow->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::Colors::Dim().name()));
        rowLayout->addWidget(arrow);

        auto* edit = new QLineEdit(oldName, row);
        edit->setFont(Theme::getMonospaceFont(9));
        edit->setStyleSheet(styleEdit());
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        edit->setMinimumWidth(160);
        edit->setClearButtonEnabled(false);
        edit->installEventFilter(this);
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            clearError();
            updateStatus();
        });
        rowLayout->addWidget(edit, 1);

        m_rowsLayout->addWidget(row);
        m_edits.append(edit);
    }

    m_rowsLayout->addStretch(1);
    if (m_title) {
        m_title->setText(QStringLiteral("BULK RENAME · %1 ITEMS")
                             .arg(static_cast<qulonglong>(m_paths.size())));
    }
    updateStatus();
}

void BulkRenamePanel::updateStatus() {
    int changed = 0;
    for (int i = 0; i < m_edits.size() && i < static_cast<int>(m_paths.size()); ++i) {
        const QString newName = m_edits[i]->text().trimmed();
        const QString oldName = QString::fromUtf8(m_paths[static_cast<size_t>(i)].filename().c_str());
        if (!newName.isEmpty() && newName != oldName) ++changed;
    }
    if (m_status) {
        m_status->setText(QStringLiteral("%1 of %2 names will change")
                              .arg(changed)
                              .arg(m_edits.size()));
    }
}

void BulkRenamePanel::apply() {
    clearError();
    m_renames.clear();

    QSet<QString> targets;
    for (int i = 0; i < m_edits.size() && i < static_cast<int>(m_paths.size()); ++i) {
        const auto& source = m_paths[static_cast<size_t>(i)];
        const QString oldName = QString::fromUtf8(source.filename().c_str());
        const QString newName = m_edits[i]->text().trimmed();
        if (newName == oldName) continue;

        const QString nameError = validateName(newName);
        if (!nameError.isEmpty()) {
            showError(QStringLiteral("Row %1: %2").arg(i + 1).arg(nameError), i);
            return;
        }

        const std::filesystem::path target = source.parent_path() / newName.toUtf8().toStdString();
        const QString targetKey = QString::fromUtf8(target.c_str());
        if (targets.contains(targetKey)) {
            showError(QStringLiteral("Row %1: the same target name is already used above")
                          .arg(i + 1), i);
            return;
        }
        targets.insert(targetKey);

        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            showError(QStringLiteral("Row %1: an item with this name already exists")
                          .arg(i + 1), i);
            return;
        }

        m_renames.emplace_back(source, target);
    }

    if (m_renames.empty()) {
        showError(QStringLiteral("No changes to rename"));
        return;
    }
    emit accepted();
}

void BulkRenamePanel::showError(const QString& message, int row) {
    if (m_error) {
        m_error->setText(message);
        m_error->show();
    }
    if (row >= 0 && row < m_edits.size()) {
        focusEdit(row);
        m_edits[row]->setFocus(Qt::OtherFocusReason);
        m_edits[row]->selectAll();
    }
}

void BulkRenamePanel::clearError() {
    if (!m_error) return;
    m_error->clear();
    m_error->hide();
}

void BulkRenamePanel::focusEdit(int index) {
    if (index < 0 || index >= m_edits.size()) return;
    QLineEdit* edit = m_edits[index];
    edit->setFocus(Qt::OtherFocusReason);
    edit->selectAll();
    if (m_scroll) m_scroll->ensureWidgetVisible(edit, 0, 12);
}

bool BulkRenamePanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (watched == this) {
            if (key->key() == Qt::Key_Escape) {
                emit cancelled();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                apply();
                return true;
            }
        }
        auto* edit = qobject_cast<QLineEdit*>(watched);
        if (edit && m_edits.contains(edit)) {
            if (key->key() == Qt::Key_Escape) {
                emit cancelled();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                apply();
                return true;
            }
            if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
                const int current = m_edits.indexOf(edit);
                const int delta = key->key() == Qt::Key_Up ? -1 : 1;
                const int last = static_cast<int>(m_edits.size()) - 1;
                const int next = std::clamp(current + delta, 0, last);
                if (next != current) {
                    focusEdit(next);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BulkRenamePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        apply();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace Widgets
