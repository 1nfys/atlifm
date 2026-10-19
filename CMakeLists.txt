cmake_minimum_required(VERSION 3.28)
project(atlifm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Concurrent
    Svg
    SvgWidgets
    DBus
)

add_executable(atlifm
    atlifm-logo.qrc
    main.cpp
    atlifm-theme/Theme.hpp
    atlifm-core/Icons.hpp
    atlifm-core/Icons.cpp
    atlifm-core/XdgUtils.hpp
    atlifm-core/XdgUtils.cpp
    atlifm-core/TabState.hpp
    atlifm-core/Bookmarks.hpp
    atlifm-core/Keybinds.hpp
    atlifm-core/Plugins.hpp
    atlifm-core/atlifm-plugin-api.h
    atlifm-core/UndoManager.hpp
    atlifm-widgets/NativePlugins.hpp
    atlifm-widgets/NativePlugins.cpp
    atlifm-models/FileSystemModel.hpp
    atlifm-models/FileSystemModel.cpp
    atlifm-models/ArchiveModel.hpp
    atlifm-models/ArchiveModel.cpp
    atlifm-dnd/DragDropHandler.hpp
    atlifm-dnd/DragDropHandler.cpp
    atlifm-widgets/FileItemDelegate.hpp
    atlifm-widgets/FileItemDelegate.cpp
    atlifm-widgets/TabBar.hpp
    atlifm-widgets/TabBar.cpp
    atlifm-widgets/BookmarksBar.hpp
    atlifm-widgets/BookmarksBar.cpp
    atlifm-widgets/PreviewPane.hpp
    atlifm-widgets/PreviewPane.cpp
    atlifm-widgets/ContextMenu.hpp
    atlifm-widgets/ContextMenu.cpp
    atlifm-widgets/PropertiesDialog.hpp
    atlifm-widgets/PropertiesDialog.cpp
    atlifm-widgets/OpenWithDialog.hpp
    atlifm-widgets/OpenWithDialog.cpp
    atlifm-widgets/FileChooserDialog.hpp
    atlifm-widgets/FileChooserDialog.cpp
    atlifm-widgets/StatusBar.hpp
    atlifm-widgets/StatusBar.cpp
    atlifm-widgets/TuiInputPopup.hpp
    atlifm-widgets/TuiInputPopup.cpp
    atlifm-widgets/TuiOutputOverlay.hpp
    atlifm-widgets/TuiOutputOverlay.cpp
    atlifm-widgets/HelpDialog.hpp
    atlifm-widgets/HelpDialog.cpp
    atlifm-widgets/MillerColumns.hpp
    atlifm-widgets/MillerColumns.cpp
    atlifm-widgets/MainWindow.hpp
    atlifm-widgets/MainWindow.cpp
    atlifm-portal/PortalService.hpp
    atlifm-portal/PortalService.cpp
)

target_compile_options(atlifm PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# харденинг для GCC/Clang: fortify (только оптимизированные сборки),
# stack protector, ассерты libstdc++, полный RELRO при линковке
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(atlifm PRIVATE -fstack-protector-strong)
    target_compile_definitions(atlifm PRIVATE _GLIBCXX_ASSERTIONS)
    target_compile_definitions(atlifm PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:_FORTIFY_SOURCE=3>)
    target_link_options(atlifm PRIVATE -Wl,-z,relro -Wl,-z,now)
endif()

target_link_libraries(atlifm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::DBus
    ${CMAKE_DL_LIBS}
)

# установка (пользовательская: cmake --install build --prefix ~/.local),
# ~/.local/share попадает в XDG_DATA_DIRS, ~/.local/bin обычно в PATH
install(TARGETS atlifm RUNTIME DESTINATION bin)

# SDK плагины (C ABI): сторонние *.so компилируются с этим заголовком
install(FILES atlifm-core/atlifm-plugin-api.h DESTINATION include/atlifm)

install(FILES
    atlifm.desktop
    atlifm-portal/atlifm-portal.desktop
    DESTINATION share/applications
)
install(FILES atlifm-portal/atlifm.portal
    DESTINATION share/xdg-desktop-portal/portals
)
install(FILES atlifm-portal/hyprland-portals.conf
    DESTINATION share/xdg-desktop-portal
)

# иконка приложения: в .desktop Icon=atlifm, тема hicolor резолвит имя
# 1024x1024 НЕТ в index.theme hicolor, поэтому иконка обязана лежать и в
# стандартных каталогах, иначе панель/лаунчер покажет заглушку
# ресайзы в atlifm-logo/icons/
install(FILES atlifm-logo/icons/atlifm-16.png  DESTINATION share/icons/hicolor/16x16/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-22.png  DESTINATION share/icons/hicolor/22x22/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-24.png  DESTINATION share/icons/hicolor/24x24/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-32.png  DESTINATION share/icons/hicolor/32x32/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-48.png  DESTINATION share/icons/hicolor/48x48/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-64.png  DESTINATION share/icons/hicolor/64x64/apps   RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-128.png DESTINATION share/icons/hicolor/128x128/apps RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME atlifm.png)
install(FILES atlifm-logo/icons/atlifm-512.png DESTINATION share/icons/hicolor/512x512/apps RENAME atlifm.png)
install(FILES atlifm-logo/atlifm.png
    DESTINATION share/icons/hicolor/1024x1024/apps
    RENAME atlifm.png
)

# сервис D-Bus генерируется в момент установки: Exec обязан указывать на
# реальный префикс установки (учитывается и `cmake --install --prefix`),
# а не на каталог сборки разработчика
install(CODE [[
    file(MAKE_DIRECTORY "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/dbus-1/services")
    file(WRITE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/dbus-1/services/org.freedesktop.impl.portal.desktop.atlifm.service"
        "[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.atlifm\nExec=${CMAKE_INSTALL_PREFIX}/bin/atlifm --portal\n")
]])
