#-------------------------------------------------
#
# Project created by QtCreator 2018-05-05T17:41:00
#
#-------------------------------------------------

QT -= core gui

TARGET = moonlight-common-c
TEMPLATE = lib

# Build a static library
CONFIG += staticlib

# Include global qmake defs
include(../globaldefs.pri)

win32 {
    contains(QT_ARCH, i386) {
        INCLUDEPATH += $$PWD/../libs/windows/include/x86
    }
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../libs/windows/include/x64
    }
    contains(QT_ARCH, arm64) {
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64
    }

    INCLUDEPATH += $$PWD/../libs/windows/include
    DEFINES += HAS_QOS_FLOWID=1 HAS_PQOS_FLOWID=1
}
macx:!disable-prebuilts {
    INCLUDEPATH += $$PWD/../libs/mac/include
}
unix:if(!macx|disable-prebuilts) {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl
}

COMMON_C_DIR = $$PWD/moonlight-common-c
ENET_DIR = $$COMMON_C_DIR/enet
SOURCES += \
    $$ENET_DIR/callbacks.c \
    $$ENET_DIR/compress.c \
    $$ENET_DIR/host.c \
    $$ENET_DIR/list.c \
    $$ENET_DIR/packet.c \
    $$ENET_DIR/peer.c \
    $$ENET_DIR/protocol.c \
    $$ENET_DIR/unix.c \
    $$ENET_DIR/win32.c \
    $$COMMON_C_DIR/nanors/deps/obl/oblas_common.c \
    $$COMMON_C_DIR/nanors/deps/obl/oblas_lite.c \
    $$COMMON_C_DIR/nanors/rs.c \
    $$COMMON_C_DIR/src/AudioStream.c \
    $$COMMON_C_DIR/src/ByteBuffer.c \
    $$COMMON_C_DIR/src/Connection.c \
    $$COMMON_C_DIR/src/ConnectionTester.c \
    $$COMMON_C_DIR/src/ControlStream.c \
    $$COMMON_C_DIR/src/FakeCallbacks.c \
    $$COMMON_C_DIR/src/InputStream.c \
    $$COMMON_C_DIR/src/LinkedBlockingQueue.c \
    $$COMMON_C_DIR/src/Misc.c \
    $$COMMON_C_DIR/src/Platform.c \
    $$COMMON_C_DIR/src/PlatformCrypto.c \
    $$COMMON_C_DIR/src/PlatformSockets.c \
    $$COMMON_C_DIR/src/RtpAudioQueue.c \
    $$COMMON_C_DIR/src/RtpVideoQueue.c \
    $$COMMON_C_DIR/src/RtspConnection.c \
    $$COMMON_C_DIR/src/RtspParser.c \
    $$COMMON_C_DIR/src/SdpGenerator.c \
    $$COMMON_C_DIR/src/SimpleStun.c \
    $$COMMON_C_DIR/src/VideoDepacketizer.c \
    $$COMMON_C_DIR/src/VideoStream.c
HEADERS += \
    $$COMMON_C_DIR/src/Limelight.h
INCLUDEPATH += \
    $$ENET_DIR/include \
    $$COMMON_C_DIR/src \
    $$COMMON_C_DIR/nanors \
    $$COMMON_C_DIR/nanors/deps \
    $$COMMON_C_DIR/nanors/deps/obl
DEFINES += HAS_SOCKLEN_T

# DualSense waveform haptics (Vibeshine): apply this fork's small patch to the
# submodule at qmake time. Already applied is skipped; anything else stops the build.
macx {
    HAPTICS_DIR = $$PWD/haptics
    HAPTICS_GIT = git -C $$shell_quote($$COMMON_C_DIR)
    HAPTICS_PATCH = $$shell_quote($$HAPTICS_DIR/common-c-haptics.patch)
    system("$$HAPTICS_GIT apply --reverse --check $$HAPTICS_PATCH > /dev/null 2>&1") {
        message("common-c haptics patch already applied")
    } else:system("$$HAPTICS_GIT apply --check $$HAPTICS_PATCH > /dev/null 2>&1") {
        !system("$$HAPTICS_GIT apply $$HAPTICS_PATCH") {
            error("Applying the common-c haptics patch failed.")
        }
        message("Applied the common-c haptics patch")
    } else {
        error("The common-c haptics patch does not apply. Reset the submodule with: git -C $$COMMON_C_DIR checkout -- . then re-run qmake. If it still fails, upstream changed the patched lines; regenerate moonlight-common-c/haptics/common-c-haptics.patch.")
    }

    DEFINES += HAVE_COMMON_C_HAPTICS
    INCLUDEPATH += $$HAPTICS_DIR
    HEADERS += $$HAPTICS_DIR/ControllerHaptics.h
    SOURCES += $$HAPTICS_DIR/ControllerHapticsHook.c
}

CONFIG(debug, debug|release) {
    # Enable asserts on debug builds
    DEFINES += LC_DEBUG
}

# Enable support for C11 atomics on MSVC
*-msvc {
    QMAKE_CFLAGS += -std:c11 -experimental:c11atomics
}

# Disable unused parameter warnings on GCC and Clang
*-g++|*-clang* {
    QMAKE_CFLAGS_WARN_ON += -Wno-unused-parameter
}
