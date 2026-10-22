#include "ConnectionDialog.hpp"
#include "../atlifm-core/Credentials.hpp"
#include "../atlifm-core/SshConfig.hpp"
#include "../atlifm-theme/Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace Widgets {

namespace {

QLabel* fieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::getMonospaceFont(8, QFont::Bold));
    label->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Subtext().name()));
    return label;
}

bool splitHostPort(const QString& input, QString* host, int* port) {
    const QString text = input.trimmed();
    if (text.isEmpty()) return false;

    if (text.startsWith(QLatin1Char('['))) {
        const int close = text.indexOf(QLatin1Char(']'));
        if (close <= 1) return false;
        const QString tail = text.mid(close + 1);
        if (!tail.startsWith(QLatin1Char(':'))) return false;
        bool ok = false;
        const int value = tail.mid(1).toInt(&ok);
        if (!ok || value <= 0 || value > 65535) return false;
        *host = text.mid(1, close - 1);
        *port = value;
        return true;
    }

    if (text.count(QLatin1Char(':')) != 1) return false;
    const int colon = text.indexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    bool ok = false;
    const int value = text.mid(colon + 1).toInt(&ok);
    if (!ok || value <= 0 || value > 65535) return false;
    *host = text.left(colon);
    *port = value;
    return true;
}

}

ConnectionDialog::ConnectionDialog(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("connectionDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(520, 470);
    setupUi();
}

void ConnectionDialog::setupUi() {
    using namespace Theme::Colors;

    setStyleSheet(QString(
        "QWidget#connectionDialog { background-color: %1; border: 1px solid %2; }")
        .arg(Base().name(), Accent().name()));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("NETWORK CONNECTION"), this);
    title->setFont(Theme::getMonospaceFont(12, QFont::Bold));
    title->setStyleSheet(QString("color: %1;").arg(Accent().name()));
    outer->addWidget(title);

    auto* form = new QGridLayout();
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(7);
    form->setColumnStretch(1, 1);

    int row = 0;
    form->addWidget(fieldLabel(QStringLiteral("Protocol"), this), row, 0);
    m_protocol = new QComboBox(this);
    m_protocol->addItem(QStringLiteral("SFTP"), static_cast<int>(Core::Network::Protocol::Sftp));
    m_protocol->addItem(QStringLiteral("FTP"), static_cast<int>(Core::Network::Protocol::Ftp));
    m_protocol->addItem(QStringLiteral("FTPS"), static_cast<int>(Core::Network::Protocol::Ftps));
    m_protocol->addItem(QStringLiteral("FTPS (implicit)"), static_cast<int>(Core::Network::Protocol::Ftpi));
    m_protocol->addItem(QStringLiteral("SMB"), static_cast<int>(Core::Network::Protocol::Smb));
    form->addWidget(m_protocol, row++, 1);

    form->addWidget(fieldLabel(QStringLiteral("Host / IP"), this), row, 0);
    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(QStringLiteral("host, host:port or server.local"));
    form->addWidget(m_host, row++, 1);

    form->addWidget(fieldLabel(QStringLiteral("Port"), this), row, 0);
    m_port = new QLineEdit(this);
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setPlaceholderText(QStringLiteral("22"));
    form->addWidget(m_port, row++, 1);

    form->addWidget(fieldLabel(QStringLiteral("Path"), this), row, 0);
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(QStringLiteral("folder or share/folder (optional)"));
    form->addWidget(m_path, row++, 1);

    form->addWidget(fieldLabel(QStringLiteral("User"), this), row, 0);
    m_username = new QLineEdit(this);
    m_username->setPlaceholderText(QStringLiteral("optional"));
    form->addWidget(m_username, row++, 1);

    m_passwordLabel = fieldLabel(QStringLiteral("Password"), this);
    form->addWidget(m_passwordLabel, row, 0);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("optional"));
    form->addWidget(m_password, row++, 1);

    m_sshKeyLabel = fieldLabel(QStringLiteral("SSH key"), this);
    form->addWidget(m_sshKeyLabel, row, 0);
    m_sshKeyRow = new QWidget(this);
    auto* keyLayout = new QHBoxLayout(m_sshKeyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(6);
    m_sshKey = new QLineEdit(m_sshKeyRow);
    m_sshKey->setPlaceholderText(QStringLiteral("optional"));
    m_sshKey->setToolTip(QStringLiteral("Private key for SFTP; the password field is used for its passphrase."));
    keyLayout->addWidget(m_sshKey, 1);
    m_browseKey = new QPushButton(QStringLiteral("..."), m_sshKeyRow);
    m_browseKey->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_browseKey->setCursor(Qt::PointingHandCursor);
    m_browseKey->setFixedWidth(34);
    m_browseKey->setToolTip(QStringLiteral("Choose SSH private key file"));
    keyLayout->addWidget(m_browseKey);
    form->addWidget(m_sshKeyRow, row++, 1);

    m_rememberPassword = new QCheckBox(QStringLiteral("Remember password"), this);
    form->addWidget(m_rememberPassword, row++, 1);

    m_domainLabel = fieldLabel(QStringLiteral("Domain"), this);
    form->addWidget(m_domainLabel, row, 0);
    m_domain = new QLineEdit(this);
    m_domain->setPlaceholderText(QStringLiteral("optional, for SMB"));
    form->addWidget(m_domain, row++, 1);

    m_anonymous = new QCheckBox(QStringLiteral("Anonymous / guest"), this);
    form->addWidget(m_anonymous, row++, 1);

    m_acceptHostKey = new QCheckBox(QStringLiteral("Accept unknown SSH host key"), this);
    form->addWidget(m_acceptHostKey, row++, 1);

    outer->addLayout(form);
    outer->addStretch(1);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setFont(Theme::getMonospaceFont(8));
    m_error->setStyleSheet(QString("color: %1;").arg(Theme::Colors::Red().name()));
    m_error->hide();
    outer->addWidget(m_error);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setFont(Theme::getMonospaceFont(9));
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setStyleSheet(QString(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 6px 16px; }"
        "QPushButton:hover { color: %4; border-color: %4; }")
        .arg(Surface0().name(), Text().name(), Surface1().name(), Accent().name()));
    buttons->addWidget(m_cancel);

    m_connect = new QPushButton(QStringLiteral("Connect"), this);
    m_connect->setFont(Theme::getMonospaceFont(9, QFont::Bold));
    m_connect->setCursor(Qt::PointingHandCursor);
    m_connect->setStyleSheet(QString(
        "QPushButton { background: %1; color: %2; border: 1px solid %1; padding: 6px 18px; }"
        "QPushButton:hover { background: %3; }")
        .arg(Accent().name(), Base().name(), SelectionBg().name()));
    buttons->addWidget(m_connect);
    outer->addLayout(buttons);

    const QString inputStyle = QString(
        "QLineEdit, QComboBox { background-color: %1; color: %2; border: 1px solid %3;"
        " padding: 5px 7px; selection-background-color: %4; selection-color: %5; }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QComboBox QAbstractItemView { background-color: %1; color: %2;"
        " selection-background-color: %4; selection-color: %5; }")
        .arg(Surface0().name(), Text().name(), Surface1().name(),
             SelectionBg().name(), SelectionFg().name());
    m_host->setStyleSheet(inputStyle);
    m_port->setStyleSheet(inputStyle);
    m_path->setStyleSheet(inputStyle);
    m_username->setStyleSheet(inputStyle);
    m_password->setStyleSheet(inputStyle);
    m_sshKey->setStyleSheet(inputStyle);
    m_domain->setStyleSheet(inputStyle);
    m_protocol->setStyleSheet(inputStyle);
    m_protocol->setFont(Theme::getMonospaceFont(9));
    m_browseKey->setStyleSheet(QString(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 5px 6px; }"
        "QPushButton:hover { color: %4; border-color: %4; }")
        .arg(Surface0().name(), Text().name(), Surface1().name(), Accent().name()));
    m_anonymous->setFont(Theme::getMonospaceFont(9));
    m_anonymous->setStyleSheet(QString("QCheckBox { color: %1; spacing: 8px; }")
                                   .arg(Text().name()));
    m_acceptHostKey->setFont(Theme::getMonospaceFont(9));
    m_acceptHostKey->setStyleSheet(QString("QCheckBox { color: %1; spacing: 8px; }")
                                       .arg(Text().name()));
    m_rememberPassword->setFont(Theme::getMonospaceFont(9));
    m_rememberPassword->setStyleSheet(QString("QCheckBox { color: %1; spacing: 8px; }")
                                          .arg(Text().name()));
    m_rememberPassword->setVisible(Core::Network::credentialsAvailable());

    for (QLineEdit* edit : {m_host, m_port, m_path, m_username, m_password, m_sshKey, m_domain}) {
        edit->setFont(Theme::getMonospaceFont(9));
        edit->installEventFilter(this);
        connect(edit, &QLineEdit::returnPressed, this, &ConnectionDialog::submit);
    }
    connect(m_host, &QLineEdit::editingFinished, this, &ConnectionDialog::applyHostPort);
    connect(m_sshKey, &QLineEdit::textChanged, this, [this]() { updatePasswordHint(); });
    connect(m_browseKey, &QPushButton::clicked, this, &ConnectionDialog::chooseSshKey);
    auto fillStoredPassword = [this]() {
        if (m_busy || m_anonymous->isChecked() || !m_password->text().isEmpty()) return;
        const auto stored = Core::Network::lookupPassword(location());
        if (!stored.isEmpty()) {
            m_password->setText(stored);
            m_rememberPassword->setChecked(true);
        }
    };
    connect(m_host, &QLineEdit::editingFinished, this, fillStoredPassword);
    connect(m_username, &QLineEdit::editingFinished, this, fillStoredPassword);
    connect(m_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateProtocolFields(); });
    connect(m_anonymous, &QCheckBox::toggled, this, [this]() {
        updateSensitive();
    });
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (m_busy) return;
        hide();
        emit closeRequested();
    });
    connect(m_connect, &QPushButton::clicked, this, &ConnectionDialog::submit);

    m_lastProtocol = Core::Network::Protocol::Sftp;
    updateSensitive();
    updateProtocolFields();
}

bool ConnectionDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !m_busy) {
            hide();
            emit closeRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ConnectionDialog::open(const Core::Network::Location& location) {
    m_error->clear();
    m_error->hide();
    setBusy(false);

    const int index = m_protocol->findData(static_cast<int>(location.protocol));
    m_protocol->blockSignals(true);
    m_protocol->setCurrentIndex(index >= 0 ? index : 0);
    m_protocol->blockSignals(false);
    m_lastProtocol = location.protocol;

    m_host->setText(location.host);
    m_port->setText(QString::number(
        location.port > 0 ? location.port : Core::Network::defaultPort(location.protocol)));
    applyHostPort();
    m_path->setText(location.path);
    m_username->setText(location.username);
    m_password->setText(location.password);
    m_sshKey->setText(location.sshKeyPath);
    if (!location.anonymous && location.sshKeyPath.trimmed().isEmpty() &&
        location.protocol == Core::Network::Protocol::Sftp) {
        const QString remembered = Core::Network::rememberedSshKey(location);
        if (!remembered.isEmpty()) m_sshKey->setText(remembered);
    }
    m_domain->setText(location.domain);
    m_anonymous->setChecked(location.anonymous);
    m_acceptHostKey->setChecked(location.acceptUnknownHostKey);
    m_rememberPassword->setChecked(false);
    if (!location.anonymous && location.password.isEmpty() &&
        Core::Network::credentialsAvailable()) {
        const auto stored = Core::Network::lookupPassword(location);
        if (!stored.isEmpty()) {
            m_password->setText(stored);
            m_rememberPassword->setChecked(true);
        }
    } else if (!location.anonymous && !location.password.isEmpty()) {
        m_rememberPassword->setChecked(location.rememberPassword);
    }
    updateSensitive();
    updateProtocolFields();
    updatePasswordHint();
    focusPrimaryInput();
}

void ConnectionDialog::showError(const QString& message) {
    setBusy(false);
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

void ConnectionDialog::setBusy(bool busy) {
    m_busy = busy;
    m_connect->setText(busy ? QStringLiteral("Connecting…") : QStringLiteral("Connect"));
    m_connect->setEnabled(!busy);
    m_cancel->setEnabled(!busy);
    m_protocol->setEnabled(!busy);
    m_host->setEnabled(!busy);
    m_port->setEnabled(!busy);
    m_path->setEnabled(!busy);
    m_username->setEnabled(!busy && !m_anonymous->isChecked());
    m_password->setEnabled(!busy && !m_anonymous->isChecked());
    m_sshKey->setEnabled(!busy && !m_anonymous->isChecked());
    m_browseKey->setEnabled(!busy && !m_anonymous->isChecked());
    m_domain->setEnabled(!busy && !m_anonymous->isChecked());
    m_anonymous->setEnabled(!busy);
    m_acceptHostKey->setEnabled(!busy);
    m_rememberPassword->setEnabled(!busy && !m_anonymous->isChecked());
    updateSensitive();
    if (busy) {
        m_error->clear();
        m_error->hide();
    }
}

Core::Network::Location ConnectionDialog::location() const {
    Core::Network::Location location;
    location.protocol = static_cast<Core::Network::Protocol>(
        m_protocol->currentData().toInt());
    location.host = m_host->text().trimmed();
    int port = m_port->text().trimmed().toInt();
    QString host;
    if (splitHostPort(location.host, &host, &port)) {
        location.host = host;
    }
    location.port = port > 0 ? port : Core::Network::defaultPort(location.protocol);
    location.path = m_path->text().trimmed();
    location.username = m_username->text().trimmed();
    location.password = m_password->text();
    location.sshKeyPath = m_sshKey->text().trimmed();
    location.domain = m_domain->text().trimmed();
    location.anonymous = m_anonymous->isChecked();
    location.acceptUnknownHostKey = m_acceptHostKey->isChecked();
    location.rememberPassword = m_rememberPassword->isChecked();
    return location;
}

void ConnectionDialog::focusPrimaryInput() {
    if (m_host->text().trimmed().isEmpty()) {
        m_host->setFocus(Qt::OtherFocusReason);
    } else if (!m_anonymous->isChecked()) {
        m_password->setFocus(Qt::OtherFocusReason);
        m_password->selectAll();
    } else {
        m_connect->setFocus(Qt::OtherFocusReason);
    }
}

void ConnectionDialog::updateSensitive() {
    const bool anonymous = m_anonymous->isChecked();
    const auto protocol = static_cast<Core::Network::Protocol>(
        m_protocol->currentData().toInt());
    const bool smb = protocol == Core::Network::Protocol::Smb;
    const bool sftp = protocol == Core::Network::Protocol::Sftp;
    m_username->setEnabled(!m_busy && !anonymous);
    m_password->setEnabled(!m_busy && !anonymous);
    m_sshKey->setEnabled(!m_busy && !anonymous && sftp);
    m_browseKey->setEnabled(!m_busy && !anonymous && sftp);
    m_sshKey->setVisible(sftp);
    m_sshKeyLabel->setVisible(sftp);
    m_sshKeyRow->setVisible(sftp);
    m_domain->setEnabled(!m_busy && !anonymous && smb);
    m_domain->setVisible(smb);
    m_domainLabel->setVisible(smb);
    m_acceptHostKey->setVisible(sftp);
    m_acceptHostKey->setEnabled(!m_busy && sftp);
    m_rememberPassword->setEnabled(!m_busy && !anonymous);
}

void ConnectionDialog::updateProtocolFields() {
    const auto protocol = static_cast<Core::Network::Protocol>(
        m_protocol->currentData().toInt());
    const int previousDefault = Core::Network::defaultPort(m_lastProtocol);
    const int port = m_port->text().trimmed().toInt();
    if (m_port->text().trimmed().isEmpty() || port == previousDefault || port <= 0) {
        m_port->setText(QString::number(Core::Network::defaultPort(protocol)));
    }
    m_port->setPlaceholderText(QString::number(Core::Network::defaultPort(protocol)));
    m_lastProtocol = protocol;
    if (protocol == Core::Network::Protocol::Smb) {
        m_path->setPlaceholderText(QStringLiteral("share/folder (optional)"));
        m_domain->show();
        m_domainLabel->show();
    } else if (protocol == Core::Network::Protocol::Ftp ||
               protocol == Core::Network::Protocol::Ftps ||
               protocol == Core::Network::Protocol::Ftpi) {
        m_path->setPlaceholderText(QStringLiteral("/folder (optional)"));
        m_domain->hide();
        m_domainLabel->hide();
    } else {
        m_path->setPlaceholderText(QStringLiteral("/folder (optional)"));
        m_domain->hide();
        m_domainLabel->hide();
    }
    updateSensitive();
    updatePasswordHint();
}

void ConnectionDialog::updatePasswordHint() {
    const auto protocol = static_cast<Core::Network::Protocol>(
        m_protocol->currentData().toInt());
    const bool keyAuth = protocol == Core::Network::Protocol::Sftp &&
                         !m_sshKey->text().trimmed().isEmpty();
    m_passwordLabel->setText(keyAuth ? QStringLiteral("Passphrase")
                                     : QStringLiteral("Password"));
    m_password->setPlaceholderText(keyAuth ? QStringLiteral("key passphrase (optional)")
                                           : QStringLiteral("optional"));
    m_password->setToolTip(keyAuth ? QStringLiteral("Passphrase for the selected SSH key")
                                   : QString());
    m_rememberPassword->setText(keyAuth ? QStringLiteral("Remember passphrase")
                                        : QStringLiteral("Remember password"));
}

void ConnectionDialog::chooseSshKey() {
    QString start = m_sshKey->text().trimmed();
    if (start.startsWith(QLatin1String("~/"))) start = QDir::homePath() + start.mid(1);
    const QFileInfo current(start);
    if (current.exists() && !current.isDir()) start = current.absolutePath();
    if (start.isEmpty() || !QFileInfo(start).isDir()) {
        const QString sshDir = QDir::homePath() + QStringLiteral("/.ssh");
        start = QFileInfo(sshDir).isDir() ? sshDir : QDir::homePath();
    }
    emit sshKeyBrowseRequested(start);
}

void ConnectionDialog::focusSshKey() {
    m_sshKey->setFocus(Qt::OtherFocusReason);
    m_sshKey->setCursorPosition(m_sshKey->text().size());
}

void ConnectionDialog::setSshKeyPath(const QString& path) {
    const QFileInfo info(path);
    m_sshKey->setText(info.absoluteFilePath());
    focusSshKey();
    updatePasswordHint();
}

void ConnectionDialog::applyHostPort() {
    QString host;
    int port = m_port->text().trimmed().toInt();
    if (!splitHostPort(m_host->text(), &host, &port)) return;
    m_host->setText(host);
    m_port->setText(QString::number(port));
}

void ConnectionDialog::submit() {
    if (m_busy) return;
    applyHostPort();
    auto location = this->location();
    if (location.host.isEmpty()) {
        showError(QStringLiteral("Host or IP address is required"));
        m_host->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (location.protocol != Core::Network::Protocol::Sftp) {
        location.sshKeyPath.clear();
    } else if (!location.sshKeyPath.isEmpty()) {
        QString key = location.sshKeyPath;
        if (key == QLatin1String("~")) key = QDir::homePath();
        else if (key.startsWith(QLatin1String("~/"))) key = QDir::homePath() + key.mid(1);
        const QFileInfo info(key);
        if (!info.exists() || !info.isFile() || !info.isReadable()) {
            showError(QStringLiteral("SSH key file is missing or unreadable"));
            m_sshKey->setFocus(Qt::OtherFocusReason);
            return;
        }
        location.sshKeyPath = info.absoluteFilePath();
    }
    if (!location.path.isEmpty() && !location.path.startsWith(QLatin1Char('/'))) {
        location.path.prepend(QLatin1Char('/'));
    }
    if (location.anonymous) {
        location.username.clear();
        location.password.clear();
        location.sshKeyPath.clear();
        location.domain.clear();
        location.rememberPassword = false;
    }
    if (location.protocol == Core::Network::Protocol::Sftp && !location.anonymous) {
        Core::Network::rememberSshKey(location);
    }
    m_error->clear();
    m_error->hide();
    setBusy(true);
    emit connectRequested(location);
}

}
