# Required to generate MOC
target_sources(
  flameshot
  PRIVATE abstractlogger.h
          crashhandler.h
          translationmanager.h
          zaggertr.h
          filenamehandler.h
          screengrabber.h
          systemnotification.h
          valuehandler.h
          strfparse.h
)

target_sources(
  flameshot
  PRIVATE abstractlogger.cpp
          crashhandler.cpp
          translationmanager.cpp
          filenamehandler.cpp
          screengrabber.cpp
          monitorpreview.cpp
          confighandler.cpp
          systemnotification.cpp
          valuehandler.cpp
          screenshotsaver.cpp
          globalvalues.cpp
          desktopfileparse.cpp
          desktopinfo.cpp
          pathinfo.cpp
          colorutils.cpp
          history.cpp
          strfparse.cpp
)

IF (UNIX AND NOT APPLE)
target_sources(
  flameshot
  PRIVATE request.h
          request.cpp
)
ENDIF()

IF (WIN32)
  target_sources(
    flameshot
    PRIVATE scrollcapture.h
            scrollcapture.cpp
            scrolluia.h
            scrolluia.cpp
            winlnkfileparse.cpp
  )
ENDIF()
