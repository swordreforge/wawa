# Maintainer: swordreforge
# Contributor: sewnie <sewn@disroot.org>

pkgname=wawa-git
_pkgname=wawa
pkgver=r53.cae4003
pkgrel=1
pkgdesc="A simple, hackable, and distinctive Wayland wallpaper setter — wlr-layer-shell based, SAIL-powered"
arch=('x86_64')
url="https://github.com/swordreforge/wawa"
license=('MIT')
depends=(
    'wayland'
    # Common image format backends — each is loaded on demand by SAIL's
    # dynamic codec plugin (dlopen), not linked into wawa itself.
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
)
provides=("${_pkgname}=${pkgver}")
conflicts=("${_pkgname}")
source=("git+https://github.com/swordreforge/wawa.git")
sha256sums=('SKIP')

pkgver() {
    cd "$srcdir/${_pkgname}"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$srcdir/${_pkgname}"

    # SAIL is bundled as a subproject. Codecs are built as standalone
    # MODULE .so plugins loaded via dlopen at runtime (SAIL_COMBINE_CODECS=OFF).
    # SAIL's cmake installs them to /usr/lib/sail/codecs/ automatically.
    local cmake_options=(
        -B build
        -Wno-dev
        -D CMAKE_BUILD_TYPE=None
        -D CMAKE_INSTALL_PREFIX=/usr
        -D CMAKE_INSTALL_LIBDIR=lib
        -D WAWA_MARCH=x86-64
    )
    cmake "${cmake_options[@]}"
    cmake --build build
}

check() {
    cd "$srcdir/${_pkgname}"
    # Verify the binary can at least print its usage (no crash on startup)
    ./build/wawa 2>&1 | grep -q "usage:" || return 1
}

package() {
    cd "$srcdir/${_pkgname}"

    DESTDIR="$pkgdir" cmake --install build

    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 sail/LICENSE.txt "$pkgdir/usr/share/licenses/$pkgname/LICENSE.sail"
}
