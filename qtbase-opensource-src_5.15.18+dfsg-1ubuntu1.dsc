-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: qtbase-opensource-src
Binary: libqt5core5t64, libqt5gui5t64, qt5-gtk-platformtheme, qt5-xdgdesktopportal-platformtheme, libqt5network5t64, libqt5opengl5t64, libqt5sql5t64, libqt5sql5-mysql, libqt5sql5-odbc, libqt5sql5-psql, libqt5sql5-sqlite, libqt5sql5-tds, libqt5sql5-ibase, libqt5xml5t64, libqt5dbus5t64, libqt5test5t64, libqt5concurrent5t64, libqt5widgets5t64, libqt5printsupport5t64, qtbase5-dev, qtbase5-private-dev, libqt5opengl5-dev, qtbase5-dev-tools, qt5-qmake-bin, qt5-qmake, qtbase5-examples, qtbase5-doc, qtbase5-doc-html, qtbase5-doc-dev
Architecture: any all
Version: 5.15.18+dfsg-1ubuntu1
Maintainer: Ubuntu Developers <ubuntu-devel-discuss@lists.ubuntu.com>
Uploaders: Sune Vuorela <sune@debian.org>, Pino Toscano <pino@debian.org>, Timo Jyrinki <timo@debian.org>, Dmitry Shachnev <mitya57@debian.org>, Simon Quigley <tsimonq2@debian.org>
Homepage: https://www.qt.io/developers/
Standards-Version: 4.7.3
Vcs-Browser: https://salsa.debian.org/qt-kde-team/qt/qtbase
Vcs-Git: https://salsa.debian.org/qt-kde-team/qt/qtbase.git
Build-Depends: debhelper-compat (= 13), default-libmysqlclient-dev, dh-exec, dpkg-dev (>= 1.22.5), firebird-dev [!hurd-any !m68k !x32], freetds-dev, libasound2-dev [linux-any], libatspi2.0-dev, libcups2-dev, libdbus-1-dev, libdouble-conversion-dev, libdrm-dev [linux-any], libfontconfig-dev, libfreetype-dev, libgbm-dev [linux-any], libgl-dev, libgles-dev, libglib2.0-dev, libglu1-mesa-dev | libglu-dev, libgtk-3-dev, libharfbuzz-dev, libicu-dev, libinput-dev [linux-any], libjpeg-dev, libkrb5-dev, libmd4c-dev, libmtdev-dev [linux-any], libpcre2-dev, libpng-dev, libpq-dev, libproxy-dev, libpulse-dev, libsqlite3-dev, libssl-dev, libudev-dev [linux-any], libvulkan-dev, libx11-dev, libx11-xcb-dev, libxcb-glx0-dev, libxcb-icccm4-dev, libxcb-image0-dev, libxcb-keysyms1-dev, libxcb-randr0-dev, libxcb-render-util0-dev, libxcb-render0-dev, libxcb-shape0-dev, libxcb-shm0-dev, libxcb-sync-dev, libxcb-util-dev, libxcb-xfixes0-dev, libxcb-xinerama0-dev, libxcb-xinput-dev, libxcb-xkb-dev, libxcb1-dev, libxext-dev, libxi-dev, libxkbcommon-dev, libxkbcommon-x11-dev, libxrender-dev, libzstd-dev, pkg-kde-tools, pkgconf, publicsuffix, qt5-qmake-bin <cross>, unixodbc-dev, zlib1g-dev
Build-Depends-Indep: qdoc-qt5 (>= 5.15~) <!nodoc>, qhelpgenerator-qt5 (>= 5.15~) <!nodoc>, qtattributionsscanner-qt5 <!nodoc>
Package-List:
 libqt5concurrent5t64 deb libs optional arch=any
 libqt5core5t64 deb libs optional arch=any
 libqt5dbus5t64 deb libs optional arch=any
 libqt5gui5t64 deb libs optional arch=any
 libqt5network5t64 deb libs optional arch=any
 libqt5opengl5-dev deb libdevel optional arch=any
 libqt5opengl5t64 deb libs optional arch=any
 libqt5printsupport5t64 deb libs optional arch=any
 libqt5sql5-ibase deb libs optional arch=amd64,arm64,armel,armhf,i386,mips64el,ppc64el,riscv64,s390x,alpha,hppa,loong64,powerpc,ppc64,sh4,sparc64
 libqt5sql5-mysql deb libs optional arch=any
 libqt5sql5-odbc deb libs optional arch=any
 libqt5sql5-psql deb libs optional arch=any
 libqt5sql5-sqlite deb libs optional arch=any
 libqt5sql5-tds deb libs optional arch=any
 libqt5sql5t64 deb libs optional arch=any
 libqt5test5t64 deb libs optional arch=any
 libqt5widgets5t64 deb libs optional arch=any
 libqt5xml5t64 deb libs optional arch=any
 qt5-gtk-platformtheme deb libs optional arch=any
 qt5-qmake deb devel optional arch=any
 qt5-qmake-bin deb devel optional arch=any
 qt5-xdgdesktopportal-platformtheme deb libs optional arch=any
 qtbase5-dev deb libdevel optional arch=any
 qtbase5-dev-tools deb devel optional arch=any
 qtbase5-doc deb doc optional arch=all profile=!nodoc profile:v1=!nodoc
 qtbase5-doc-dev deb libdevel optional arch=all profile=!nodoc profile:v1=!nodoc
 qtbase5-doc-html deb doc optional arch=all profile=!nodoc profile:v1=!nodoc
 qtbase5-examples deb x11 optional arch=any
 qtbase5-private-dev deb libdevel optional arch=any
Checksums-Sha1:
 a3b7b5ac4b67a9465f0e7dceb05d052df5332603 49200288 qtbase-opensource-src_5.15.18+dfsg.orig.tar.xz
 a4b06fa63ebfa25f435cc55a59a2c94c6af732c8 227900 qtbase-opensource-src_5.15.18+dfsg-1ubuntu1.debian.tar.xz
Checksums-Sha256:
 2db9dde2b8eb78bafe8428516d6a871872d1642d532c9bd87aa936e902f2c878 49200288 qtbase-opensource-src_5.15.18+dfsg.orig.tar.xz
 c491fd6f184efa7ac6ea253c7956c1392f89463db9aee33e055dc63ca88d638a 227900 qtbase-opensource-src_5.15.18+dfsg-1ubuntu1.debian.tar.xz
Files:
 4b61c7df2dc0540f151aad0942a86d64 49200288 qtbase-opensource-src_5.15.18+dfsg.orig.tar.xz
 7b8bcd3bb5ccba598f3e456fd8e27a5c 227900 qtbase-opensource-src_5.15.18+dfsg-1ubuntu1.debian.tar.xz
Original-Maintainer: Debian Qt/KDE Maintainers <debian-qt-kde@lists.debian.org>

-----BEGIN PGP SIGNATURE-----

iQJHBAEBCgAxFiEE8kKZ/xu8kBi5BqTLYCaTbS8ciuAFAmlhGsQTHG1pdHlhNTdA
ZGViaWFuLm9yZwAKCRBgJpNtLxyK4K8qEACRga1+Brd06qfAAHJMPpaCJNK0rqYL
/TfwroXav4sBXWBjTpMruN4wQH0vdQEr9YjueC4uroP5P1vDTZF0aDP0caLs2j62
jwBc56Y8gCGE+GXdSOBPfJwHPbXkApfkPzOSqXdxfCrYJMaX4H7JL11j4OR1dLAm
M/IUlypRzLAw1LF0mEEbiCqHnmDh5J/GON9ORc58D1KiGIHmQKprngiwv10e3C9I
wdkOYPqtRiBIocdzsmBychanCqUw106/r2puG0jaB/HrsIHoO2bEPTUtt+vJpm8v
1q0827s0rn1A0JfS65rWLj0rlDayqvCCQvMKfJIytEYizj2D5RgySIFD5BM5oT3w
hpB4LVBEcbBn7TYlbIKC+P84TMvdXvBK+sVPOrhHBR8YC5SQEqP0tdkNx+DWf2rk
74aVz3KMSPVW1k++K8Sdr8n8MUR0j8yOIJNk2piETgno62qogqdSH6rX/Kel1tNi
cqnXuxAY28ReioZRim1WIZ2fi29w2884TobszxdslLjW4P0Ll3o2OAmgAWtPH8KJ
IBNAYe5C+ErOleAvCmrWgouMYkBkZvqD3mAtwxG6BYe8vGlCLdW5j6ONXFXjedzM
Ki/YazJquuZlYEQ47o+eiQeeCq6d8IVJTByNvDRM8YiTxGCLJMQTnhkJD163Q8O/
RLyUrK54B41vIg==
=aGz+
-----END PGP SIGNATURE-----
