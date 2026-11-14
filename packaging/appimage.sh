#!/bin/sh
# Empacota o openbitdo como AppImage.
#
# Requer: build/ com os binários (make), appimagetool em $APPIMAGETOOL
# (default: /tmp/appimg/appimagetool) e Python3+Pillow para o ícone.
#
# Uso: packaging/appimage.sh [versão]   → gera openbitdo-<versão>-x86_64.AppImage
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=${1:-dev}
APPIMAGETOOL=${APPIMAGETOOL:-/tmp/appimg/appimagetool}
APPDIR="$ROOT/AppDir"
OUT="$ROOT/openbitdo-$VERSION-x86_64.AppImage"

cd "$ROOT"

[ -x build/8bdgui ] || { echo "build/8bdgui ausente: rode 'make' primeiro" >&2; exit 1; }

echo "==> limpando AppDir"
rm -rf "$APPDIR"

echo "==> binários"
mkdir -p "$APPDIR/usr/bin"
for bin in 8bdgui bd_cli bd_build bd_watch; do
    install -m 755 "build/$bin" "$APPDIR/usr/bin/$bin"
done

echo "==> ícone"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
( cd "$APPDIR/usr/share/icons/hicolor/256x256/apps" && python3 "$ROOT/packaging/make-icon.py" >/dev/null )
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/openbitdo.png" "$APPDIR/openbitdo.png"

echo "==> desktop entry"
mkdir -p "$APPDIR/usr/share/applications"
cat > "$APPDIR/openbitdo.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=openbitdo
Comment=Configure 8BitDo Ultimate controller profiles on Linux
Exec=8bdgui
Icon=openbitdo
Terminal=false
Categories=Utility;Settings;HardwareSettings;
Keywords=8bitdo;controller;gamepad;profile;
DESKTOP
cp "$APPDIR/openbitdo.desktop" "$APPDIR/usr/share/applications/openbitdo.desktop"

echo "==> AppRun"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PATH="$HERE/usr/bin:$PATH"
# O hidraw é acessado direto; o app é GTK4 do sistema.
exec "$HERE/usr/bin/8bdgui" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

echo "==> appimagetool"
ARCH=x86_64 VERSION="$VERSION" "$APPIMAGETOOL" --no-appstream "$APPDIR" "$OUT"

echo "pronto: $OUT"
