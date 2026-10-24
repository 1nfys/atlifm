#include "StatusBar.hpp"
#include "../atlifm-core/Network.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTimer>
#include <QAbstractItemView>
#include <QApplication>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <cstdlib>

namespace Widgets {

static QString pathToQString(const std::filesystem::path& p) {
    // явный utf8: нативные байты в linux это utf8 поэтому декодируем так для кириллицы
    return QString::fromUtf8(p.c_str());
}

static QString formatPathString(const std::filesystem::path& path) {
    QString q = pathToQString(path);
    const char* home = std::getenv("HOME");
    if (home) {
        QString homeQ = QString::fromUtf8(home);
        if (q == homeQ) {
            return QStringLiteral("~");
        }
        if (q.startsWith(homeQ + "/")) {
            q = QStringLiteral("~") + q.mid(homeQ.length());
        }
    }
    return q;
}

TuiStatusBar::TuiStatusBar(QWidget* parent)
    : QWidget(parent) {
    setFixedHeight(25); // контент 18px + 7px нижнего отступа
    setupUi();
}

void TuiStatusBar::setupUi() {
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(6, 0, 8, 7); // низ 7px отступа
    m_layout->setSpacing(8);

    // адресная строка растягивается динамически
    m_pathInput = new QLineEdit(this);
    m_pathInput->setFont(Theme::getMonospaceFont(9));
    m_pathInput->setFrame(false);
    // без вертикальных отступов, иначе минимум виджета (шрифт + padding + рамка)
    // переполняет контентную высоту бара и съедает нижний отступ панели
    m_pathInput->setStyleSheet(QStringLiteral("QLineEdit { padding: 0px 6px; }"));
    m_pathInput->setAlignment(Qt::AlignLeft);
    m_pathInput->installEventFilter(this);

    auto inputLayout = new QHBoxLayout(m_pathInput);
    inputLayout->setContentsMargins(0, 0, 6, 0);
    inputLayout->addStretch(1);

    m_pathSuffixLabel = new QLabel("path", m_pathInput);
    m_pathSuffixLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_pathSuffixLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    inputLayout->addWidget(m_pathSuffixLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    QFontMetrics fm(m_pathSuffixLabel->font());
    int badgeWidth = fm.horizontalAdvance("path") + 12;
    m_pathInput->setTextMargins(2, 0, badgeWidth, 0);

    m_completerModel = new QFileSystemModel(this);
    m_completerModel->setRootPath(QDir::homePath());
    m_completerModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);

    m_pathCompleter = new QCompleter(m_completerModel, this);
    m_pathCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_pathCompleter->setCompletionMode(QCompleter::PopupCompletion);

    m_pathInput->setCompleter(m_pathCompleter);

    m_pathInput->setMinimumWidth(80);
    m_pathInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    connect(m_pathInput, &QLineEdit::returnPressed, this, &TuiStatusBar::onPathInputReturnPressed);
    m_layout->addWidget(m_pathInput, 1, Qt::AlignVCenter);

    m_filterCommandLabel = new QLabel(this);
    m_filterCommandLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_filterCommandLabel->setTextFormat(Qt::PlainText);
    m_filterCommandLabel->hide();
    m_filterCommandLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_layout->addWidget(m_filterCommandLabel, 0, Qt::AlignVCenter);

    m_filterInput = new QLineEdit(this);
    m_filterInput->setFont(Theme::getMonospaceFont(9));
    m_filterInput->setPlaceholderText("Search...");
    m_filterInput->setFrame(false);
    m_filterInput->setStyleSheet(QStringLiteral("QLineEdit { padding: 0px 6px; }"));
    m_filterInput->hide();
    m_filterInput->installEventFilter(this);

    connect(m_filterInput, &QLineEdit::textChanged, this, &TuiStatusBar::handleFilterTextChanged);
    connect(m_filterInput, &QLineEdit::returnPressed, this, &TuiStatusBar::filterSubmitted);
    m_layout->addWidget(m_filterInput, 0, Qt::AlignVCenter);

    // бейдж сообщений, слева от индикатора sudo
    m_statusMessageLabel = new QLabel(this);
    m_statusMessageLabel->setFont(Theme::getMonospaceFont(9));
    // пустой бейдж скрываем, иначе между адресной строкой и правами
    // двойной отступ (пустой label + его spacing)
    m_statusMessageLabel->hide();
    m_layout->addWidget(m_statusMessageLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    // инлайн промпт размещается в области бейджа отмены и повтора, перед счётчиком
    m_promptLabel = new QLabel(this);
    m_promptLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_promptLabel->hide();
    m_layout->addWidget(m_promptLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    m_promptInput = new QLineEdit(this);
    m_promptInput->setFont(Theme::getMonospaceFont(9));
    m_promptInput->setStyleSheet(QStringLiteral("QLineEdit { padding: 0px 6px; }"));
    m_promptInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_promptInput->installEventFilter(this);
    m_promptInput->hide();
    m_layout->addWidget(m_promptInput, 1, Qt::AlignVCenter);
    connect(m_promptInput, &QLineEdit::returnPressed, this, [this]() { finishInlinePrompt(1); });

    m_promptOkBtn = new QPushButton("Yes", this);
    m_promptOkBtn->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_promptOkBtn->setStyleSheet(QStringLiteral("QPushButton { padding: 0px 8px; }"));
    m_promptOkBtn->setCursor(Qt::PointingHandCursor);
    m_promptOkBtn->installEventFilter(this);
    m_promptOkBtn->hide();
    m_layout->addWidget(m_promptOkBtn, 0, Qt::AlignVCenter);
    connect(m_promptOkBtn, &QPushButton::clicked, this, [this]() { finishInlinePrompt(1); });

    m_promptCancelBtn = new QPushButton("No", this);
    m_promptCancelBtn->setFont(Theme::getMonospaceFont(9));
    m_promptCancelBtn->setStyleSheet(QStringLiteral("QPushButton { padding: 0px 8px; }"));
    m_promptCancelBtn->setCursor(Qt::PointingHandCursor);
    m_promptCancelBtn->installEventFilter(this);
    m_promptCancelBtn->hide();
    m_layout->addWidget(m_promptCancelBtn, 0, Qt::AlignVCenter);
    connect(m_promptCancelBtn, &QPushButton::clicked, this, [this]() { finishInlinePrompt(0); });

    // агрегированный прогресс фоновых задач слева от бейджа sudo
    m_taskSummaryLabel = new QLabel(this);
    m_taskSummaryLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_taskSummaryLabel->hide();
    m_layout->addWidget(m_taskSummaryLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    // красный бейдж sudo между бейджами сообщений и счётчиком файлов
    m_sudoBadgeLabel = new QLabel("sudo", this);
    m_sudoBadgeLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_sudoBadgeLabel->setAlignment(Qt::AlignCenter);
    m_sudoBadgeLabel->hide();
    m_layout->addWidget(m_sudoBadgeLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    // права текущего элемента, правый кластер в стиле yazi
    m_permsLabel = new QLabel(this);
    m_permsLabel->setFont(Theme::getMonospaceFont(9));
    m_permsLabel->setAlignment(Qt::AlignCenter);
    m_permsLabel->setTextFormat(Qt::RichText);
    m_layout->addWidget(m_permsLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    // счётчик выделения вида 1/70 или 3/70 sel, прижат вправо
    m_statsLabel = new QLabel(this);
    m_statsLabel->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_statsLabel->setAlignment(Qt::AlignCenter);
    m_layout->addWidget(m_statsLabel, 0, Qt::AlignVCenter | Qt::AlignRight);

    // скрытая неиспользуемая метка диска, оставлена для сохранения ABI
    m_diskLabel = new QLabel(this);
    m_diskLabel->hide();

    updateTheme();
}

void TuiStatusBar::updateTheme() {
    using namespace Theme::Colors;

    // полупрозрачный текст "path" справа внутри строки ввода (alpha 80)
    QColor dimCol = Dim();
    m_pathSuffixLabel->setStyleSheet(QString(
        "QLabel { color: rgba(%1, %2, %3, 80); font-weight: bold; padding-right: 2px; }"
    ).arg(dimCol.red()).arg(dimCol.green()).arg(dimCol.blue()));

    m_pathInput->setStyleSheet(QString(R"(
        QLineEdit {
            background-color: transparent;
            color: %1;
            border: none;
            border-bottom: 1px solid %2;
            padding: 1px 4px;
            selection-background-color: %2;
            selection-color: %3;
        }
        QLineEdit:focus {
            border: none;
            border-bottom: 1px solid %2;
        }
    )")
    .arg(Text().name(), Accent().name(), AccentFg().name()));

    m_filterInput->setStyleSheet(QString(R"(
        QLineEdit {
            background-color: transparent;
            color: %1;
            border: none;
            border-bottom: 1px solid %2;
            padding: 1px 4px;
            selection-background-color: %2;
            selection-color: %3;
        }
        QLineEdit:focus {
            border: none;
            border-bottom: 1px solid %2;
        }
    )")
    .arg(Text().name(), Accent().name(), AccentFg().name()));

    m_filterCommandLabel->setStyleSheet(QString(
        "QLabel { background-color: %1; color: %2; padding: 1px 7px; margin-right: 2px; }")
        .arg(Accent().name(), AccentFg().name()));

    if (m_pathCompleter) {
        if (auto* popup = m_pathCompleter->popup()) {
            popup->setFont(Theme::getMonospaceFont(9));
            popup->setStyleSheet(QString(
                "QAbstractItemView {"
                "  background-color: %1;"
                "  color: %2;"
                "  border: 1px solid %3;"
                "  selection-background-color: %4;"
                "  selection-color: %5;"
                "  outline: none;"
                "  padding: 2px;"
                "}"
                "QAbstractItemView::item {"
                "  padding: 3px 6px;"
                "  min-height: 20px;"
                "}"
                "QAbstractItemView::item:selected {"
                "  background-color: %4;"
                "  color: %5;"
                "}"
            ).arg(
                Base().name(),
                Text().name(),
                Surface1().name(),
                Accent().name(),
                AccentFg().name()
            ));
        }
    }

    m_sudoBadgeLabel->setStyleSheet(QString(
        "QLabel { background-color: %1; color: %2; font-weight: bold; padding: 1px 8px; border-radius: 0px; }"
    ).arg(Red().name(), Base().name()));

    if (m_taskSummaryLabel) {
        m_taskSummaryLabel->setStyleSheet(QString(
            "QLabel { background-color: %1; color: %2; font-weight: bold; padding: 1px 8px; border-radius: 0px; }"
        ).arg(Surface1().name(), Accent().name()));
    }

    m_statsLabel->setStyleSheet(QString(
        "QLabel { background-color: %1; color: %2; font-weight: bold; padding: 1px 8px; border-radius: 0px; }"
    ).arg(Accent().name(), AccentFg().name()));

    m_permsLabel->setStyleSheet(QString(
        "QLabel { color: %1; }"
    ).arg(Dim().name()));

    if (m_promptLabel) {
        m_promptLabel->setStyleSheet(QString(
            "QLabel { color: %1; font-weight: bold; padding: 0 2px; }"
        ).arg(Accent().name()));
    }
    if (m_promptInput) {
        m_promptInput->setStyleSheet(QString(R"(
            QLineEdit {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 1px 6px;
                selection-background-color: %4;
                selection-color: %5;
            }
        )").arg(Base().name(), Text().name(), Surface1().name(), Accent().name(), AccentFg().name()));
    }
    if (m_promptOkBtn) {
        m_promptOkBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: none;
                border-radius: 0px;
                padding: 2px 12px;
                font-weight: bold;
            }
        )").arg(Accent().name(), AccentFg().name()));
    }
    if (m_promptCancelBtn) {
        m_promptCancelBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: 0px;
                padding: 2px 12px;
                font-weight: bold;
            }
        )").arg(Surface0().name(), Text().name(), Surface1().name()));
    }

    update();
}

bool TuiStatusBar::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_pathInput || watched == m_filterInput) && event->type() == QEvent::ContextMenu) {
        auto* lineEdit = qobject_cast<QLineEdit*>(watched);
        if (lineEdit) {
            auto* cme = static_cast<QContextMenuEvent*>(event);
            QMenu* menu = lineEdit->createStandardContextMenu();
            menu->setFont(Theme::getMonospaceFont(9));
            menu->setStyleSheet(Theme::getApplicationStyleSheet());
            for (auto* act : menu->actions()) {
                act->setFont(Theme::getMonospaceFont(9));
            }
            menu->exec(cme->globalPos());
            delete menu;
            return true;
        }
    }

    if (m_promptActive && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        int key = ke->key();
        if (key == Qt::Key_Escape) {
            finishInlinePrompt(0);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // enter подтверждаем вручную: QPushButton на enter кликает только
            // для кнопок в QDialog, у кнопок промпта диалога нет
            if (watched == m_promptInput) {
                // обработает returnPressed
            } else if (watched == m_promptCancelBtn) {
                finishInlinePrompt(0);
                return true;
            } else {
                finishInlinePrompt(1);
                return true;
            }
        }
        if (watched == m_promptOkBtn || watched == m_promptCancelBtn) {
            if (key == Qt::Key_Y) {
                finishInlinePrompt(1);
                return true;
            }
            if (key == Qt::Key_N) {
                finishInlinePrompt(0);
                return true;
            }
            if ((key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Tab) &&
                m_promptCancelBtn->isVisible()) {
                if (watched == m_promptOkBtn) m_promptCancelBtn->setFocus();
                else m_promptOkBtn->setFocus();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TuiStatusBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    int w = width();
    int ptSize = (w < 400) ? 7 : ((w < 550) ? 8 : 9);
    QFont f = Theme::getMonospaceFont(ptSize);
    m_pathInput->setFont(f);
    m_pathSuffixLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    m_statsLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    m_sudoBadgeLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    if (m_taskSummaryLabel) m_taskSummaryLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    m_statusMessageLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    if (m_permsLabel) m_permsLabel->setFont(Theme::getMonospaceFont(ptSize));
    if (m_filterInput) m_filterInput->setFont(f);
    if (m_promptLabel) m_promptLabel->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    if (m_promptInput) m_promptInput->setFont(f);
    if (m_promptOkBtn) m_promptOkBtn->setFont(Theme::getMonospaceFont(ptSize, QFont::Bold));
    if (m_promptCancelBtn) m_promptCancelBtn->setFont(Theme::getMonospaceFont(ptSize));

    using namespace Theme::Colors;
    if (w < 420) {
        // узкое окно: компактная адресная строка, счётчик файлов скрывается
        m_pathSuffixLabel->hide();
        m_pathInput->setTextMargins(2, 0, 2, 0);
        m_pathInput->setMinimumWidth(40);

        m_statsLabel->hide();
        m_permsLabel->hide();
        if (auto* l = qobject_cast<QHBoxLayout*>(layout())) {
            l->setContentsMargins(3, 0, 3, 0);
            l->setSpacing(4);
        }
    } else {
        m_pathSuffixLabel->show();
        QFontMetrics fm(m_pathSuffixLabel->font());
        int badgeWidth = fm.horizontalAdvance("path") + 12;
        m_pathInput->setTextMargins(2, 0, badgeWidth, 0);
        m_pathInput->setMinimumWidth(80);

        if (m_statsEnabled) m_statsLabel->show();
        if (!m_permsLabel->text().isEmpty()) m_permsLabel->show();
        if (auto* l = qobject_cast<QHBoxLayout*>(layout())) {
            l->setContentsMargins(6, 0, 8, 0);
            l->setSpacing(8);
        }
    }
}

void TuiStatusBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    // тот же base, что у колонок с файлами, нижняя панель не отдельная полоса другого оттенка
    p.fillRect(rect(), Theme::Colors::Base());
}

void TuiStatusBar::setPath(const std::filesystem::path& path) {
    m_currentPath = path;
    QString formatted = formatPathString(path);

    if (!m_pathInput->hasFocus()) {
        m_pathInput->setText(formatted);
    }

    setMountStats(path);
}

void TuiStatusBar::onPathInputReturnPressed() {
    QString text = m_pathInput->text().trimmed();
    if (text.isEmpty()) {
        m_pathInput->setText(formatPathString(m_currentPath));
        m_pathInput->clearFocus();
        return;
    }

    // utf8 пути и раскрываем тильду в домашний каталог
    std::string pathBytes = text.toUtf8().toStdString();
    const char* home = std::getenv("HOME");
    if (home && !pathBytes.empty() && pathBytes[0] == '~') {
        if (pathBytes.size() == 1 || pathBytes[1] == '/') {
            pathBytes = std::string(home) + pathBytes.substr(1);
        }
    }

    std::filesystem::path p(pathBytes);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        emit pathEntered(p);
    } else if (Core::Network::looksLikeNetwork(text)) {
        emit networkLocationEntered(text);
        m_pathInput->setText(formatPathString(m_currentPath));
    } else {
        m_pathInput->setText(formatPathString(m_currentPath));
        setProgressMessage(QStringLiteral("Path not found: %1").arg(text));
    }
    m_pathInput->clearFocus();
}

void TuiStatusBar::focusPathInput() {
    m_pathSuffixLabel->hide();
    m_pathInput->setFocus();
}

void TuiStatusBar::setItemStatsVisible(bool visible) {
    m_statsEnabled = visible;
    if (m_statsLabel) m_statsLabel->setVisible(visible);
}

void TuiStatusBar::addTrailingWidget(QWidget* widget) {
    if (widget && m_layout) m_layout->addWidget(widget, 0, Qt::AlignVCenter);
}

void TuiStatusBar::setItemStats(int currentIndex, int totalItems, int selectedItems) {
    if (!m_statsEnabled) return;
    // счётчик без скобок: "1/70" для фокуса, "3/70 sel" для мультивыбора
    if (totalItems <= 0 && selectedItems <= 0) {
        m_statsLabel->setText(QStringLiteral("0"));
    } else if (selectedItems > 1) {
        m_statsLabel->setText(QStringLiteral("%1/%2 sel").arg(selectedItems).arg(totalItems));
    } else {
        int cur = std::max(1, currentIndex);
        m_statsLabel->setText(QStringLiteral("%1/%2").arg(cur).arg(totalItems));
    }

    if (width() < 420) {
        m_statsLabel->hide();
    } else {
        m_statsLabel->show();
    }
}

void TuiStatusBar::setItemPerms(const QString& perms) {
    if (!m_permsLabel) return;
    // посимвольная раскраска прав: read жёлтый, write красный, прочерк почти невидимый
    QString html;
    html.reserve(perms.size() * 40);
    for (const QChar c : perms) {
        QString color;
        if (c == QLatin1Char('r')) {
            color = Theme::Colors::Yellow().name();
        } else if (c == QLatin1Char('w')) {
            color = Theme::Colors::Red().name();
        } else if (c == QLatin1Char('-')) {
            color = Theme::Colors::Surface2().name();
        } else {
            // тип (d/l/c/b/p), exec (x) и suid/sticky (s/t/S/T)
            color = Theme::Colors::Accent().name();
        }
        html += QStringLiteral("<span style='color:%1'>%2</span>").arg(color, QString(c));
    }
    m_permsLabel->setText(html);
    m_permsLabel->setVisible(!perms.isEmpty() && width() >= 420);
}

void TuiStatusBar::setSudoBadge(bool visible) {
    if (m_sudoBadgeLabel) {
        m_sudoBadgeLabel->setVisible(visible);
    }
}

bool TuiStatusBar::isSudoBadgeVisible() const {
    return m_sudoBadgeLabel ? m_sudoBadgeLabel->isVisible() : false;
}

void TuiStatusBar::setMountStats(const std::filesystem::path& path) {
    std::error_code ec;
    auto spaceInfo = std::filesystem::space(path, ec);
    if (!ec) {
        QString freeStr = Core::XdgUtils::formatSize(spaceInfo.available);
        m_diskLabel->setText(QString("\u2502 Free: %1").arg(freeStr));
    } else {
        m_diskLabel->clear();
    }
}

void TuiStatusBar::setProgressMessage(const QString& msg, bool isFinished) {
    m_statusMessageLabel->setText(msg);
    if (!msg.isEmpty()) {
        m_statusMessageLabel->show();
        // разные высококонтрастные цвета с тёмным шрифтом для разных типов сообщений
        using namespace Theme::Colors;
        QString lower = msg.toLower();
        QString bg = Sapphire().name();
        if (lower.contains("trash") || lower.contains("delet") || lower.contains("cut") ||
            lower.contains("fail") || lower.contains("error") || lower.contains("cancel") || lower.contains("clear")) {
            bg = Red().name();
        } else if (lower.contains("undo") || lower.contains("redo")) {
            bg = Peach().name();
        } else if (lower.contains("cop") || lower.contains("creat") || lower.contains("extract") ||
                   lower.contains("archiv") || lower.contains("mov") || lower.contains("past")) {
            bg = Green().name();
        } else if (lower.contains("renam") || lower.contains("path") || lower.contains("bookmark") ||
                   lower.contains("hidden") || lower.contains("sort") || lower.contains("filter")) {
            bg = Teal().name();
        }

        m_statusMessageLabel->setStyleSheet(QString(
            "QLabel { background-color: %1; color: %2; font-weight: bold; padding: 1px 8px; border-radius: 0px; }"
        ).arg(bg, Base().name()));

        if (m_messageTimer) {
            m_messageTimer->stop();
        }
        if (isFinished) {
            scheduleMessageDismiss();
        }
    } else {
        if (m_messageTimer) m_messageTimer->stop();
        m_statusMessageLabel->clear();
        m_statusMessageLabel->hide();
    }
}

void TuiStatusBar::setTaskSummary(const QString& summary) {
    if (!m_taskSummaryLabel) return;
    m_taskSummaryLabel->setText(summary);
    m_taskSummaryLabel->setVisible(!summary.isEmpty());
}

// автоскрытие бейджа сообщения через 1.5 с, после инлайн промпта таймер
// перезапускается (beginInlinePrompt останавливает его), иначе бейдж висит вечно
void TuiStatusBar::scheduleMessageDismiss() {
    if (!m_messageTimer) {
        m_messageTimer = new QTimer(this);
        m_messageTimer->setSingleShot(true);
    }
    disconnect(m_messageTimer, &QTimer::timeout, nullptr, nullptr);
    connect(m_messageTimer, &QTimer::timeout, this, [this]() {
        m_statusMessageLabel->clear();
        m_statusMessageLabel->hide();
        m_statusMessageLabel->setStyleSheet(QString("color: %1; padding-left: 4px;").arg(Theme::Colors::Yellow().name()));
    });
    m_messageTimer->start(1500);
}

void TuiStatusBar::setFilterBuffer(const QString& filterText, bool isEditing) {
    if (isEditing) {
        m_filterInput->setText(filterText);
    } else if (filterText.isEmpty()) {
        m_statusMessageLabel->clear();
        m_statusMessageLabel->hide();
    } else {
        m_statusMessageLabel->setText(QString("[Filter: %1]").arg(filterText));
        m_statusMessageLabel->show();
    }
}

void TuiStatusBar::setFilterCommands(const QStringList& commands) {
    m_filterCommands.clear();
    for (const QString& command : commands) {
        const QString token = command.trimmed().toLower();
        if (!token.isEmpty()) m_filterCommands.insert(token);
    }
}

void TuiStatusBar::updateFilterCommandLabel() {
    if (m_filterCommand.isEmpty()) {
        m_filterCommandLabel->hide();
        return;
    }
    m_filterCommandLabel->setText(m_filterCommand);
    m_filterCommandLabel->setVisible(m_isFilterActive);
}

void TuiStatusBar::clearFilterCommand() {
    m_filterCommand.clear();
    m_filterQuery.clear();
    updateFilterCommandLabel();
}

void TuiStatusBar::handleFilterTextChanged(const QString& text) {
    if (!m_filterCommand.isEmpty()) {
        m_filterQuery = text;
        emit filterCommandChanged(m_filterCommand, m_filterQuery);
        return;
    }

    const int space = text.indexOf(QLatin1Char(' '));
    if (space > 0) {
        const QString token = text.left(space).toLower();
        if (m_filterCommands.contains(token)) {
            m_filterCommand = token;
            m_filterQuery = text.mid(space + 1);
            {
                QSignalBlocker blocker(m_filterInput);
                m_filterInput->setText(m_filterQuery);
                m_filterInput->setCursorPosition(m_filterQuery.size());
            }
            updateFilterCommandLabel();
            emit filterCommandChanged(m_filterCommand, m_filterQuery);
            return;
        }
    }

    emit filterChanged(text);
}

bool TuiStatusBar::cancelFilterCommand() {
    if (m_filterCommand.isEmpty()) return false;
    const QString plain = m_filterQuery.isEmpty()
        ? m_filterCommand
        : m_filterCommand + QLatin1Char(' ') + m_filterQuery;
    clearFilterCommand();
    {
        QSignalBlocker blocker(m_filterInput);
        m_filterInput->setText(plain);
        m_filterInput->setCursorPosition(plain.size());
    }
    emit filterChanged(plain);
    return true;
}

void TuiStatusBar::activateFilter() {
    m_isFilterActive = true;
    clearFilterCommand();
    {
        QSignalBlocker blocker(m_filterInput);
        m_filterInput->clear();
    }
    m_filterInput->show();
    m_filterInput->setFocus();
}

void TuiStatusBar::activateFilterWithText(const QString& text) {
    if (!m_isFilterActive) {
        activateFilter();
        m_filterInput->setText(text);
    } else {
        m_filterInput->setText(m_filterInput->text() + text);
    }
}

void TuiStatusBar::deactivateFilter() {
    m_isFilterActive = false;
    clearFilterCommand();
    {
        QSignalBlocker blocker(m_filterInput);
        m_filterInput->clear();
    }
    m_filterInput->hide();
    emit filterDismissed();
}

// инлайн промпт (одно окно, без отдельных окон)

void TuiStatusBar::beginInlinePrompt(bool hidePath) {
    m_prevFocusWidget = QApplication::focusWidget();
    m_promptWasFilterVisible = m_filterInput->isVisible();
    m_promptHidesPath = hidePath;
    if (hidePath) {
        m_pathInput->hide();
    }
    m_filterInput->hide();
    if (m_filterCommandLabel) m_filterCommandLabel->hide();
    if (m_messageTimer) m_messageTimer->stop();
    m_statusMessageLabel->hide();
    m_promptActive = true;
}

void TuiStatusBar::endInlinePrompt() {
    m_promptInput->setEchoMode(QLineEdit::Normal);
    m_promptLabel->hide();
    m_promptInput->hide();
    m_promptInput->clear();
    m_promptOkBtn->hide();
    m_promptCancelBtn->hide();
    if (m_promptHidesPath) {
        m_pathInput->show();
    }
    if (m_promptWasFilterVisible) {
        m_filterInput->show();
        updateFilterCommandLabel();
    }
    if (!m_statusMessageLabel->text().isEmpty()) {
        m_statusMessageLabel->show();
        // промпт останавливал таймер, перезапускаем, чтобы бейдж ушёл сам
        scheduleMessageDismiss();
    }
    m_promptActive = false;
    if (m_prevFocusWidget) {
        m_prevFocusWidget->setFocus();
        m_prevFocusWidget.clear();
    }
}

void TuiStatusBar::finishInlinePrompt(int res) {
    if (!m_promptActive || !m_promptLoop) return;
    m_promptResult = res;
    m_promptLoop->quit();
}

QString TuiStatusBar::askTextInline(const QString& label, const QString& initial,
                                    bool selectStemOnly, bool* ok, bool isPassword,
                                    const std::function<void(const QString&)>& onTextChanged) {
    if (m_promptActive) {
        if (ok) *ok = false;
        return QString();
    }
    beginInlinePrompt(true);
    m_promptLabel->setText(label);
    m_promptLabel->setToolTip(label);
    m_promptLabel->show();
    m_promptInput->setEchoMode(isPassword ? QLineEdit::Password : QLineEdit::Normal);
    m_promptInput->setText(initial);
    m_promptInput->show();
    if (selectStemOnly && !initial.isEmpty()) {
        QFileInfo fi(initial);
        QString stem = fi.completeBaseName();
        if (!stem.isEmpty() && stem.length() < initial.length()) {
            m_promptInput->setSelection(0, stem.length());
        } else {
            m_promptInput->selectAll();
        }
    } else {
        m_promptInput->selectAll();
    }
    m_promptOkBtn->setText("OK");
    m_promptOkBtn->show();
    m_promptCancelBtn->setText("Cancel");
    m_promptCancelBtn->show();
    m_promptInput->setFocus();

    QMetaObject::Connection liveConnection;
    if (onTextChanged) {
        liveConnection = connect(m_promptInput, &QLineEdit::textChanged, this,
                                 [onTextChanged](const QString& text) { onTextChanged(text); });
    }

    QEventLoop loop;
    m_promptLoop = &loop;
    m_promptResult = 0;
    loop.exec();
    m_promptLoop = nullptr;
    if (liveConnection) disconnect(liveConnection);

    QString text = m_promptInput->text();
    bool accepted = (m_promptResult == 1);
    endInlinePrompt();
    if (ok) *ok = accepted;
    return accepted ? text : QString();
}

bool TuiStatusBar::askConfirmInline(const QString& title, const QString& message,
                                     const QString& confirmText, const QString& cancelText,
                                     bool isDanger) {
    if (m_promptActive) return false;
    // поле пути остаётся видимым, чтобы подтверждение появлялось в области бейджа сообщений
    beginInlinePrompt(false);
    QString full = title.isEmpty() ? message : (title + ": " + message);
    m_promptLabel->setText(m_promptLabel->fontMetrics().elidedText(full, Qt::ElideRight, 500));
    m_promptLabel->setToolTip(full);
    if (isDanger) {
        m_promptLabel->setStyleSheet(QString(
            "QLabel { background-color: %1; color: %2; font-weight: bold; padding: 1px 8px; border-radius: 0px; }"
        ).arg(Theme::Colors::Red().name(), Theme::Colors::Base().name()));
    }
    m_promptLabel->show();
    m_promptInput->hide();
    m_promptOkBtn->setText(confirmText.isEmpty() ? "Yes" : confirmText);
    if (isDanger) {
        m_promptOkBtn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: none;
                border-radius: 0px;
                padding: 2px 10px;
                font-weight: bold;
            }
        )").arg(Theme::Colors::Red().name(), Theme::Colors::Base().name()));
    }
    m_promptOkBtn->show();
    if (cancelText.isEmpty()) {
        m_promptCancelBtn->hide();
    } else {
        m_promptCancelBtn->setText(cancelText);
        m_promptCancelBtn->show();
    }
    // фокус на кнопке Yes, чтобы Enter или Y подтверждали сразу
    m_promptOkBtn->setFocus();

    QEventLoop loop;
    m_promptLoop = &loop;
    m_promptResult = 0;
    loop.exec();
    m_promptLoop = nullptr;

    bool accepted = (m_promptResult == 1);
    endInlinePrompt();
    updateTheme();
    return accepted;
}

void TuiStatusBar::showAlertInline(const QString& title, const QString& message) {
    if (m_promptActive) {
        setProgressMessage(title + ": " + message, true);
        return;
    }
    askConfirmInline(title, message, "OK", "", false);
}

}
