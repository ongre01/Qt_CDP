QT += network websockets widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    cdp/cdpclient.cpp \
    korail/autobookingcontroller.cpp \
    korail/korailauthcontroller.cpp \
    korail/traininfoparser.cpp \
    recorder/pagerecorder.cpp \
    recorder/snapshotstorage.cpp

HEADERS += \
    mainwindow.h \
    cdp/cdpclient.h \
    korail/autobookingcontroller.h \
    korail/korailauthcontroller.h \
    korail/traininfo.h \
    korail/traininfoparser.h \
    recorder/pagerecorder.h \
    recorder/snapshotstorage.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
