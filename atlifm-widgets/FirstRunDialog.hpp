#pragma once

#include <QDialog>

namespace Widgets {

class FirstRunDialog : public QDialog {
    Q_OBJECT

public:
    explicit FirstRunDialog(QWidget* parent = nullptr);
};

}
