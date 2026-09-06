#!/bin/bash
# 打包 macOS 可分发的 MusicPlayer.app + dmg
# 用法：./deploy_mac.sh
set -euo pipefail

cd "$(dirname "$0")"

QT_PREFIX="$HOME/Qt/6.5.3/macos"
BIN=build/music_player
APP=MusicPlayer.app
DMG=MusicPlayer.dmg
STAGE=dist

rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE/$APP/Contents/MacOS" "$STAGE/$APP/Contents/Resources"

# 1. 二进制 + 图标 + Info.plist
cp "$BIN" "$STAGE/$APP/Contents/MacOS/music_player"
cp MusicPlayer.icns "$STAGE/$APP/Contents/Resources/"
cat > "$STAGE/$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>            <string>MusicPlayer</string>
    <key>CFBundleDisplayName</key>     <string>Music Player</string>
    <key>CFBundleIdentifier</key>      <string>local.musicplayer.app</string>
    <key>CFBundleExecutable</key>      <string>music_player</string>
    <key>CFBundleIconFile</key>        <string>MusicPlayer</string>
    <key>CFBundlePackageType</key>     <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>0.2.0</string>
    <key>CFBundleVersion</key>         <string>0.2.0</string>
    <key>LSMinimumSystemVersion</key>  <string>12.0</string>
    <key>NSHighResolutionCapable</key> <true/>
</dict>
</plist>
PLIST

# 2. macdeployqt：打包 Qt 框架与插件（platforms/multimedia/imageformats/styles/tls）
"$QT_PREFIX/bin/macdeployqt" "$STAGE/$APP"

# 3. TagLib：复制进 Frameworks 并改写引用路径（二进制当前引用 Homebrew 绝对路径）
mkdir -p "$STAGE/$APP/Contents/Frameworks"
cp /usr/local/opt/taglib/lib/libtag.2.dylib "$STAGE/$APP/Contents/Frameworks/"
install_name_tool -change \
    /usr/local/opt/taglib/lib/libtag.2.dylib \
    @executable_path/../Frameworks/libtag.2.dylib \
    "$STAGE/$APP/Contents/MacOS/music_player"
install_name_tool -id @rpath/libtag.2.dylib \
    "$STAGE/$APP/Contents/Frameworks/libtag.2.dylib"

# 4. 打 DMG（含 /Applications 快捷方式）
mkdir -p "$STAGE/dmg"
cp -R "$STAGE/$APP" "$STAGE/dmg/"
ln -s /Applications "$STAGE/dmg/Applications"
hdiutil create -volname "MusicPlayer" -srcfolder "$STAGE/dmg" -ov -format UDZO "$DMG" >/dev/null

echo "✅ 输出: $DMG ($(du -h "$DMG" | cut -f1))"
