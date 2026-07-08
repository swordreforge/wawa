# Maintainer: Your Name <your@email>
# Contributor: Your Name <your@email>

pkgname=wawa-git
_pkgname=wawa
pkgver=r37.3c30a5f
pkgrel=1
pkgdesc="A simple, hackable, and distinctive Wayland wallpaper setter — wlr-layer-shell based, SAIL-powered"
arch=('x86_64')
url="https://codeberg.org/sewn/wawa"
license=('MIT')
depends=(
    'wayland'
    # Common image format backends (SAIL auto-detects)
    'libpng'
    'libjpeg-turbo'
    'libwebp'
    'libtiff'
    'giflib'
    'libavif'
)
makedepends=('cmake' 'wayland-protocols' 'git')
optdepends=(
    'libheif: HEIF image format support'
    'libjxl: JPEG XL image format support'
    'openexr: OpenEXR image format support'
    'libraw: RAW image format support (CRW/NEF/ARW/…)'
    'openjpeg2: JPEG 2000 image format support'
    'librsvg: SVG image format support'
    'ffmpeg: Video frame / animated image extraction'
)
provides=("${_pkgname}=${pkgver}")
conflicts=("${_pkgname}")
source=("git+https://codeberg.org/sewn/wawa.git")
sha256sums=('SKIP')

pkgver() {
    cd "$srcdir/${_pkgname}"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$srcdir/${_pkgname}"

    # SAIL is bundled as a subproject in the source tree.
    # Codec dependencies are auto-detected at CMake time —
    # whatever system libraries are present get compiled into
    # the combined libsail-codecs.so.
    local cmake_options=(
        -B build
        -Wno-dev
        -D CMAKE_BUILD_TYPE=None
        -D CMAKE_INSTALL_PREFIX=/usr
        -D CMAKE_INSTALL_LIBDIR=lib
    )
    cmake "${cmake_options[@]}"
    cmake --build build
}

package() {
    cd "$srcdir/${_pkgname}"

    DESTDIR="$pkgdir" cmake --install build

    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 sail/LICENSE.txt "$pkgdir/usr/share/licenses/$pkgname/LICENSE.sail"
}
