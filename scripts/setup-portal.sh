#!/usr/bin/env bash
set -euo pipefail

XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

PORTALS_DIR="$XDG_DATA_HOME/xdg-desktop-portal/portals"
SERVICES_DIR="$XDG_DATA_HOME/dbus-1/services"
APPS_DIR="$XDG_DATA_HOME/applications"
CONF_DIR="$XDG_CONFIG_HOME/xdg-desktop-portal"

mkdir -p "$PORTALS_DIR" "$SERVICES_DIR" "$APPS_DIR" "$CONF_DIR"

cat << 'PORTAL' > "$PORTALS_DIR/atlifm.portal"
[portal]
DBusName=org.freedesktop.impl.portal.desktop.atlifm
Interfaces=org.freedesktop.impl.portal.FileChooser;
PORTAL

cat << 'SERVICE' > "$SERVICES_DIR/org.freedesktop.impl.portal.desktop.atlifm.service"
[D-BUS Service]
Name=org.freedesktop.impl.portal.desktop.atlifm
Exec=atlifm --portal
SERVICE

cat << 'DESKTOP' > "$APPS_DIR/atlifm-portal.desktop"
[Desktop Entry]
Name=atlifm Portal
Comment=XDG Desktop Portal FileChooser backend for atlifm
Exec=atlifm --portal
Icon=atlifm
Terminal=false
Type=Application
NoDisplay=true
StartupNotify=true
StartupWMClass=atlifm-portal
DESKTOP

ensure_conf() {
    local target="$1"
    if [ ! -f "$target" ]; then
        cat << 'CONF' > "$target"
[preferred]
default=hyprland;gtk
org.freedesktop.impl.portal.FileChooser=atlifm
CONF
    elif ! grep -q "FileChooser=atlifm" "$target"; then
        if grep -q "\[preferred\]" "$target"; then
            sed -i '/\[preferred\]/a org.freedesktop.impl.portal.FileChooser=atlifm' "$target"
        else
            printf "\n[preferred]\ndefault=hyprland;gtk\norg.freedesktop.impl.portal.FileChooser=atlifm\n" >> "$target"
        fi
    fi
}

ensure_conf "$CONF_DIR/portals.conf"
ensure_conf "$CONF_DIR/hyprland-portals.conf"

HYPR_LUA="$XDG_CONFIG_HOME/hypr/hyprland.lua"
if [ -f "$HYPR_LUA" ]; then
    if ! grep -q "atlifm-portal" "$HYPR_LUA"; then
        if grep -q "consplorer-portal" "$HYPR_LUA"; then
            sed -i 's/class = "\^consplorer-portal\$"/class = "^(consplorer|atlifm)-portal$"/g' "$HYPR_LUA"
        else
            printf '\no.window({ class = "^atlifm-portal$" }, { tag = "+floating-window" })\n' >> "$HYPR_LUA"
        fi
    fi
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user restart xdg-desktop-portal || true
fi

echo "atlifm portal configured successfully."
