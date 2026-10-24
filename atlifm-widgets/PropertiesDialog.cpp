#include "PropertiesDialog.hpp"
#include "TuiInputPopup.hpp"
#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-core/XdgUtils.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMimeDatabase>
#include <QKeyEvent>
#include <QEventLoop>
#include <QStorageInfo>
#include <QTextEdit>
#include <QLineEdit>
#include <QTextStream>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QDir>
#include <QTimer>
#include <QMenu>
#include <QScrollBar>
#include <cmath>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Widgets {

PropertiesDialog::PropertiesDialog(const std::filesystem::path& path, QWidget* parent)
    : QDialog(parent), m_path(path) {
    // мгновенный показ без анимаций и затемнений (запрещены): без
    // полупрозрачности, без fade, без overlay на всё окно
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_TranslucentBackground, false);
    QString fnameQ = QString::fromUtf8(path.filename().c_str());
    if (fnameQ.isEmpty()) fnameQ = QString::fromUtf8(path.c_str());
    setWindowTitle(QString("Properties: %1").arg(fnameQ));
    setFont(Theme::getMonospaceFont(10));
    setFixedWidth(560);
    setStyleSheet(Theme::getApplicationStyleSheet());
    setupUi();
    loadFileInfo();
}

PropertiesDialog::~PropertiesDialog() {
    // гасим живой скан: устаревшие файлы и сигналы не нужны
    ++m_diskScanJob;
    if (m_diskProc && m_diskProc->state() != QProcess::NotRunning) {
        m_diskProc->disconnect(this);
        m_diskProc->kill();
        m_diskProc->waitForFinished(200);
    }
    ++m_weightJob;
    if (m_weightProc && m_weightProc->state() != QProcess::NotRunning) {
        m_weightProc->disconnect(this);
        m_weightProc->kill();
        m_weightProc->waitForFinished(200);
    }
}

int PropertiesDialog::exec() {
    // мгновенный показ: nonmodal + show() вместо applicationmodal qdialog::exec()
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_TranslucentBackground, false);
    show();
    raise();
    activateWindow();
    int res = QDialog::Rejected;
    QEventLoop loop;
    connect(this, &QDialog::finished, &loop, [&](int r) {
        res = r;
        loop.quit();
    });
    loop.exec();
    return res;
}

void PropertiesDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (m_embedded) {
            emit closeRequested();
        } else {
            reject();
        }
        return;
    }
    QDialog::keyPressEvent(event);
}

void PropertiesDialog::setEmbedded(bool embedded) {
    m_embedded = embedded;
    if (embedded) {
        // обычный дочерний виджет: никакого отдельного окна, снимаем
        // фиксированную ширину диалога чтобы не ломать сплиттер
        setWindowFlags(Qt::Widget);
        setWindowModality(Qt::NonModal);
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

void PropertiesDialog::setPath(const std::filesystem::path& p) {
    if (p.empty()) return;
    m_path = p;
    QString fnameQ = QString::fromUtf8(p.filename().c_str());
    if (fnameQ.isEmpty()) fnameQ = QString::fromUtf8(p.c_str());
    setWindowTitle(QString("Properties: %1").arg(fnameQ));
    loadFileInfo();
}

// заголовок секции: только название (accent, bold), без рамок и ободков
namespace {
QLabel* makeSectionHeader(const QString& title, QWidget* parent) {
    auto* lbl = new QLabel(parent);
    lbl->setTextFormat(Qt::RichText);
    lbl->setText(QStringLiteral(
        "<span style=\"color:%1; font-weight:bold;\">%2</span>")
        .arg(Theme::Colors::Accent().name(), title.toHtmlEscaped()));
    lbl->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    return lbl;
}

bool isVirtualKernelFs(const QStorageInfo& storage) {
    const QString fs = QString::fromLatin1(storage.fileSystemType());
    return fs == QLatin1String("proc") || fs == QLatin1String("sysfs")
        || fs == QLatin1String("devtmpfs") || fs == QLatin1String("devpts")
        || fs == QLatin1String("efivarfs") || fs == QLatin1String("cgroup")
        || fs == QLatin1String("cgroup2") || fs == QLatin1String("pstore")
        || fs == QLatin1String("securityfs") || fs == QLatin1String("debugfs")
        || fs == QLatin1String("tracefs") || fs == QLatin1String("configfs")
        || fs == QLatin1String("fusectl") || fs == QLatin1String("mqueue")
        || fs == QLatin1String("hugetlbfs");
}

bool isMountRoot(const std::filesystem::path& p) {
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    if (canon == std::filesystem::path("/")) return true;

    const QString qPath = QDir::cleanPath(QString::fromUtf8(canon.c_str()));
    const QStorageInfo storage(qPath);
    if (!storage.isValid() || !storage.isReady()) return false;
    if (isVirtualKernelFs(storage)) return false;

    const QString root = QDir::cleanPath(storage.rootPath());
    if (root != qPath) return false;

    const QString fs = QString::fromLatin1(storage.fileSystemType()).toLower();
    if (fs == QLatin1String("autofs") || fs == QLatin1String("tmpfs")) {
        return false;
    }

    if (qPath.startsWith(QLatin1String("/mnt/")) ||
        qPath.startsWith(QLatin1String("/media/")) ||
        qPath.startsWith(QLatin1String("/run/media/"))) {
        return true;
    }

    if (qPath.startsWith(QLatin1String("/var/")) ||
        qPath.startsWith(QLatin1String("/etc/")) ||
        qPath.startsWith(QLatin1String("/usr/")) ||
        qPath.startsWith(QLatin1String("/sys/")) ||
        qPath.startsWith(QLatin1String("/proc/")) ||
        qPath.startsWith(QLatin1String("/dev/")) ||
        qPath.startsWith(QLatin1String("/run/"))) {
        return false;
    }

    return true;
}

}

void PropertiesDialog::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 12, 14, 12);
    mainLayout->setSpacing(8);

    auto grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(5);

    using namespace Theme::Colors;

    auto addRow = [grid, this](int row, const QString& title, QLabel*& valLabel) {
        auto lbl = new QLabel(title + ":", this);
        lbl->setStyleSheet(QString("color: %1; font-weight: bold;").arg(Theme::Colors::Subtext().name()));
        valLabel = new QLabel(this);
        valLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valLabel->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Text().name()));
        grid->addWidget(lbl, row, 0);
        grid->addWidget(valLabel, row, 1);
    };

    addRow(0, "Name", m_nameLabel);
    addRow(1, "Location", m_locationLabel);
    addRow(2, "MIME Type", m_typeLabel);
    addRow(3, "Size", m_sizeLabel);
    addRow(4, "Modified", m_modifiedLabel);
    addRow(5, "Inode / Links", m_inodeLabel);
    addRow(6, "Owner / Group", m_ownerLabel);
    m_sizeLabel->setObjectName(QStringLiteral("propsSizeValue"));

    mainLayout->addLayout(grid);

    // disk usage консоль внутри диалога свойств, автоматически для корня
    // диска/фс; скан фоновый, ui не блокируется
    m_diskUsageGroup = new QWidget(this);
    m_diskUsageGroup->setObjectName(QStringLiteral("diskUsageGroup"));
    auto diskLayout = new QVBoxLayout(m_diskUsageGroup);
    diskLayout->setContentsMargins(0, 0, 0, 0);
    diskLayout->setSpacing(8);
    diskLayout->addWidget(makeSectionHeader(QStringLiteral("DISK USAGE"), m_diskUsageGroup));

    // строка статуса скана над консолью: чип «scanning [бар] nn%» и вплотную
    // к нему чип «needed sudo» + настоящее поле пароля (резерв, когда полкит
    // недоступен или отказал; в норме пароль спрашивает системный диалог pkexec)
    // живёт отдельным виджетом рядом с полосой прогресса, по умолчанию скрыта
    m_diskStatusRow = new QWidget(m_diskUsageGroup);
    m_diskStatusRow->setObjectName(QStringLiteral("diskStatusRow"));
    auto statusLayout = new QHBoxLayout(m_diskStatusRow);
    statusLayout->setContentsMargins(0, 0, 0, 6);
    statusLayout->setSpacing(5);

    // плашки scanning / needed sudo одной ширины (общий моношрифт + равное
    // число знаков с полями nbsp); needed sudo встаёт ровно на место
    // полосы прогресса и перекрывает её, пока ждём пароль
    auto chipHtml = [](const QString& text, const QColor& bg, const QColor& fg) {
        constexpr int kChipChars = 13; // 11 знаков «needed sudo» + поля как у scanning
        const int inner = std::max(0, kChipChars - int(text.size()));
        const int left = inner / 2;
        const int right = inner - left;
        const QString padded = QString(left, QChar(0x00A0)) + text + QString(right, QChar(0x00A0));
        return QStringLiteral("<span style=\"background-color:%1; color:%2; font-weight:bold;\">%3</span>")
            .arg(bg.name(), fg.name(), padded.toHtmlEscaped());
    };

    m_diskScanChip = new QLabel(chipHtml(QStringLiteral("scanning"), Sapphire(), Base()), m_diskStatusRow);
    m_diskScanChip->setTextFormat(Qt::RichText);
    m_diskScanChip->setFont(Theme::getMonospaceFont(9));

    m_diskBarLabel = new QLabel(m_diskStatusRow);
    m_diskBarLabel->setTextFormat(Qt::RichText);
    m_diskBarLabel->setFont(Theme::getMonospaceFont(9));

    // плашка needed sudo встаёт ровно на место чипа scanning (та же ширина),
    // scanning в это время скрыт; бар не сбрасывается, а замирает на последнем
    // показанном значении и остаётся видна справа от плашки
    m_diskNeededChip = new QLabel(chipHtml(QStringLiteral("needed sudo"), Red(), Base()), m_diskStatusRow);
    m_diskNeededChip->setTextFormat(Qt::RichText);
    m_diskNeededChip->setFont(Theme::getMonospaceFont(9));

    m_diskPasswordCaption = new QLabel("password:", m_diskStatusRow);
    m_diskPasswordCaption->setStyleSheet(QString("color: %1;").arg(Subtext().name()));

    m_diskPasswordEdit = new QLineEdit(m_diskStatusRow);
    m_diskPasswordEdit->setEchoMode(QLineEdit::Password);
    m_diskPasswordEdit->setFont(Theme::getMonospaceFont(9));
    m_diskPasswordEdit->setFixedWidth(100);
    m_diskPasswordEdit->setStyleSheet(QString(
        "QLineEdit { background-color: %1; color: %2; border: 1px solid %3; border-radius: 0px; padding: 2px 6px; }"
    ).arg(Base().name(), Text().name(), Surface1().name()));
    connect(m_diskPasswordEdit, &QLineEdit::returnPressed, this, &PropertiesDialog::onDiskPasswordEntered);

    m_diskSudoErrorLabel = new QLabel(m_diskStatusRow);
    m_diskSudoErrorLabel->setStyleSheet(QString("color: %1;").arg(Red().name()));

    statusLayout->addWidget(m_diskScanChip);
    statusLayout->addWidget(m_diskNeededChip);
    statusLayout->addWidget(m_diskBarLabel);
    statusLayout->addWidget(m_diskPasswordCaption);
    statusLayout->addWidget(m_diskPasswordEdit);
    statusLayout->addWidget(m_diskSudoErrorLabel);
    statusLayout->addStretch(1);
    m_diskStatusRow->hide();
    diskLayout->addWidget(m_diskStatusRow);

    m_diskConsole = new QTextEdit(m_diskUsageGroup);
    m_diskConsole->setReadOnly(true);
    m_diskConsole->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {
        QFont diskMono = Theme::getMonospaceFont(9);
        m_diskConsole->setFont(diskMono);
        m_diskConsole->document()->setDefaultFont(diskMono);
    }
    m_diskConsole->setLineWrapMode(QTextEdit::NoWrap);
    m_diskConsole->setStyleSheet(QString(
        "QTextEdit { background-color: %1; color: %2; border: 1px solid %3; border-radius: 0px; padding: 6px; }"
    ).arg(Base().name(), Text().name(), Surface1().name()));
    m_diskConsole->setMinimumHeight(200);
    // контекстное меню консоли в стиле tui приложения, а не системный попап ос
    m_diskConsole->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_diskConsole, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(m_diskConsole);
        menu.setFont(Theme::getMonospaceFont(9));
        menu.setStyleSheet(Theme::getApplicationStyleSheet());
        QAction* actCopy = menu.addAction("Copy\tCtrl+C");
        actCopy->setEnabled(m_diskConsole->textCursor().hasSelection());
        connect(actCopy, &QAction::triggered, m_diskConsole, &QTextEdit::copy);
        QAction* actSelectAll = menu.addAction("Select All\tCtrl+A");
        connect(actSelectAll, &QAction::triggered, m_diskConsole, &QTextEdit::selectAll);
        menu.exec(m_diskConsole->mapToGlobal(pos));
    });
    diskLayout->addWidget(m_diskConsole);
    m_diskUsageGroup->hide();
    mainLayout->addWidget(m_diskUsageGroup);


    // права: консольные маркеры tui [ ] / [x], без системных галочек ос (guideline 1.4)
    // без рамки и ободка: заголовок только название, поверх обычного qwidget
    auto permGroup = new QWidget(this);
    permGroup->setStyleSheet(QString(
        "QCheckBox { background: transparent; spacing: 0px; color: %1; } "
        "QCheckBox::indicator { width: 0px; height: 0px; border: none; background: transparent; }"
    ).arg(Text().name()));
    auto permLayout = new QVBoxLayout(permGroup);
    permLayout->setContentsMargins(0, 0, 0, 0);
    permLayout->setSpacing(8);
    permLayout->addWidget(makeSectionHeader(QStringLiteral("PERMISSIONS"), permGroup));

    auto permGrid = new QGridLayout();
    permGrid->setHorizontalSpacing(16);
    permGrid->setVerticalSpacing(4);

    permGrid->addWidget(new QLabel("Read", this), 0, 1);
    permGrid->addWidget(new QLabel("Write", this), 0, 2);
    permGrid->addWidget(new QLabel("Execute", this), 0, 3);

    auto makeTuiBox = [this](QCheckBox*& box) {
        box = new QCheckBox(QStringLiteral("[ ]"), this);
        box->setFont(Theme::getMonospaceFont(10, QFont::Bold));
        box->setCursor(Qt::PointingHandCursor);
        connect(box, &QCheckBox::toggled, [box](bool checked) {
            box->setText(checked ? QStringLiteral("[x]") : QStringLiteral("[ ]"));
        });
    };

    permGrid->addWidget(new QLabel("Owner:", this), 1, 0);
    makeTuiBox(m_userRead);
    makeTuiBox(m_userWrite);
    makeTuiBox(m_userExec);
    permGrid->addWidget(m_userRead, 1, 1);
    permGrid->addWidget(m_userWrite, 1, 2);
    permGrid->addWidget(m_userExec, 1, 3);

    permGrid->addWidget(new QLabel("Group:", this), 2, 0);
    makeTuiBox(m_groupRead);
    makeTuiBox(m_groupWrite);
    makeTuiBox(m_groupExec);
    permGrid->addWidget(m_groupRead, 2, 1);
    permGrid->addWidget(m_groupWrite, 2, 2);
    permGrid->addWidget(m_groupExec, 2, 3);

    permGrid->addWidget(new QLabel("Others:", this), 3, 0);
    makeTuiBox(m_otherRead);
    makeTuiBox(m_otherWrite);
    makeTuiBox(m_otherExec);
    permGrid->addWidget(m_otherRead, 3, 1);
    permGrid->addWidget(m_otherWrite, 3, 2);
    permGrid->addWidget(m_otherExec, 3, 3);

    m_octalLabel = new QLabel("Octal: 0755", this);
    m_octalLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(Accent().name()));
    permGrid->addWidget(m_octalLabel, 4, 0, 1, 4);

    permLayout->addLayout(permGrid);
    mainLayout->addWidget(permGroup);

    // кнопки
    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    m_applyBtn = new QPushButton("Apply Permissions", this);
    m_closeBtn = new QPushButton("Close", this);
    // кнопки autodefault отключены: enter в инлайн поле пароля sudo не
    // должен «нажимать» apply/close, submit только у самого поля
    m_applyBtn->setAutoDefault(false);
    m_closeBtn->setAutoDefault(false);

    connect(m_applyBtn, &QPushButton::clicked, this, &PropertiesDialog::applyPermissions);
    connect(m_closeBtn, &QPushButton::clicked, this, [this]() {
        if (m_embedded) {
            emit closeRequested();
        } else {
            accept();
        }
    });

    btnLayout->addWidget(m_applyBtn);
    btnLayout->addWidget(m_closeBtn);
    mainLayout->addLayout(btnLayout);
}

void PropertiesDialog::loadFileInfo() {
    auto pathToQ = [](const std::filesystem::path& p) { return QString::fromUtf8(p.c_str()); };
    QString displayName = pathToQ(m_path.filename());
    if (displayName.isEmpty()) displayName = pathToQ(m_path);
    m_nameLabel->setText(displayName);
    m_locationLabel->setText(pathToQ(m_path.parent_path()));

    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForFile(pathToQ(m_path));
    m_typeLabel->setText(mime.name() + " (" + mime.comment() + ")");

    struct stat st;
    bool isDir = false;
    if (lstat(m_path.c_str(), &st) == 0) {
        isDir = S_ISDIR(st.st_mode);
        m_diskUsageShown = isDir && isMountRoot(m_path);
        if (m_diskUsageShown) {
            QStorageInfo storage(QString::fromUtf8(m_path.c_str()));
            const QString fsType = storage.isValid() ? QString::fromLatin1(storage.fileSystemType()) : QString();
            m_typeLabel->setText(fsType.isEmpty() ? QStringLiteral("Drive") : QString("Drive (%1)").arg(fsType));
            if (storage.isValid() && storage.bytesTotal() > 0) {
                m_sizeLabel->setText(QString("%1 (%2 free)").arg(
                    Core::XdgUtils::formatSize(storage.bytesTotal()),
                    Core::XdgUtils::formatSize(storage.bytesAvailable())));
            } else {
                m_sizeLabel->setText(QStringLiteral("Drive"));
            }
        } else {
            m_sizeLabel->setText(isDir ? QStringLiteral("…") : Core::XdgUtils::formatSize(st.st_size));
        }

        std::tm tmVal;
        localtime_r(&st.st_mtime, &tmVal);
        std::ostringstream ss;
        ss << std::put_time(&tmVal, "%Y-%m-%d %H:%M:%S");
        m_modifiedLabel->setText(QString::fromStdString(ss.str()));

        m_inodeLabel->setText(QString("Inode: %1 | Hardlinks: %2").arg(st.st_ino).arg(st.st_nlink));

        struct passwd* pw = getpwuid(st.st_uid);
        struct group* gr = getgrgid(st.st_gid);
        QString owner = pw ? QString::fromUtf8(pw->pw_name) : QString::number(st.st_uid);
        QString grp = gr ? QString::fromUtf8(gr->gr_name) : QString::number(st.st_gid);
        m_ownerLabel->setText(owner + " : " + grp);

        // чекбоксы
        m_userRead->setChecked(st.st_mode & S_IRUSR);
        m_userWrite->setChecked(st.st_mode & S_IWUSR);
        m_userExec->setChecked(st.st_mode & S_IXUSR);

        m_groupRead->setChecked(st.st_mode & S_IRGRP);
        m_groupWrite->setChecked(st.st_mode & S_IWGRP);
        m_groupExec->setChecked(st.st_mode & S_IXGRP);

        m_otherRead->setChecked(st.st_mode & S_IROTH);
        m_otherWrite->setChecked(st.st_mode & S_IWOTH);
        m_otherExec->setChecked(st.st_mode & S_IXOTH);

        mode_t oct = st.st_mode & 0777;
        m_octalLabel->setText(QString("Octal: 0%1").arg(oct, 3, 8, QChar('0')));
    }

    // disk usage: консоль автоматически для корня диска/фс (и только для
    // каталогов); счётчик джобов отменяет результаты предыдущего скана
    if (m_diskUsageGroup) {
        if (isDir && m_diskUsageShown) {
            m_diskUsageGroup->show();
            startDiskUsageScan();
        } else {
            ++m_diskScanJob;
            stopDiskUsageProc();
            m_diskScanning = false;
            m_diskSudoQueueRunning = false;
            m_diskUsageGroup->hide();
        }
    }
    // компактные свойства: вместо консоли только вес папки в поле size
    if (isDir && !m_diskUsageShown) {
        startPathWeightScan();
    } else {
        stopPathWeightProc();
    }
}

// вес каталога для компактных свойств: один асинхронный du sb
// при отказе молчаливая проба sudo только если жив
// тикет sudo и никаких диалогов пароля из окна свойств
void PropertiesDialog::startPathWeightScan() {
    stopPathWeightProc();
    ++m_weightJob;
    m_sizeLabel->setText(QStringLiteral("…"));
    m_sizeLabel->setToolTip(QString());
    runPathWeightProc(false);
}

void PropertiesDialog::runPathWeightProc(bool viaSudo) {
    const uint64_t job = m_weightJob.load();
    auto* proc = new QProcess(this);
    m_weightProc = proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C.UTF-8");
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, job, viaSudo, proc](int code, QProcess::ExitStatus status) {
        m_weightProc = nullptr;
        proc->deleteLater();
        if (m_weightJob.load() != job) return;

        const QByteArray out = proc->readAllStandardOutput();
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        qint64 bytes = -1;
        {
            const int tab = out.indexOf('\t');
            const QByteArray num = tab > 0 ? out.left(tab) : out;
            bool ok = false;
            const qint64 v = num.trimmed().toLongLong(&ok);
            if (ok) bytes = v;
        }
        // du печатает итог даже при частичных ошибках доступа и берем его
        if (bytes >= 0 && status == QProcess::NormalExit) {
            m_sizeLabel->setText(Core::XdgUtils::formatSize(uintmax_t(bytes)));
            if (code != 0 && !err.isEmpty()) m_sizeLabel->setToolTip(err);
            return;
        }
        if (!viaSudo && Core::XdgUtils::sudoTicketAlive()) {
            runPathWeightProc(true);
            return;
        }
        m_sizeLabel->setText(QStringLiteral("?"));
        if (!err.isEmpty()) m_sizeLabel->setToolTip(err);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError e) {
        if (m_weightJob.load() != job || e != QProcess::FailedToStart) return;
        if (m_weightProc) {
            m_weightProc->deleteLater();
            m_weightProc = nullptr;
        }
        m_sizeLabel->setText(QStringLiteral("?"));
    });

    QStringList args{"du", "-s", "-b", "--", QString::fromUtf8(m_path.c_str())};
    if (viaSudo) args = Core::XdgUtils::rootArgs(args);
    proc->start(args.first(), args.mid(1));
}

void PropertiesDialog::stopPathWeightProc() {
    if (!m_weightProc) return;
    m_weightProc->disconnect(this);
    if (m_weightProc->state() != QProcess::NotRunning) {
        m_weightProc->kill();
        m_weightProc->waitForFinished(200);
    }
    m_weightProc->deleteLater();
    m_weightProc = nullptr;
}

// disk usage: шапка фс и заголовок таблицы видны сразу, `du` течёт
// асинхронно (readyread в поток gui), веса папок и полоса прогресса
// наполняются по ходу. закрытые подпапки показываются «?» и измеряются
// последними (авторизация sudo или полкит ниже), без повторного полного
// скана; если не вышло даже с sudo остаётся «?» и легенда в конце
namespace {
constexpr int kDiskScanCapMs = 30000;      // лимит по простою: тишина du без вывода
constexpr int kDiskScanHardCapMs = 600000; // абсолютный лимит фазы (10 минут)
constexpr int kDiskBatchCapMs = 300000;    // мягкий лимит батча закрытых папок
constexpr int kDiskRenderThrottleMs = 100; // троттлинг перерисовки таблицы
constexpr int kDiskCapRearmMs = 5000;      // перезапуск таймера простоя не чаще
constexpr int kDiskParallelMax = 8;        // одновременных du на медленных фс



// stderr du: «permission denied» для конкретных путей; полные строки копятся
// в buf (хвост дожидаётся следующего чанка), путь сворачивается до первого
// компонента под корнем (сам корень флагом); true, если появилось новое
// ограничение. используется и одиночным du, и параллельной волной
bool extractRestrictionsFromErr(QString& buf, const QString& rootClean,
                                QSet<QString>& restricted, bool& rootRestricted) {
    bool newRestriction = false;
    int nl = -1;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QString line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (!line.contains(QLatin1String("Permission denied"))) continue;
        const int q1 = line.indexOf('\'');
        const int q2 = line.lastIndexOf('\'');
        if (q1 < 0 || q2 <= q1) continue;
        const QString path = QDir::cleanPath(line.mid(q1 + 1, q2 - q1 - 1));
        if (path == rootClean) {
            if (!rootRestricted) newRestriction = true;
            rootRestricted = true;
            continue;
        }
        const QString prefix = rootClean.endsWith('/') ? rootClean : rootClean + '/';
        if (path.startsWith(prefix)) {
            const QString comp = path.mid(prefix.size()).section('/', 0, 0);
            if (!comp.isEmpty() && !restricted.contains(comp)) {
                newRestriction = true;
                restricted.insert(comp);
            }
        }
    }
    return newRestriction;
}

}

// убивает текущий процесс du (если жив) и отключает его сигналы: рестарт
// скана, уход на файл, разрушение диалога; заодно гасит параллельную волну
void PropertiesDialog::stopDiskUsageProc() {
    if (!m_diskParallelProcs.isEmpty()) {
        const auto procs = m_diskParallelProcs;
        m_diskParallelProcs.clear();
        m_diskParallelBufs.clear();
        m_diskParallelErrBufs.clear();
        for (QProcess* proc : procs) {
            proc->disconnect(this);
            if (proc->state() != QProcess::NotRunning) {
                proc->kill();
                proc->waitForFinished(200);
            }
            proc->deleteLater();
        }
    }
    m_diskParallelQueue.clear();
    m_diskParallelMode = false;

    if (!m_diskProc) return;
    QProcess* proc = m_diskProc;
    m_diskProc = nullptr;
    proc->disconnect(this);
    if (proc->state() != QProcess::NotRunning) {
        proc->kill();
        proc->waitForFinished(200);
    }
    proc->deleteLater();
}

// отдельный остановщик привилегированной фазы закрытых папок не нужен: её
// процесс живёт в m_diskproc (общий потоковый парсер), stopdiskusageproc гасит и его

void PropertiesDialog::startDiskUsageScan() {
    if (!m_diskConsole) return;

    stopDiskUsageProc();
    const uint64_t job = ++m_diskScanJob;

    // состояние скана заново
    m_diskChildren.clear();
    m_diskSizes.clear();
    m_diskResolved.clear();
    m_diskRestricted.clear();
    m_diskMountChildren.clear();
    m_diskMountWaveDone = false;
    m_diskUnknown.clear();
    m_diskProgressChildren.clear();
    m_diskOutBuf.clear();
    m_diskErrBuf.clear();
    m_diskDoneDirs = 0;
    m_diskTotalDirs = 0;
    m_diskScanning = true;
    m_diskSudoPhase = false;
    m_diskRootRestricted = false;
    m_diskRenderScheduled = false;
    m_diskSudoQueue.clear();
    m_diskSudoQueueTotal = 0;
    m_diskSudoQueueRunning = false;
    m_diskSudoReady = false;
    m_diskSudoNeeded = false;
    m_diskSudoPkexec = false;
    m_diskPkexecAuthFailed = false;
    m_diskPkexecWaiting = false;
    m_diskSudoDeclined = false;
    m_diskSudoBatchRunning = false;
    m_diskNormalDone = false;
    m_diskBarShownPct = -1;
    m_diskFrozenPct = -2;
    m_diskQueueBasePct = 0;
    if (m_diskPasswordEdit) m_diskPasswordEdit->clear();
    if (m_diskSudoErrorLabel) m_diskSudoErrorLabel->clear();

    m_diskRootClean = QDir::cleanPath(QString::fromUtf8(m_path.c_str()));
    if (m_diskRootClean.isEmpty()) m_diskRootClean = QString::fromUtf8(m_path.c_str());

    // шапка файловой системы мгновенно, до всякого du (диск виден сразу)
    QString fsTypeStr;
    {
        using namespace Theme::Colors;
        QStorageInfo storage(QString::fromUtf8(m_path.c_str()));
        const qint64 totalBytes = storage.bytesTotal();
        const qint64 availBytes = storage.bytesAvailable();
        const qint64 usedBytes = (totalBytes > availBytes) ? (totalBytes - availBytes) : 0;
        const bool storageValid = storage.isValid();
        const QString devStr = storageValid ? QString::fromLatin1(storage.device()) : QString();
        fsTypeStr = storageValid ? QString::fromLatin1(storage.fileSystemType()) : QString();
        const QString mountRootStr = storageValid ? storage.rootPath() : QString();

        const int diskPct = (totalBytes > 0)
            ? std::clamp(static_cast<int>((usedBytes * 100) / totalBytes), 0, 100)
            : 0;
        constexpr int capBarLen = 16;
        const int capFilled = (diskPct * capBarLen) / 100;

        QString base;
        QTextStream ts(&base);
        ts << "Filesystem: " << (devStr.isEmpty() ? QString("-") : devStr)
           << " (" << (fsTypeStr.isEmpty() ? QString("-") : fsTypeStr) << ")\n";
        ts << "Mount:      " << (mountRootStr.isEmpty() ? QString("-") : mountRootStr) << "\n";
        ts << "Capacity:   " << Core::XdgUtils::formatSize(usedBytes) << " / "
           << Core::XdgUtils::formatSize(totalBytes) << "  [<span style='color:" << Accent().name() << ";'>"
           << QString(capFilled, QChar(0x2588)) << "</span><span style='color:" << Surface2().name() << ";'>"
           << QString(capBarLen - capFilled, QChar(0x2591)) << "</span>] <span style='color:" << Accent().name()
           << "; font-weight:bold;'>" << diskPct << "%</span>\n";
        ts << "Available:  " << Core::XdgUtils::formatSize(availBytes) << " free\n\n";
        m_diskBaseHtml = base;
    }

    // подкаталоги первого уровня: для счетчика прогресса и строк со знаком вопроса
    // уровень что и du первого уровня а симлинки du не обходит и мы пропускаем
    // в счетчик прогресса идут только каталоги той же файловой системы
    // иначе счетчик застревал бы на детях точки монтирования
    {
        struct stat rootSt {};
        const bool haveRootSt = (::stat(m_path.c_str(), &rootSt) == 0);
        std::error_code ec;
        std::filesystem::directory_iterator it(
            m_path, std::filesystem::directory_options::skip_permission_denied, ec);
        if (!ec) {
            const std::filesystem::directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                const auto& entry = *it;
                std::error_code tec;
                if (entry.is_symlink(tec) || !entry.is_directory(tec)) continue;
                const QString name = QString::fromUtf8(entry.path().filename().c_str());
                if (name.isEmpty()) continue;
                m_diskChildren.insert(name);
                struct stat est {};
                const bool haveEst = (::stat(entry.path().c_str(), &est) == 0);
                if (haveRootSt && haveEst && est.st_dev == rootSt.st_dev) {
                    ++m_diskTotalDirs;
                    m_diskProgressChildren.insert(name);
                }
                // виртуальные файловые системы ядра сразу 0 байт
                // другую файловую систему или сабволюм du пропускает
                // и их домерит отдельная волна без root
                const QStorageInfo childStorage(QString::fromUtf8(entry.path().c_str()));
                const QString childClean = QDir::cleanPath(QString::fromUtf8(entry.path().c_str()));
                if (childStorage.isValid() && isVirtualKernelFs(childStorage)) {
                    m_diskSizes.insert(name, 0);
                    m_diskResolved.insert(name);
                } else if ((childStorage.isValid()
                            && QDir::cleanPath(childStorage.rootPath()) == childClean
                            && childClean != m_diskRootClean)
                           || (haveRootSt && haveEst && est.st_dev != rootSt.st_dev)) {
                    m_diskMountChildren.insert(name);
                }
            }
        }
    }

    renderDiskUsage(); // шапка + заголовок таблицы + чип scanning сразу

    // медленные файловые системы ntfs через fuse на которых du тормозит
    // последовательный du по всему корню слишком медленный
    // поэтому подпапки первого уровня сканируются параллельной волной du
    const bool slowFs = fsTypeStr.contains(QLatin1String("ntfs"), Qt::CaseInsensitive)
                     || fsTypeStr.contains(QLatin1String("fuseblk"), Qt::CaseInsensitive);
    if (slowFs && m_diskChildren.size() >= 2) {
        startParallelDiskScan();
        return;
    }

    auto* proc = new QProcess(this);
    m_diskProc = proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C.UTF-8");
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, job]() {
        if (m_diskScanJob.load() == job) onDiskDuOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, job]() {
        if (m_diskScanJob.load() == job) collectDiskDuErrors();
    });
    connect(proc, &QProcess::finished, this, [this, job](int code, QProcess::ExitStatus) {
        if (m_diskScanJob.load() == job) onDiskDuFinished(false, code);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError e) {
        if (m_diskScanJob.load() != job || e != QProcess::FailedToStart) return;
        onDiskDuFinished(false, -1); // например, du не установлен «?» + легенда
    });
    proc->start("du", {"-x", "-d", "1", "-b", "--null", "--", QString::fromUtf8(m_path.c_str())});
    armDiskScanCap(job, false);
}

// гасит процессы текущей фазы (одиночный du и параллельная волна): по
// сигналам finished фаза финализируется строками «?». очередь волны
// очищается до kill: иначе onparalleldudone освободившегося слота запустил бы
// следующие du, и кап (idle/жёсткий) не смог бы завершить фазу
void PropertiesDialog::killDiskPhaseProcs() {
    m_diskParallelQueue.clear();
    if (m_diskProc && m_diskProc->state() != QProcess::NotRunning) {
        m_diskProc->kill();
    }
    for (QProcess* proc : m_diskParallelProcs) {
        if (proc->state() != QProcess::NotRunning) proc->kill();
    }
}

// лимиты фазы: кап по простою убивает процесс после паузы в выводе
// а медленные файловые системы вроде ntfs сканируют минуты но не молчат
// таймер привязан к фазе и смена фазы обнуляет старый таймер
void PropertiesDialog::armDiskScanCap(uint64_t job, bool sudoPhase) {
    m_diskCapClock.start();
    QPointer<PropertiesDialog> self(this);
    QTimer::singleShot(kDiskScanCapMs, this, [self, job, sudoPhase]() {
        if (!self || self->m_diskScanJob.load() != job || !self->m_diskScanning) return;
        if (self->m_diskSudoPhase != sudoPhase) return; // фаза сменилась, таймер устарел
        // pkexec ждет ответа системного диалога а батч du выдает первую
        // запись только в конце каталога и тишина тут не признак зависания
        if (self->m_diskSudoPkexec || self->m_diskSudoBatchRunning) return;
        self->killDiskPhaseProcs();
    });

    if (m_diskHardCapJob != job || m_diskHardCapPhase != sudoPhase) {
        m_diskHardCapJob = job;
        m_diskHardCapPhase = sudoPhase;
        QTimer::singleShot(kDiskScanHardCapMs, this, [self, job, sudoPhase]() {
            if (!self || self->m_diskScanJob.load() != job || !self->m_diskScanning) return;
            if (self->m_diskSudoPhase != sudoPhase) return;
            self->killDiskPhaseProcs();
        });
    }
}

// du жив и продолжает сыпать данные, кап по простою продлевается (но не абсолютный)
void PropertiesDialog::rearmDiskScanCapIfActive() {
    const bool mainActive = m_diskProc && m_diskProc->state() != QProcess::NotRunning;
    const bool parallelActive = !m_diskParallelProcs.isEmpty();
    if (!m_diskScanning || (!mainActive && !parallelActive)) return;
    if (m_diskCapClock.isValid() && m_diskCapClock.elapsed() < kDiskCapRearmMs) return;
    armDiskScanCap(m_diskScanJob.load(), m_diskSudoPhase);
}

    // полная фаза root только для закрытого корня когда листинг невозможен
    // без root: потоковый привилегированный du где живой тикет запускает sudo
    // молча иначе один pkexec системный диалог пароля а отказ диалога разбирает
    // declinesudoauthorization и повторно не спрашиваем
void PropertiesDialog::startSudoDiskUsage() {
    const QStringList duCmd{"du", "-x", "-d", "1", "-b", "--null", "--", QString::fromUtf8(m_path.c_str())};
    startSudoPhaseProc(m_diskSudoPkexec ? Core::XdgUtils::pkexecArgs(duCmd)
                                        : Core::XdgUtils::rootArgs(duCmd), false);
}

// батч закрытых папок: один привилегированный вызов на весь добор
// со всеми путями где одна авторизация полкита дает один системный диалог
// записи того же формата что у потокового du поэтому парсер и таблица общие
void PropertiesDialog::startSudoBatch() {
    // путь du подставляется в скрипт sh, который исполняется как root:
    // path пользователя не должен влиять на то, что запустится от root
    QString duExe = Core::XdgUtils::resolveTrustedExe(QStringLiteral("du"));
    if (duExe.isEmpty()) duExe = QStringLiteral("/usr/bin/du");
    const QString script = QStringLiteral(
        "for p in \"$@\"; do \"%1\" -s -b --null -- \"$p\" || true; done").arg(duExe);
    QStringList cmd{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script, QStringLiteral("_")};
    for (const QString& name : m_diskSudoQueue) {
        cmd << (m_diskRootClean.endsWith('/') ? m_diskRootClean + name
                                              : m_diskRootClean + '/' + name);
    }
    startSudoPhaseProc(m_diskSudoPkexec ? Core::XdgUtils::pkexecArgs(cmd)
                                        : Core::XdgUtils::rootArgs(cmd), true);
}

// общий запуск привилегированной фазы: процесс живёт в m_diskproc, поэтому
// парсер и таблица работают без отдельной ветки а batch равен true для закрытых
// папок где запись появляется только в конце каталога поэтому
// вместо капа по простою используется мягкий общий кап
void PropertiesDialog::startSudoPhaseProc(const QStringList& argv, bool batch) {
    stopDiskUsageProc();
    m_diskSudoPhase = true;
    m_diskSudoBatchRunning = batch;
    m_diskSudoQueueRunning = batch;
    m_diskSudoNeeded = false;
    m_diskPkexecAuthFailed = false;
    // pkexec откроет системный диалог пароля ещё до первой записи: бар
    // замирает на последнем показанном значении, а чип scanning сменяется на ожидание sudo
    m_diskPkexecWaiting = m_diskSudoPkexec;
    m_diskFrozenPct = -2;
    m_diskScanning = true;
    m_diskDoneDirs = 0;
    if (!batch) {
        m_diskRestricted.clear();
        m_diskRootRestricted = false;
    }
    m_diskOutBuf.clear();
    m_diskErrBuf.clear();
    updateDiskStatusRow();
    scheduleDiskRender();

    const uint64_t job = m_diskScanJob.load();
    auto* proc = new QProcess(this);
    m_diskProc = proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C.UTF-8");
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, job]() {
        if (m_diskScanJob.load() == job) onDiskDuOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, job]() {
        if (m_diskScanJob.load() == job) collectDiskDuErrors();
    });
    connect(proc, &QProcess::finished, this, [this, job](int code, QProcess::ExitStatus) {
        if (m_diskScanJob.load() == job) onDiskDuFinished(true, code);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError e) {
        if (m_diskScanJob.load() != job || e != QProcess::FailedToStart) return;
        onDiskDuFinished(true, -1); // например, pkexec/du не установлен
    });
    proc->start(argv.first(), argv.mid(1));
    armDiskScanCap(job, true);
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: sudo phase start method=[%1] batch=%2 args=%3")
        .arg(argv.first()).arg(batch ? 1 : 0).arg(argv.size()));

    if (batch) {
        QPointer<PropertiesDialog> self(this);
        QTimer::singleShot(kDiskBatchCapMs, this, [self, job, proc]() {
            if (!self || self->m_diskScanJob.load() != job) return;
            if (self->m_diskProc == proc && proc->state() != QProcess::NotRunning) {
                proc->kill(); // finished доведёт добор до финала (частичные «?»)
            }
        });
    }
}

// ================= параллельная волна: медленные фс (ntfs/fuse) =================

// очередь подпапок первого уровня и одновременный запуск du
// записи того же формата что у одиночного вызова поэтому таблица работает без изменений
void PropertiesDialog::startParallelDiskScan() {
    QStringList names(m_diskChildren.cbegin(), m_diskChildren.cend());
    names.sort(Qt::CaseInsensitive);
    if (names.isEmpty()) {
        onDiskDuFinished(false, -1); // подпапок нет сразу финал
        return;
    }
    startDiskChildWave(names);
}

// волна поштучных du без root по указанным подпапкам
// ntfs все дети сразу а для остальных файловых систем дети точки монтирования
// которых потоковый du пропускает как чужую файловую систему а ошибки доступа
// отправляют папку в список закрытых
// её домерит очередь sudo
void PropertiesDialog::startDiskChildWave(const QStringList& names) {
    m_diskParallelMode = true;
    m_diskParallelBufs.clear();
    m_diskParallelErrBufs.clear();
    for (const QString& name : names) {
        m_diskParallelQueue << (m_diskRootClean.endsWith('/')
            ? m_diskRootClean + name
            : m_diskRootClean + '/' + name);
    }
    spawnParallelDu();
}

void PropertiesDialog::spawnParallelDu() {
    const uint64_t job = m_diskScanJob.load();
    while (!m_diskParallelQueue.isEmpty() && m_diskParallelProcs.size() < kDiskParallelMax) {
        const QString childPath = m_diskParallelQueue.takeFirst();
        auto* proc = new QProcess(this);
        m_diskParallelProcs.append(proc);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("LC_ALL", "C.UTF-8");
        proc->setProcessEnvironment(env);
        proc->setProcessChannelMode(QProcess::SeparateChannels);
        connect(proc, &QProcess::readyReadStandardOutput, this, [this, job, proc]() {
            if (m_diskScanJob.load() == job) onParallelDuOutput(proc);
        });
        connect(proc, &QProcess::readyReadStandardError, this, [this, job, proc]() {
            if (m_diskScanJob.load() == job) onParallelDuError(proc);
        });
        connect(proc, &QProcess::finished, this, [this, job, proc](int, QProcess::ExitStatus) {
            if (m_diskScanJob.load() == job) onParallelDuDone(proc);
        });
        connect(proc, &QProcess::errorOccurred, this, [this, job, proc](QProcess::ProcessError e) {
            if (m_diskScanJob.load() == job && e == QProcess::FailedToStart) onParallelDuDone(proc);
        });
        proc->start("du", {"-s", "-b", "-x", "--null", "--", childPath});
    }
    armDiskScanCap(job, m_diskSudoPhase); // кап по простою следит и за волной
}

// поток stdout одного du волны: записи nul держим в раздельном буфере процесса,
// иначе частичные записи разных процессов в общем буфере склеились бы неверно
void PropertiesDialog::onParallelDuOutput(QProcess* proc) {
    if (!m_diskParallelMode || !proc) return;
    QByteArray& buf = m_diskParallelBufs[proc];
    buf += proc->readAllStandardOutput();
    int pos = 0;
    bool gotRecord = false;
    while (true) {
        const int nul = buf.indexOf('\0', pos);
        if (nul < 0) break;
        if (parseDiskDuRecord(buf.mid(pos, nul - pos))) gotRecord = true;
        pos = nul + 1;
    }
    if (pos > 0) buf.remove(0, pos);
    if (gotRecord) {
        scheduleDiskRender();
        rearmDiskScanCapIfActive();
    }
}

// ошибки доступа одного процесса волны отправляют путь в закрытые
// где очередь sudo домерит закрытые а для ntfs волны это единственный путь
void PropertiesDialog::onParallelDuError(QProcess* proc) {
    if (!m_diskParallelMode || !proc) return;
    m_diskParallelErrBufs[proc] += QString::fromUtf8(proc->readAllStandardError());
    rearmDiskScanCapIfActive(); // stderr du тоже признак живого скана
    const bool newRestriction = extractRestrictionsFromErr(
        m_diskParallelErrBufs[proc], m_diskRootClean, m_diskRestricted, m_diskRootRestricted);
    if (newRestriction) {
        Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: wave restriction restricted=[%1]")
            .arg(QStringList(m_diskRestricted.cbegin(), m_diskRestricted.cend()).join(',')));
        if (!m_diskSudoPhase && !m_diskSudoQueueRunning) noteDiskSudoNeeded();
    }
}

// один du волны закончил: дочитываем хвост, освобождаем слот, при пустой
// очереди и без активных процессов финал фазы (как у одиночного du)
void PropertiesDialog::onParallelDuDone(QProcess* proc) {
    if (!m_diskParallelMode || !proc) return;
    if (!m_diskParallelProcs.contains(proc)) return;
    m_diskParallelProcs.removeOne(proc);

    onParallelDuError(proc); // хвост stderr до удаления процесса
    m_diskParallelErrBufs.remove(proc);
    const QByteArray tail = proc->readAllStandardOutput();
    proc->deleteLater();
    int pos = 0;
    bool gotRecord = false;
    while (true) {
        const int nul = tail.indexOf('\0', pos);
        if (nul < 0) break;
        if (parseDiskDuRecord(tail.mid(pos, nul - pos))) gotRecord = true;
        pos = nul + 1;
    }
    if (pos == 0 && !tail.trimmed().isEmpty()) parseDiskDuRecord(tail.trimmed());

    if (gotRecord) scheduleDiskRender();

    if (m_diskParallelProcs.isEmpty() && m_diskParallelQueue.isEmpty()) {
        m_diskParallelMode = false;
        onDiskDuFinished(false, -1);
    } else {
        spawnParallelDu();
    }
}

// разбор потока stdout du: записи nul «<байты>\t<путь>» могут прийти
// по частям, хвост держим в буфере до следующего readyread
void PropertiesDialog::onDiskDuOutput() {
    if (!m_diskProc) return;
    m_diskOutBuf += m_diskProc->readAllStandardOutput();
    int pos = 0;
    bool gotRecord = false;
    while (true) {
        const int nul = m_diskOutBuf.indexOf('\0', pos);
        if (nul < 0) break;
        if (parseDiskDuRecord(m_diskOutBuf.mid(pos, nul - pos))) gotRecord = true;
        pos = nul + 1;
    }
    if (pos > 0) m_diskOutBuf.remove(0, pos);
    if (gotRecord) {
        scheduleDiskRender();
        rearmDiskScanCapIfActive(); // du жив, кап по простою продлевается
    }
}

bool PropertiesDialog::parseDiskDuRecord(const QByteArray& rec) {
    if (rec.isEmpty()) return false;
    const int tabPos = rec.indexOf('\t');
    if (tabPos <= 0) return false;
    bool numOk = false;
    const qint64 sz = rec.left(tabPos).trimmed().toLongLong(&numOk);
    if (!numOk) return false;
    const QString dirPath = QDir::cleanPath(QString::fromUtf8(rec.mid(tabPos + 1)).trimmed());
    if (dirPath.isEmpty() || dirPath == m_diskRootClean) return false; // итоговая строка корня
    const QString base = dirPath.section('/', -1);
    if (base.isEmpty()) return false;
    const bool firstResolution = !m_diskResolved.contains(base);
    if (m_diskSudoPhase && m_diskPkexecWaiting) {
        // первая запись привилегированной фазы = авторизация пройдена:
        // снимаем «замороженное» ожидание диалога полкита
        m_diskPkexecWaiting = false;
        m_diskFrozenPct = -2;
    }
    m_diskSizes.insert(base, sz);
    m_diskResolved.insert(base);
    m_diskChildren.insert(base); // sudo увидел то, что листинг не смог (закрытый корень)
    m_diskRestricted.remove(base); // размер этой папки привилегированная фаза уже снял
    // прогресс только по детям той же фс (знаменатель m_disktotaldirs) и
    // ровно один раз: повторный замер в фазе sudo и дети точки
    // монтирования которых du пропускает счетчик больше не раздувают
    if (firstResolution && m_diskProgressChildren.contains(base)) ++m_diskDoneDirs;
    return true;
}

// stderr du одиночной/привилегированной фазы. как только закрытая папка
// впервые замечена (в обычной фазе), сразу идёт подготовка авторизации
// (notedisksudoneeded): живой тикет молча, полкит системный диалог
// откроет сам pkexec при измерении; чип инлайн «needed sudo» с полем пароля
// только если полкит недоступен или отказал. здесь же ловятся маркеры stderr
// отказа pkexec (диалог отклонён / нет агента), их разберёт финал фазы
void PropertiesDialog::collectDiskDuErrors() {
    if (!m_diskProc) return;
    m_diskErrBuf += QString::fromUtf8(m_diskProc->readAllStandardError());
    rearmDiskScanCapIfActive(); // stderr du тоже признак живого скана
    if (m_diskSudoPkexec && !m_diskPkexecAuthFailed
        && (m_diskErrBuf.contains(QLatin1String("Request dismissed"))
            || m_diskErrBuf.contains(QLatin1String("Not authorized"))
            || m_diskErrBuf.contains(QLatin1String("No authentication agent"))
            || m_diskErrBuf.contains(QLatin1String("Error executing command as another user")))) {
        m_diskPkexecAuthFailed = true;
    }
    const bool newRestriction = extractRestrictionsFromErr(
        m_diskErrBuf, m_diskRootClean, m_diskRestricted, m_diskRootRestricted);
    if (newRestriction && !m_diskSudoPhase && !m_diskSudoQueueRunning) {
        Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: restriction root=%1 restricted=[%2]")
            .arg(m_diskRootRestricted ? 1 : 0)
            .arg(QStringList(m_diskRestricted.cbegin(), m_diskRestricted.cend()).join(',')));
        noteDiskSudoNeeded();
    }
}

// первое ограничение доступа: авторизация ровно один раз на скан
// живой тикет молча через sudo иначе
// ветка полкита где системный диалог открывается сам в момент измерения
// отказ диалога разбирает declinesudoauthorization
// а без полкита сразу строка инлайн ввода пароля sudo
void PropertiesDialog::noteDiskSudoNeeded() {
    if (m_diskSudoReady || m_diskSudoNeeded || m_diskSudoPhase || m_diskSudoDeclined) return;
    const bool ticketAlive = Core::XdgUtils::sudoTicketAlive();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: auth needed job=%1 ticket=%2 polkit=%3")
        .arg(m_diskScanJob.load()).arg(ticketAlive ? 1 : 0)
        .arg(Core::XdgUtils::polkitAvailable() ? 1 : 0));
    if (ticketAlive) {
        m_diskSudoReady = true;
        m_diskSudoPkexec = false;
        updateDiskStatusRow();
        // проба тикета крутит вложенный event loop: если du успел завершиться
        // прямо во время неё, обычная фаза уже закрыта и ждать её конца больше
        // некому; добор закрытых папок стартует отсюда (иначе «?» навсегда)
        if (m_diskNormalDone && !m_diskScanning) startRestrictedMeasurement();
        return;
    }

    if (Core::XdgUtils::polkitAvailable()) {
        // готовность без диалога: батч pkexec сам откроет системный диалог
        // ровно один раз. m_disksudoready=true сразу повторные permission
        // denied от продолжающегося du не зовут нотификацию снова
        m_diskSudoReady = true;
        m_diskSudoPkexec = true;
        updateDiskStatusRow();
        if (m_diskNormalDone && !m_diskScanning) startRestrictedMeasurement();
        return;
    }

    m_diskSudoNeeded = true;
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: inline password prompt shown (no polkit)"));
    if (m_diskPasswordEdit && isActiveWindow()) m_diskPasswordEdit->setFocus();
    updateDiskStatusRow();
}

// ввод инлайн пароля: строгая проверка пароля и контроль тикета
// успех прячет строку и запускает измерение закрытых папок
// а отказ выводит красную строку ошибки
void PropertiesDialog::onDiskPasswordEntered() {
    if (!m_diskPasswordEdit) return;
    const QString pass = m_diskPasswordEdit->text();
    m_diskPasswordEdit->clear();
    if (pass.isEmpty()) return;

    QString err;
    const bool ok = Core::XdgUtils::sudoApplyPassword(pass, err);
    if (ok) {
        m_diskSudoReady = true;
        m_diskSudoNeeded = false;
        m_diskSudoPkexec = false; // пароль создал тикет и дальше работает sudo
        m_diskSudoErrorLabel->clear();
        updateDiskStatusRow();
        if (m_diskNormalDone && !m_diskScanning) startRestrictedMeasurement();
    } else {
        // лаконичная строка ошибки рядом с полем (полный текст в подсказке):
        // до ~11 знаков, чтобы строка не обрезалась даже посреди скана
        QString shortErr = err;
        if (shortErr.contains(QLatin1String("incorrect password"))) shortErr = QStringLiteral("wrong pass");
        else if (shortErr.contains(QLatin1String("no response"))) shortErr = QStringLiteral("timeout");
        else if (shortErr.contains(QLatin1String("not installed"))) shortErr = QStringLiteral("no sudo");
        else if (shortErr.contains(QLatin1String("ticket was not created"))) shortErr = QStringLiteral("no ticket");
        else if (shortErr.contains(QLatin1String("failed to execute"))) shortErr = QStringLiteral("no exec");
        if (shortErr.isEmpty()) shortErr = QStringLiteral("sudo denied");
        m_diskSudoErrorLabel->setText(shortErr);
        m_diskSudoErrorLabel->setToolTip(err);
        updateDiskStatusRow();
        if (!m_diskScanning) {
            m_diskPasswordEdit->selectAll();
            m_diskPasswordEdit->setFocus();
        }
    }
}

void PropertiesDialog::onDiskDuFinished(bool sudoPhase, int exitCode) {
    if (sudoPhase != m_diskSudoPhase) return; // сигнал от уже завершённой фазы
    onDiskDuOutput();      // добираем остаток stdout
    collectDiskDuErrors(); // и хвост stderr

    if (sudoPhase) {
        // фаза полкита без единой записи и с отказом авторизации (код pkexec
        // 126/127, маркеры stderr «request dismissed»/«not authorized») это
        // ответ пользователя, а не «пустой каталог»: инлайн пароль
        // повторно не показываем, строки «?» остаются (declinesudoauthorization);
        // фолбек инлайн только когда полкит реально недоступен (нет агента или
        // провал запуска); авторизованный пустой каталог даёт exit 0,
        // финализируем как обычно
        if (m_diskSudoPkexec && m_diskDoneDirs == 0) {
            const bool pkexecUnavailable = exitCode < 0
                || m_diskErrBuf.contains(QLatin1String("No authentication agent"));
            if (pkexecUnavailable) {
                fallbackToInlinePassword(QStringLiteral("unavailable exit=%1").arg(exitCode));
                return;
            }
            const bool pkexecRefused = m_diskPkexecAuthFailed
                || m_diskErrBuf.contains(QLatin1String("Request dismissed"))
                || m_diskErrBuf.contains(QLatin1String("Not authorized"))
                || exitCode == 126 || exitCode == 127;
            if (pkexecRefused) {
                declineSudoAuthorization(QStringLiteral("dismissed/no auth exit=%1").arg(exitCode));
                return;
            }
        }
        // привилегированная фаза закончена, финал
        finalizeDiskUsage();
        return;
    }

    // обычная фаза закончена. строки «?» закрытых папок встают в конец
    // таблицы; измеряются они последними и только после пароля sudo (или
    // сразу, если тикет уже жив), повторного полного скана нет
    stopDiskUsageProc();
    m_diskNormalDone = true;
    m_diskScanning = false;
    recomputeDiskUnknown();

    // дети точки монтирования на другой файловой системе которых потоковый du пропустил
    // домеряем поштучно без root а закрытые уйдут в очередь sudo
    if (!m_diskMountWaveDone && !m_diskSudoPhase) {
        QStringList pendingMounts;
        for (const QString& name : m_diskMountChildren) {
            if (!m_diskResolved.contains(name) && !m_diskRestricted.contains(name)) {
                pendingMounts << name;
            }
        }
        if (!pendingMounts.isEmpty()) {
            m_diskMountWaveDone = true;
            m_diskScanning = true; // скан продолжается волной
            Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: mount wave [%1]")
                .arg(pendingMounts.join(',')));
            updateDiskStatusRow();
            scheduleDiskRender();
            startDiskChildWave(pendingMounts);
            return;
        }
    }

    const bool restrictedSeen = m_diskRootRestricted || !m_diskRestricted.isEmpty();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: normal done restricted=%1 ready=%2 needed=%3 pkexec=%4")
        .arg(restrictedSeen ? 1 : 0)
        .arg(m_diskSudoReady ? 1 : 0)
        .arg(m_diskSudoNeeded ? 1 : 0)
        .arg(m_diskSudoPkexec ? 1 : 0));
    if (restrictedSeen && m_diskSudoReady) {
        startRestrictedMeasurement();
        return;
    }
    if (restrictedSeen) m_diskSudoNeeded = true;
    updateDiskStatusRow();
    renderDiskUsage();
}

// вторая волна: только закрытые папки и последними
// для закрытого корня потоковый привилегированный du по корню
// иначе один батч du по всем закрытым подпапкам
// где живой тикет использует sudo иначе один диалог pkexec на весь батч
void PropertiesDialog::startRestrictedMeasurement() {
    m_diskSudoNeeded = false;
    if (m_diskSudoErrorLabel) m_diskSudoErrorLabel->clear();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: restricted start root=%1 restricted=[%2]")
        .arg(m_diskRootRestricted ? 1 : 0)
        .arg(QStringList(m_diskRestricted.cbegin(), m_diskRestricted.cend()).join(',')));

    if (m_diskRootRestricted || m_diskChildren.isEmpty()) {
        startSudoDiskUsage();
        return;
    }

    m_diskSudoQueue = m_diskRestricted.values();
    m_diskSudoQueue.sort(Qt::CaseInsensitive);
    if (m_diskSudoQueue.isEmpty()) {
        finalizeDiskUsage();
        return;
    }
    // шкала продолжается с последнего показанного значения без отката
    m_diskQueueBasePct = std::clamp(m_diskBarShownPct, 0, 100);
    m_diskSudoQueueTotal = m_diskSudoQueue.size();
    startSudoBatch();
}

// привилегированная фаза не состоялась из за недоступности полкита
// показываем поле инлайн пароля где ручной ввод создаст тикет после чего
// добор перезапустится через sudo без полкита
void PropertiesDialog::fallbackToInlinePassword(const QString& reason) {
    stopDiskUsageProc();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: pkexec unavailable (%1) -> inline password").arg(reason));
    m_diskSudoPkexec = false;
    m_diskPkexecWaiting = false;
    m_diskFrozenPct = -2;
    m_diskSudoReady = false;
    m_diskSudoNeeded = true;
    m_diskSudoPhase = false;
    m_diskSudoBatchRunning = false;
    m_diskSudoQueueRunning = false;
    m_diskScanning = false;
    if (m_diskSudoErrorLabel) m_diskSudoErrorLabel->clear();
    updateDiskStatusRow();
    renderDiskUsage();
    if (m_diskPasswordEdit && isActiveWindow()) m_diskPasswordEdit->setFocus();
}

// авторизацию в системном диалоге полкита отклонили (126/127, «request
// dismissed»/«not authorized») это окончательный ответ пользователя:
// второй запрос пароля (строка инлайн) не показываем, недоступные папки
// остаются со знаком вопроса а вернуться к добору можно вручную через тикет
// в терминале и открыть свойства заново
void PropertiesDialog::declineSudoAuthorization(const QString& reason) {
    stopDiskUsageProc();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: polkit declined (%1)").arg(reason));
    m_diskSudoPkexec = false;
    m_diskPkexecWaiting = false;
    m_diskFrozenPct = -2;
    m_diskSudoReady = false;
    m_diskSudoNeeded = false;
    m_diskSudoDeclined = true;
    m_diskSudoPhase = false;
    m_diskSudoBatchRunning = false;
    m_diskSudoQueueRunning = false;
    m_diskScanning = false;
    if (m_diskSudoErrorLabel) {
        m_diskSudoErrorLabel->setText(QStringLiteral("sudo denied"));
        m_diskSudoErrorLabel->setToolTip(QStringLiteral(
            "Authorization was declined. Unreadable folders stay \"?\".\n"
            "Create a sudo ticket in a terminal (sudo -v), then reopen Properties."));
    }
    recomputeDiskUnknown();
    updateDiskStatusRow();
    renderDiskUsage();
}

// размер не получен если нет записи du или запись неполная (доступ закрыт
// в последней выполненной фазе); живёт на нескольких стадиях (обычная фаза и
// ожидание пароля и добор через sudo), поэтому пересчитывается каждым шагом
void PropertiesDialog::recomputeDiskUnknown() {
    m_diskUnknown.clear();
    for (const QString& c : m_diskChildren) {
        if (m_diskResolved.contains(c) && !m_diskRestricted.contains(c)) continue;
        m_diskUnknown.insert(c);
    }
}

// финал скана: измерили всё, что смогли; «?» остаются только там, где нет
// доступа даже с sudo; чип и строка пароля гаснут
void PropertiesDialog::finalizeDiskUsage() {
    m_diskScanning = false;
    m_diskSudoPhase = false;
    m_diskSudoQueueRunning = false;
    m_diskSudoBatchRunning = false;
    m_diskSudoNeeded = false;
    m_diskSudoPkexec = false;
    m_diskPkexecWaiting = false;
    m_diskFrozenPct = -2;
    stopDiskUsageProc();
    recomputeDiskUnknown();
    Core::XdgUtils::sudoDebugLog(QStringLiteral("disk: finalized unknown=[%1]")
        .arg(QStringList(m_diskUnknown.cbegin(), m_diskUnknown.cend()).join(',')));
    updateDiskStatusRow();
    renderDiskUsage();
}

// строка статуса над консолью. пока ждём пароль, плашка needed sudo встаёт
// ровно на место чипа scanning (та же ширина), а полоса прогресса не
// сбрасывается: замирает на последнем показанном значении; после пароля добор
// закрытых папок продолжает шкалу с той же точки до 100%;
// всё скрыто, когда работа закончена
void PropertiesDialog::updateDiskStatusRow() {
    if (!m_diskStatusRow) return;
    const bool needPw = m_diskSudoNeeded && !m_diskSudoReady;
    // pkexec открыл системный диалог пароля: до первой записи du бар замер,
    // чип scanning уступает место «needed sudo» как при пароле инлайн
    const bool authWait = m_diskPkexecWaiting && m_diskScanning && !needPw;
    const bool declined = m_diskSudoDeclined;
    m_diskStatusRow->setVisible(m_diskScanning || needPw || declined);

    // место чипа scanning: пока ждём пароль/полкит, его занимает needed sudo
    m_diskScanChip->setVisible(m_diskScanning && !needPw && !authWait && !declined);
    m_diskNeededChip->setVisible((needPw || authWait) && !declined);

    // значение бара: замороженное / продолжение очереди / обычная фаза
    int pct = 0;
    bool indeterminate = false;
    if (needPw || authWait) {
        if (m_diskFrozenPct == -2) {
            // морозим ровно то, что было на экране в последнем кадре
            m_diskFrozenPct = (m_diskBarShownPct >= 0) ? m_diskBarShownPct : -1;
        }
        indeterminate = (m_diskFrozenPct < 0);
        pct = indeterminate ? 0 : m_diskFrozenPct;
    } else if (declined) {
        indeterminate = true; // бар скрыт; значение не используется
    } else if (m_diskSudoQueueRunning && m_diskSudoQueueTotal > 0) {
        const int base = std::clamp(m_diskQueueBasePct, 0, 100);
        int done = 0;
        for (const QString& n : m_diskSudoQueue) {
            if (m_diskResolved.contains(n)) ++done;
        }
        const int doneClamped = qMin(done, m_diskSudoQueueTotal);
        pct = base + ((100 - base) * doneClamped) / m_diskSudoQueueTotal;
    } else if (m_diskTotalDirs > 0) {
        pct = std::clamp((m_diskDoneDirs * 100) / m_diskTotalDirs, 0, 100);
    } else {
        indeterminate = true;
    }

    const bool barVisible = !declined && (m_diskScanning || needPw);
    if (barVisible) {
        using namespace Theme::Colors;
        constexpr int barLen = 16;
        const int filled = (pct * barLen) / 100;
        QString bar;
        QTextStream cs(&bar);
        cs << "[" << "<span style=\"color:" << Accent().name() << ";\">"
           << QString(filled, QChar(0x2588)) << "</span><span style=\"color:" << Surface2().name()
           << ";\">" << QString(barLen - filled, QChar(0x2591)) << "</span>] ";
        if (!indeterminate) {
            cs << "<span style=\"color:" << Accent().name() << "; font-weight:bold;\">" << pct << "%</span>";
        } else {
            cs << "<span style=\"color:" << Subtext().name() << ";\">…</span>";
        }
        m_diskBarLabel->setText(bar);
        m_diskBarShownPct = indeterminate ? -1 : pct;
    }
    m_diskBarLabel->setVisible(barVisible);

    // «password:» уступает место строке ошибки, чтобы всё влезало в одну строку;
    // отказ полкита (declined) та же строка ошибки, но без поля пароля
    m_diskPasswordCaption->setVisible(needPw && !declined && m_diskSudoErrorLabel->text().isEmpty());
    m_diskPasswordEdit->setVisible(needPw && !declined);
    m_diskSudoErrorLabel->setVisible(!m_diskSudoErrorLabel->text().isEmpty()
                                     && (declined || needPw));
}

// троттлинг перерисовки: du сыплет сотни записей, html пересобираем
// не чаще, чем раз в kdiskrenderthrottlems (финал рисуется сразу)
void PropertiesDialog::scheduleDiskRender() {
    if (m_diskRenderScheduled) return;
    const qint64 elapsed = m_diskLastRender.isValid() ? m_diskLastRender.elapsed() : 1000;
    const int delay = elapsed >= kDiskRenderThrottleMs
        ? 0
        : static_cast<int>(kDiskRenderThrottleMs - elapsed);
    m_diskRenderScheduled = true;
    QPointer<PropertiesDialog> self(this);
    const uint64_t job = m_diskScanJob.load();
    QTimer::singleShot(delay, this, [self, job]() {
        if (!self) return;
        self->m_diskRenderScheduled = false;
        if (self->m_diskScanJob.load() != job) return;
        self->m_diskLastRender.restart();
        self->renderDiskUsage();
    });
}

void PropertiesDialog::renderDiskUsage() {
    if (!m_diskConsole) return;

    // чип scanning + бар + «needed sudo» живут в отдельном виджете строки,
    // синхронизируем её на каждой перерисовке
    updateDiskStatusRow();

    QScrollBar* vbar = m_diskConsole->verticalScrollBar();
    const int scrollPos = vbar ? vbar->value() : 0;

    using namespace Theme::Colors;
    const QString accentHex = Accent().name();
    const QString s1Hex = Surface1().name();
    const QString s2Hex = Surface2().name();
    const QString textHex = Text().name();
    const QString dimHex = Subtext().name();

    QString html;
    QTextStream ts(&html);
    ts << "<pre style=\"margin: 0; line-height: 1.4;\">";
    ts << m_diskBaseHtml;

    ts << "<span style='color:" << s1Hex << ";'>" << QString(62, '-') << "</span>\n";
    ts << "<span style='color:" << accentHex
       << "; font-weight:bold;'>      SIZE    SHARE  USAGE         DIRECTORY</span>\n";
    ts << "<span style='color:" << s1Hex << ";'>" << QString(62, '-') << "</span>\n";

    // строки: крупнейшие сверху; строки «?» (недоступные/неизмеренные) в конце
    QStringList known;
    QStringList unknownList;
    for (const QString& c : m_diskChildren) {
        if (m_diskUnknown.contains(c)) unknownList << c;
        else if (m_diskResolved.contains(c)) known << c;
        // ребёнок без записи du во время скана не показывается: «?» появляется
        // только в финале (недоступные/неизмеренные), без мусорных «0 b»
    }
    std::sort(known.begin(), known.end(), [this](const QString& a, const QString& b) {
        const qint64 sa = m_diskSizes.value(a, 0);
        const qint64 sb = m_diskSizes.value(b, 0);
        if (sa != sb) return sa > sb;
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    std::sort(unknownList.begin(), unknownList.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });

    qint64 sumKnown = 0;
    for (const QString& c : known) sumKnown += m_diskSizes.value(c, 0);

    if (!known.isEmpty() || !unknownList.isEmpty()) {
        for (const QString& name : known) {
            const qint64 sz = m_diskSizes.value(name, 0);
            const QString sizeCol = QString("%1").arg(Core::XdgUtils::formatSize(sz), 10);
            const double share = (sumKnown > 0)
                ? (static_cast<double>(sz) * 100.0 / static_cast<double>(sumKnown))
                : 0.0;
            const QString shareCol = QString("%1").arg(QString("%1%").arg(share, 5, 'f', 1), 8);
            int subFilled = std::clamp(static_cast<int>(std::lround(share / 10.0)), 0, 10);
            if (sz > 0 && subFilled == 0) subFilled = 1;

            ts << "<span style='color:" << textHex << ";'>" << sizeCol << "</span> "
               << "<span style='color:" << accentHex << ";'>" << shareCol << "</span>  "
               << "[<span style='color:" << accentHex << ";'>" << QString(subFilled, QChar(0x2588))
               << "</span><span style='color:" << s2Hex << ";'>" << QString(10 - subFilled, QChar(0x2591))
               << "</span>]  " << name.toHtmlEscaped() << "\n";
        }
        for (const QString& name : unknownList) {
            ts << "<span style='color:" << dimHex << ";'>" << QString("?").rightJustified(10) << " "
               << QString("?").rightJustified(8) << "  [" << QString(10, QChar(0x2591)) << "]  "
               << name.toHtmlEscaped() << "</span>\n";
        }
    } else if (!m_diskScanning && !m_diskRootRestricted) {
        ts << "<span style='color:" << dimHex << ";'>  (no subdirectories)</span>\n";
    }

    // легенда в конце: единственное объяснение строк «?» и недоступного корня
    if (!m_diskScanning && (!m_diskUnknown.isEmpty() || m_diskRootRestricted)) {
        ts << "<span style='color:" << dimHex << ";'>? - can't read or no permission</span>\n";
    }
    ts << "</pre>";

    m_diskConsole->setHtml(html);
    if (vbar) vbar->setValue(qMin(scrollPos, vbar->maximum()));
}

void PropertiesDialog::applyPermissions() {
    mode_t mode = 0;
    if (m_userRead->isChecked()) mode |= S_IRUSR;
    if (m_userWrite->isChecked()) mode |= S_IWUSR;
    if (m_userExec->isChecked()) mode |= S_IXUSR;

    if (m_groupRead->isChecked()) mode |= S_IRGRP;
    if (m_groupWrite->isChecked()) mode |= S_IWGRP;
    if (m_groupExec->isChecked()) mode |= S_IXGRP;

    if (m_otherRead->isChecked()) mode |= S_IROTH;
    if (m_otherWrite->isChecked()) mode |= S_IWOTH;
    if (m_otherExec->isChecked()) mode |= S_IXOTH;

    struct stat st;
    if (lstat(m_path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            TuiConfirmPopup::alert(this, "Notice", "Cannot change permissions of symbolic links.");
            return;
        }
        // сохраняем s_isuid, s_isgid, s_isvtx
        mode_t preservedBits = st.st_mode & (S_ISUID | S_ISGID | S_ISVTX);
        mode |= preservedBits;
    }

    if (chmod(m_path.c_str(), mode) == 0) {
        m_octalLabel->setText(QString("Octal: 0%1 (Applied!)").arg(mode & 0777, 3, 8, QChar('0')));
    } else {
        TuiConfirmPopup::alert(this, "Error", "Failed to update permissions (check sudo/ownership).");
    }
}

}
