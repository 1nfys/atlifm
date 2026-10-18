#pragma once

#include <QString>
#include <QColor>
#include <filesystem>
#include <optional>

namespace Icons {

struct IconInfo {
    QString glyph;
    QColor color;
};

// подбирает глиф Nerd Font и цвет по правилам пресетов
IconInfo getIcon(const std::filesystem::path& path, bool isDirectory,
                 bool isSymlink = false, bool isOpen = false,
                 std::optional<bool> isExecutableHint = std::nullopt);

// Nerd Font глифы для интерфейса
namespace Glyph {
    inline const QString FolderClosed{"\ue5ff"};
    inline const QString FolderOpen{"\ue5fe"};
    inline const QString FileSymlink{"\uf481"};
    inline const QString StickyPin{"\uf08d"};
    inline const QString Executable{"\uf489"};
    inline const QString FileDefault{"\uf15b"};

    inline const QString FolderConfig{"\ue5fc"};
    inline const QString FolderGit{"\ue5fb"};
    inline const QString FolderGithub{"\ue5fd"};
    inline const QString FolderDesktop{"\uf108"};
    inline const QString FolderDocuments{"\uf401"};
    inline const QString FolderDownloads{"\uf498"};
    inline const QString FolderMusic{"\uf025"};
    inline const QString FolderPictures{"\ue244"};
    inline const QString FolderVideos{"\uf447"};
    inline const QString FolderTrash{"\uf1f8"};
    inline const QString DriveDisk{"\uf0a0"};

    inline const QString FolderHome{"\uf015"};
    inline const QString TabPlus{"+"};
    inline const QString TabClose{"\u2715"};
    inline const QString Search{"\uf002"};
    inline const QString Checkmark{"\u2713"};
    inline const QString ArrowRight{"\u2192"};
    inline const QString Lock{"\uf023"};
}

}
