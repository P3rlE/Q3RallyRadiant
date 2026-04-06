# Q3RallyRadiant

> **A fork of [NetRadiant-custom](https://github.com/Garux/netradiant-custom) tailored for [Q3Rally](https://www.q3rally.com/) map development.**

---

![Q3RallyRadiant](install/bitmaps/splash.svg)

---

## What is Q3RallyRadiant?

Q3RallyRadiant is a level editor for Q3Rally, built on top of NetRadiant-custom — the open-source, cross-platform map editor for id Tech based games. This fork adds Q3Rally-specific tooling, branding and workflow improvements on top of the solid NetRadiant-custom foundation.

All upstream features of NetRadiant-custom are preserved. Q3RallyRadiant stays synchronized with upstream and merges improvements as they are released.

## Q3Rally-specific additions

### BotViz Plugin
Visualizes bot AI debug data recorded during gameplay directly in the 3D editor view.

- Loads JSONL debug files exported by the Q3Rally AI system
- Renders bot routes as speed-colored lines (blue = slow, red = fast)
- Displays `bot_path_node` positions from the currently loaded map
- Timeline slider with frame, time and speed readout
- Collision event markers (red sphere + ring)
- Accessible via **Plugins → BotViz → Load JSONL**

### Q3Rally Gamepack
Preconfigured for Q3Rally entities, shaders and compile pipeline:
- `rally_startfinish`, `rally_checkpoint`, `bot_path_node` and all Q3Rally entities
- Q3Rally shader paths and texture types
- Engine path preconfigured for `q3rally.exe`

### Branding
Q3RallyRadiant splash screen, icon and About dialog — keeping it clear this is a Q3Rally tool while crediting the upstream project.

---

## Building

### Windows (MSYS2 MinGW64) — recommended for development

```bash
# Install dependencies (run once)
pacman -S --needed base-devel git mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake mingw-w64-x86_64-qt5-base \
  mingw-w64-x86_64-qt5-svg mingw-w64-x86_64-libxml2 \
  mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-libpng \
  mingw-w64-x86_64-assimp mingw-w64-x86_64-minizip \
  mingw-w64-x86_64-libwebp unzip

# Build
cd /d/Q3RallyDev/Q3RallyRadiant
make MAKEFILE_CONF=msys2-Makefile.conf DOWNLOAD_GAMEPACKS=no INSTALL_DLLS=no BUILD=release -j$(nproc)
```

After building, copy Qt runtime DLLs and platform plugin:
```bash
cd install/
ntldd -R radiant.exe | grep mingw64 | awk -F'=> ' '{print $2}' | awk '{print $1}' | awk -F'\\' '{print $NF}' | xargs -I{} cp /mingw64/bin/{} .
mkdir -p platforms imageformats iconengines
cp /mingw64/share/qt5/plugins/platforms/qwindows.dll platforms/
cp /mingw64/share/qt5/plugins/imageformats/qsvg.dll imageformats/
cp /mingw64/share/qt5/plugins/iconengines/qsvgicon.dll iconengines/
```

### Linux (WSL Ubuntu 24.04)

```bash
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH

make install/radiant.x86_64 \
     install/plugins/botviz.so \
     install/plugins/prtview.so \
     install/plugins/sunplug.so \
     install/modules/vfspk3.so \
     install/modules/mapq3.so \
     install/modules/entity.so \
     install/modules/shaders.so \
     -j$(nproc)
```

---

## Staying in sync with upstream

Q3RallyRadiant tracks [Garux/netradiant-custom](https://github.com/Garux/netradiant-custom) as upstream remote.

```bash
git remote add upstream https://github.com/Garux/netradiant-custom.git
git fetch upstream
git merge upstream/master
```

Conflicts are expected in `radiant/gtkdlgs.cpp`, `radiant/mainframe.cpp` and `radiant/main.cpp` (branding) and `plugins/vfspk3/vfs.cpp` (GCC 13 fix). Everything under `contrib/botviz/` is Q3Rally-only and will never conflict.

---

## Credits

- **[NetRadiant-custom](https://github.com/Garux/netradiant-custom)** by Garux — the upstream editor this fork is based on
- **[NetRadiant](https://gitlab.com/xonotic/netradiant)** by the Xonotic team
- **[GtkRadiant](https://github.com/TTimo/GtkRadiant)** by id Software / TTimo
- **Q3RallyRadiant** additions by the Q3Rally development team

---

## License

Q3RallyRadiant is free software, licensed under the **GNU General Public License v2**. See [LICENSE](LICENSE) for details. All upstream code retains its original license.
