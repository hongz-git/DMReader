QT += core gui websockets texttospeech multimedia
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = DMReader
TEMPLATE = app
CONFIG += c++17

SOURCES += main.cpp mainwindow.cpp qwenttsclient.cpp
HEADERS += mainwindow.h qwenttsclient.h
