#pragma once

#include "../atlifm-core/Network.hpp"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace Widgets {

class ConnectionDialog : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionDialog(QWidget* parent = nullptr);

    void open(const Core::Network::Location& location);
    void showError(const QString& message);
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }
    Core::Network::Location location() const;
    void focusPrimaryInput();
    void focusSshKey();
    void setSshKeyPath(const QString& path);

signals:
    void connectRequested(const Core::Network::Location& location);
    void sshKeyBrowseRequested(const QString& startPath);
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void submit();
    void applyHostPort();
    void chooseSshKey();
    void updateSensitive();
    void updateProtocolFields();
    void updatePasswordHint();

    QComboBox* m_protocol{nullptr};
    QLineEdit* m_host{nullptr};
    QLineEdit* m_port{nullptr};
    QLineEdit* m_path{nullptr};
    QLineEdit* m_username{nullptr};
    QLineEdit* m_password{nullptr};
    QLabel* m_passwordLabel{nullptr};
    QLineEdit* m_sshKey{nullptr};
    QWidget* m_sshKeyRow{nullptr};
    QLabel* m_sshKeyLabel{nullptr};
    QPushButton* m_browseKey{nullptr};
    QLineEdit* m_domain{nullptr};
    QLabel* m_domainLabel{nullptr};
    QCheckBox* m_anonymous{nullptr};
    QCheckBox* m_acceptHostKey{nullptr};
    QCheckBox* m_rememberPassword{nullptr};
    QLabel* m_error{nullptr};
    QPushButton* m_connect{nullptr};
    QPushButton* m_cancel{nullptr};
    Core::Network::Protocol m_lastProtocol{Core::Network::Protocol::Sftp};
    bool m_busy{false};
};

}
