TEMPLATE = lib
TARGET = vault
TARGET = $$qtLibraryTarget($$TARGET)
TARGETPATH = $$[QT_INSTALL_LIBS]

QT -= gui

CONFIG += create_pc create_prl no_install_prl link_pkgconfig

MOC_DIR = $$OUT_PWD/.moc
OBJECTS_DIR = $$OUT_PWD/.obj
RCC_DIR = $$OUT_PWD/.rcc

SOURCES += unit.cpp
HEADERS += unit.h

develheaders.path = /usr/include/vault
develheaders.files = unit.h

target.path = $$[QT_INSTALL_LIBS]
pkgconfig.files = $$TARGET.pc
pkgconfig.path = $$target.path/pkgconfig

QMAKE_PKGCONFIG_NAME = lib$$TARGET
QMAKE_PKGCONFIG_DESCRIPTION = Vault development files
QMAKE_PKGCONFIG_LIBDIR = $$target.path
QMAKE_PKGCONFIG_INCDIR = $$develheaders.path
QMAKE_PKGCONFIG_DESTDIR = pkgconfig
QMAKE_PKGCONFIG_REQUIRES = Qt5Core
QMAKE_PKGCONFIG_VERSION = $$VERSION

INSTALLS += target develheaders pkgconfig
