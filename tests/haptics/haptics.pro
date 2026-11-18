# Standalone tests for the DualSense waveform haptics core. Not part of the app build.
#   mkdir -p temp/haptics-tests && cd temp/haptics-tests
#   qmake ../../tests/haptics/haptics.pro && make && ./tst_hapticscore
QT += testlib
QT -= gui
CONFIG += console c++17 testcase
CONFIG -= app_bundle

TARGET = tst_hapticscore

INCLUDEPATH += \
    $$PWD/../../app/streaming/input \
    $$PWD/../../moonlight-common-c/haptics

SOURCES += \
    tst_hapticscore.cpp \
    $$PWD/../../app/streaming/input/hapticscore.cpp
