LibVNCServer release artifacts

Version: 0.9.16
Platform: windows-x86_64
Build type: Release
Source commit: 73bb369
Build timestamp (UTC): 2026-06-10T09:26:42Z

These binaries were generated using:
  cmake -S . -B /tmp/libvncserver-win-build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-cross-mingw32-linux.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_TESTS=OFF -DWITH_EXAMPLES=ON \
    -DWITH_ZLIB=OFF -DWITH_LZO=OFF -DWITH_JPEG=OFF -DWITH_PNG=OFF \
    -DWITH_SDL=OFF -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_FFMPEG=OFF \
    -DWITH_OPENSSL=OFF -DWITH_GCRYPT=OFF -DWITH_GNUTLS=OFF \
    -DWITH_SYSTEMD=OFF -DWITH_SASL=OFF -DWITH_XCB=OFF -DWITH_LIBSSHTUNNEL=OFF
  cmake --build /tmp/libvncserver-win-build -j$(nproc)

Included executables are under:
  examples/server/
  examples/client/

Bundled runtime dependency:
  examples/server/libwinpthread-1.dll
  examples/client/libwinpthread-1.dll
