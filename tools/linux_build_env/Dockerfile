# Cubic.Heroes's Linux build environment (M11.4, docs/LINUX_PACKAGING.md): the glibc 2.31 baseline the Linux packages
# are made against. Debian 11 (glibc 2.31) pinned by digest; Python and OpenSSL built from pinned sources here (Debian
# 11's Python is 3.9 and its OpenSSL 1.1.1 -- the worker ships the Python it is frozen with, and Windows' carries
# OpenSSL 3); the X11, Wayland, PipeWire, PulseAudio and ALSA headers SDL2's and OpenAL Soft's run-time backends are
# compiled against are this baseline's (nothing of them is linked). Zig, the libraries and the game's runtime come
# from the checkout's own tools, run inside (tools/linux_package.py).
#
#   docker build -t cubic-heroes-linux-build tools/linux_build_env
#
# Nothing from the game is in the image; the checkout (with its git-ignored build/) is mounted when it runs.
FROM debian@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9

ARG OPENSSL_VERSION=3.5.9
ARG OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
ARG PYTHON_VERSION=3.12.16
ARG PYTHON_SHA256=b5796915dc45ff745cfb4e990a7e7c93d6f546328a102ce31448d69ebc37575c

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8

# the tools to build Python and OpenSSL; the run-time backends' headers (PipeWire from bullseye-backports, now in
# Debian's archive -- fixed, as a baseline should be: OpenAL Soft 1.25 needs a newer one than bullseye's 0.3.19);
# squashfs-tools: the AppImages' file system image (tools/make_appimage.py)
# the packages as they were on one day (snapshot.debian.org, before Debian 11's end of life), so the image is the same
# whenever it is built; bullseye-backports from Debian's archive (frozen since it left the mirrors)
ARG DEBIAN_SNAPSHOT=20260801T000000Z
RUN printf '%s\n' \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye main" \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye-updates main" \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bullseye-security main" \
      "deb [check-valid-until=no] http://archive.debian.org/debian bullseye-backports main" \
      > /etc/apt/sources.list \
 && rm -f /etc/apt/sources.list.d/* \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      build-essential ca-certificates curl xz-utils bzip2 git file binutils patchelf perl pkg-config \
      zlib1g-dev libbz2-dev libffi-dev liblzma-dev libsqlite3-dev libreadline-dev uuid-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdrm-dev libgbm-dev libegl1-mesa-dev libudev-dev libdbus-1-dev \
      libpulse-dev libasound2-dev squashfs-tools \
 && apt-get -o Acquire::Retries=5 install -y --no-install-recommends -t bullseye-backports libpipewire-0.3-dev libspa-0.2-dev \
 && rm -rf /var/lib/apt/lists/*

# OpenSSL 3.5 (LTS; Apache-2.0), shared, into /opt/python (beside the Python that uses it); its licence kept for the
# packages (M11.4: tools/make_bundle.py python_runtime_licences), as Python's notices of the software it incorporates
RUN curl -fsSL -o /tmp/openssl.tar.gz \
      "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
 && echo "${OPENSSL_SHA256}  /tmp/openssl.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/openssl.tar.gz -C /tmp \
 && cd "/tmp/openssl-${OPENSSL_VERSION}" \
 && ./Configure --prefix=/opt/python --libdir=lib --openssldir=/opt/python/ssl shared no-tests \
      '-Wl,-rpath,$$ORIGIN' \
 && make -j"$(nproc)" && make install_sw install_ssldirs \
 && mkdir -p /opt/python/licenses && cp LICENSE.txt /opt/python/licenses/OpenSSL-LICENSE.txt \
 && cd / && rm -rf /tmp/openssl*

# Python, shared (PyInstaller packs libpython), linked against that OpenSSL
RUN curl -fsSL -o /tmp/python.tar.xz "https://www.python.org/ftp/python/${PYTHON_VERSION}/Python-${PYTHON_VERSION}.tar.xz" \
 && echo "${PYTHON_SHA256}  /tmp/python.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/python.tar.xz -C /tmp \
 && cd "/tmp/Python-${PYTHON_VERSION}" \
 && ./configure --prefix=/opt/python --enable-shared --with-openssl=/opt/python --with-openssl-rpath=auto \
      --without-ensurepip LDFLAGS='-Wl,-rpath,/opt/python/lib' \
 && make -j"$(nproc)" && make install \
 && cp Doc/license.rst /opt/python/licenses/Python-license.rst \
 && cd / && rm -rf /tmp/Python* \
 && /opt/python/bin/python3 -c "import ssl, ctypes, lzma, bz2, zlib, sqlite3; print(ssl.OPENSSL_VERSION)"

ENV PATH=/opt/python/bin:$PATH
# the pinned Python packages: the worker's (pycryptodome), the icon's (Pillow), the freezer (PyInstaller), the build
# tools (CMake 4 and Ninja: the same as CI's)
# (M11.4, the review's L-4): pip from its own wheel, checked by SHA-256 (no get-pip.py); every package from
# requirements.txt, each pinned by version and by its wheel's SHA-256 (--require-hashes)
COPY requirements.txt /tmp/requirements.txt
RUN curl -fsSL -o /tmp/pip-26.3-py3-none-any.whl https://files.pythonhosted.org/packages/py3/p/pip/pip-26.3-py3-none-any.whl \
 && echo "f3b60b20ffaadd4415ed7957212a38ac63fa4c5fa375bd0f4a797777940fad13  /tmp/pip-26.3-py3-none-any.whl" | sha256sum -c - \
 && python3 /tmp/pip-26.3-py3-none-any.whl/pip install --no-cache-dir --no-index /tmp/pip-26.3-py3-none-any.whl \
 && python3 -m pip install --no-cache-dir --require-hashes --only-binary=:all: -r /tmp/requirements.txt \
 && rm /tmp/pip-26.3-py3-none-any.whl /tmp/requirements.txt

# mksquashfs's zstd (tools/linux_package.py builds squashfs-tools 4.7.5 with gzip and zstd only: zstd and zlib
# linked in statically, glibc dynamically)
RUN apt-get -o Acquire::Retries=5 update && apt-get -o Acquire::Retries=5 install -y --no-install-recommends libzstd-dev \
 && rm -rf /var/lib/apt/lists/*

RUN ldd --version | head -1 && python3 --version && cmake --version | head -1
WORKDIR /src
