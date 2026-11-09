#!/usr/bin/env bash
# DirectMesh installer: builds the DirectMesh RADV Vulkan driver for this system and installs it for the current
# user. The system driver is not changed; games use DirectMesh through a Steam launch option (or, with --global,
# every Vulkan program in your desktop session does).
#
#   bash install.sh                 install (or update) and print the Steam launch option
#   bash install.sh --global        also use DirectMesh for every Vulkan program (after logging in again)
#   bash install.sh --uninstall     remove everything this script installed
#
# Environment: DIRECTMESH_VERSION (release tag), DIRECTMESH_PREFIX (install folder), DIRECTMESH_JOBS (build jobs).
set -euo pipefail

VERSION=${DIRECTMESH_VERSION:-directmesh-v1.4}
REPO=lonewolf0622/Mesh-Shaders-for-Rdna-1-
PREFIX=${DIRECTMESH_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/directmesh}
WORK=${XDG_CACHE_HOME:-$HOME/.cache}/directmesh-build
BIN=${DIRECTMESH_BIN:-$HOME/.local/bin}
ENVD=${XDG_CONFIG_HOME:-$HOME/.config}/environment.d/90-directmesh.conf
JOBS=${DIRECTMESH_JOBS:-$(nproc 2>/dev/null || echo 4)}
ICD="$PREFIX/share/vulkan/icd.d/radeon_icd.x86_64.json"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

GLOBAL=0
case "${1:-}" in
   --uninstall)
      say "Removing DirectMesh"
      rm -rf "$PREFIX" "$WORK" "$BIN/directmesh-run" "$ENVD"
      echo "Removed. Remove the VK_DRIVER_FILES=... part from any Steam launch options you added."
      exit 0 ;;
   --global) GLOBAL=1 ;;
   "") ;;
   *) sed -n '2,12p' "$0"; exit 1 ;;
esac

[ "$(uname -m)" = x86_64 ] || die "DirectMesh needs an x86_64 system."
[ "$(id -u)" != 0 ] || die "Run this as your normal user, not as root (it asks for sudo only to install build packages)."
GPUS=$(lspci 2>/dev/null | grep -iE 'VGA|Display|3D' || true)
if [ -n "$GPUS" ] && ! printf '%s\n' "$GPUS" | grep -iE 'AMD|ATI' >/dev/null; then
   echo "Note: no AMD GPU found. DirectMesh only does something on an AMD BC-250 or RDNA 1 (Navi 10/12) GPU."
fi
if grep -qsE '^ID=(steamos|bazzite|aurora|bluefin)' /etc/os-release || [ -e /run/ostree-booted ]; then
   die "This looks like an immutable system (SteamOS, Bazzite, Silverblue...). This script installs build packages
with the system package manager, which these systems do not allow. Build inside a container that matches your
host's release instead (for example a toolbox of the same Fedora version), then run this script there."
fi

# --- build packages ---------------------------------------------------------------------------------------------
# (read in a subshell: os-release defines VERSION and other names this script uses)
OS_ID=$( (. /etc/os-release 2>/dev/null; echo "${ID:-}") )
ids=" $( (. /etc/os-release 2>/dev/null; echo "${ID:-} ${ID_LIKE:-}") ) "
PKG_ARCH="base-devel cmake python curl tar xz glslang libx11 libxcb libxshmfence xcb-util-keysyms wayland wayland-protocols
          systemd-libs expat zlib zstd libelf"
PKG_DEB="build-essential cmake pkg-config python3 python3-venv python3-pip curl tar xz-utils glslang-tools libx11-dev
         libx11-xcb-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-xfixes0-dev libxcb-sync-dev
         libxcb-randr0-dev libxcb-shm0-dev libxcb-keysyms1-dev libxshmfence-dev libwayland-dev wayland-protocols
         libudev-dev libexpat1-dev zlib1g-dev libzstd-dev libelf-dev"
PKG_FED="gcc gcc-c++ cmake pkgconf-pkg-config python3 python3-pip curl tar xz glslang libX11-devel libxcb-devel
         libxshmfence-devel xcb-util-keysyms-devel wayland-devel wayland-protocols-devel systemd-devel expat-devel
         zlib-devel libzstd-devel elfutils-libelf-devel"
PKG_SUSE="gcc gcc-c++ cmake pkgconf python3 python3-pip curl tar xz glslang-devel libX11-devel libxcb-devel
          libxshmfence-devel xcb-util-keysyms-devel wayland-devel wayland-protocols-devel libudev-devel libexpat-devel
          zlib-devel libzstd-devel libelf-devel"

missing() {
   local m=""
   for c in cc c++ cmake pkg-config python3 curl tar glslangValidator wayland-scanner; do
      command -v "$c" >/dev/null 2>&1 || m="$m $c"
   done
   python3 -c 'import venv, ensurepip' 2>/dev/null || m="$m python3-venv"
   for p in x11 x11-xcb xcb xcb-dri3 xcb-present xcb-xfixes xcb-sync xcb-randr xcb-shm xcb-keysyms xshmfence \
            wayland-client libudev expat zlib libzstd; do
      pkg-config --exists "$p" 2>/dev/null || m="$m $p"
   done
   echo "$m"
}

need=$(missing)
if [ -n "$need" ]; then
   say "Missing build tools/libraries:$need"
   case "$ids" in
      *" arch "*|*" cachyos "*|*" manjaro "*|*" endeavouros "*) cmd="sudo pacman -S --needed $PKG_ARCH" ;;
      *" debian "*|*" ubuntu "*) cmd="sudo apt-get update && sudo apt-get install -y $PKG_DEB" ;;
      *" fedora "*|*" rhel "*|*" nobara "*) cmd="sudo dnf install -y $PKG_FED" ;;
      *" opensuse"*|*" suse "*) cmd="sudo zypper install -y $PKG_SUSE" ;;
      *) die "Unknown distribution (${OS_ID:-?}). Install the development packages for:$need
(compiler, pkg-config, Python with venv, curl, tar, glslang, X11/xcb, Wayland, udev, expat, zlib, zstd), then run
this script again." ;;
   esac
   echo "Installing them with:"
   echo "  $(echo $cmd)"
   read -r -p "Continue? [Y/n] " answer </dev/tty || answer=y
   case "$answer" in [nN]*) die "Cancelled." ;; esac
   eval "$cmd"
   need=$(missing)
   [ -z "$need" ] || die "Still missing:$need"
fi

# --- build tools (meson, ninja, mako) in a private Python environment ---------------------------------------------
mkdir -p "$WORK"
if [ ! -x "$WORK/venv/bin/meson" ]; then
   say "Preparing build tools"
   python3 -m venv "$WORK/venv"
   "$WORK/venv/bin/pip" install --quiet --upgrade pip
   "$WORK/venv/bin/pip" install --quiet 'meson>=1.4,<2' ninja mako pyyaml packaging
fi
export PATH="$WORK/venv/bin:$PATH"

# --- glslang 12.2 or newer (Mesa's requirement; Debian 12 and Ubuntu 22.04 ship older ones) -----------------------
glslang_version() { glslangValidator --version 2>/dev/null | head -n 1 | awk -F: '{gsub(/ /, "", $3); print $3}'; }
glslang_new_enough() {
   local v; v=$(glslang_version)
   [ -n "$v" ] && [ "$(printf '%s\n12.2\n' "$v" | sort -V | head -n 1)" = 12.2 ]
}
if [ -x "$WORK/tools/bin/glslangValidator" ]; then
   export PATH="$WORK/tools/bin:$PATH"
fi
glslang_found=$(glslang_version)
if [ -n "${DIRECTMESH_BUILD_GLSLANG:-}" ] || ! glslang_new_enough; then
   say "Building glslang 14.3.0 (Mesa needs 12.2 or newer; found: ${glslang_found:-none})"
   rm -rf "$WORK/glslang" "$WORK/tools"
   mkdir -p "$WORK/glslang"
   curl -fL --retry 3 --progress-bar "https://github.com/KhronosGroup/glslang/archive/refs/tags/14.3.0.tar.gz" |
      tar -xz -C "$WORK/glslang" --strip-components=1
   cmake -S "$WORK/glslang" -B "$WORK/glslang/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/tools" \
      -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_CXX_FLAGS="-include cstdint" \
      > "$WORK/glslang.log" 2>&1 &&
   cmake --build "$WORK/glslang/build" -j "$JOBS" >> "$WORK/glslang.log" 2>&1 &&
   cmake --install "$WORK/glslang/build" >> "$WORK/glslang.log" 2>&1 ||
      { tail -n 30 "$WORK/glslang.log"; die "Building glslang failed (full log: $WORK/glslang.log)."; }
   rm -rf "$WORK/glslang"
   export PATH="$WORK/tools/bin:$PATH"
   glslang_new_enough || die "glslang $(glslang_version) is still older than 12.2."
fi

# --- source -------------------------------------------------------------------------------------------------------
SRC="$WORK/src-$VERSION"
if [ ! -f "$SRC/meson.build" ]; then
   say "Downloading DirectMesh $VERSION"
   rm -rf "$SRC" "$SRC.tmp"
   mkdir -p "$SRC.tmp"
   curl -fL --retry 3 --progress-bar "https://github.com/$REPO/archive/refs/tags/$VERSION.tar.gz" |
      tar -xz -C "$SRC.tmp" --strip-components=1
   mv "$SRC.tmp" "$SRC"
fi

# --- build and install (the Vulkan driver only) -------------------------------------------------------------------
say "Building the driver (about 5-20 minutes, using $JOBS jobs)"
rm -rf "$WORK/build"
meson setup "$WORK/build" "$SRC" --prefix "$PREFIX" --libdir lib -Dbuildtype=release \
   -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=x11,wayland -Dllvm=disabled -Dvideo-codecs= \
   -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
   -Dvulkan-layers= -Dtools= -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
   -Ddisplay-info=disabled -Dspirv-tools=disabled -Dxlib-lease=disabled \
   -Dallow-fallback-for=libdrm --force-fallback-for=libdrm -Dlibdrm:default_library=static \
   -Dlibdrm:intel=disabled -Dlibdrm:radeon=disabled -Dlibdrm:nouveau=disabled -Dlibdrm:vmwgfx=disabled \
   -Dlibdrm:freedreno=disabled -Dlibdrm:vc4=disabled -Dlibdrm:etnaviv=disabled -Dlibdrm:exynos=disabled \
   -Dlibdrm:omap=disabled -Dlibdrm:tegra=disabled -Dlibdrm:cairo-tests=disabled -Dlibdrm:man-pages=disabled \
   -Dlibdrm:valgrind=disabled -Dlibdrm:tests=false > "$WORK/setup.log" 2>&1 ||
   { tail -n 30 "$WORK/setup.log"; die "Configuring the build failed (full log: $WORK/setup.log)."; }
ninja -C "$WORK/build" -j "$JOBS" > "$WORK/build.log" 2>&1 ||
   { tail -n 30 "$WORK/build.log"; die "The build failed (full log: $WORK/build.log)."; }
rm -rf "$PREFIX"
ninja -C "$WORK/build" install > "$WORK/install.log" 2>&1 ||
   { tail -n 30 "$WORK/install.log"; die "Installing failed (full log: $WORK/install.log)."; }
[ -f "$ICD" ] || die "The driver manifest $ICD was not installed."

# --- how to use it ------------------------------------------------------------------------------------------------
# 32-bit Vulkan programs keep the system's 32-bit driver (the loader picks the matching one).
FILES="$ICD"
for f in /usr/share/vulkan/icd.d/radeon_icd.i686.json /usr/share/vulkan/icd.d/radeon_icd.x86.json; do
   [ -f "$f" ] && FILES="$FILES:$f" && break
done
echo "$FILES" > "$PREFIX/vk_driver_files"
mkdir -p "$BIN"
cat > "$BIN/directmesh-run" <<EOF
#!/bin/sh
# Run a program with the DirectMesh driver: directmesh-run <program> [arguments]
VK_DRIVER_FILES="$FILES" exec "\$@"
EOF
chmod +x "$BIN/directmesh-run"
if [ "$GLOBAL" = 1 ]; then
   mkdir -p "$(dirname "$ENVD")"
   echo "VK_DRIVER_FILES=$FILES" > "$ENVD"
fi
rm -rf "$WORK/build"

say "DirectMesh $VERSION is installed in $PREFIX"
if command -v vulkaninfo >/dev/null 2>&1; then
   VK_DRIVER_FILES="$FILES" vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo' | sed 's/^\s*/   /' || true
   # (grep reads everything: with pipefail, an early grep -q exit would fail vulkaninfo on a closed pipe)
   if VK_DRIVER_FILES="$FILES" vulkaninfo 2>/dev/null | grep -E 'meshShader *= *true' >/dev/null; then
      echo "   Mesh shaders: yes"
   else
      echo "   Mesh shaders: no (this GPU is not a BC-250 or Navi 10/12, or DirectMesh is turned off)"
   fi
fi
cat <<EOF

Use it in a Steam game: right-click the game > Properties > Launch Options, and enter:

   VK_DRIVER_FILES=$FILES %command%

Other programs:   directmesh-run <program>
EOF
if [ "$GLOBAL" = 1 ]; then
   echo "Every Vulkan program uses DirectMesh after you log out and back in ($ENVD)."
fi
cat <<EOF
Turn DirectMesh off for one game while keeping the driver: add RADV_DIRECTMESH=0 before %command%.
Update: run this script again.   Uninstall: bash install.sh --uninstall
EOF
