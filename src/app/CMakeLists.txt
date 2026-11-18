set(TERMX_APP_SOURCES
    main.cpp
    MainWindow.cpp MainWindow.h
    TerminalWidget.cpp TerminalWidget.h
    TerminalView.cpp TerminalView.h
    TabPage.cpp TabPage.h
    SessionDialog.cpp SessionDialog.h
    PromptDialog.cpp PromptDialog.h
    Panels.cpp Panels.h
    Theme.cpp Theme.h
    CrashHandler.cpp CrashHandler.h
    PreferencesDialog.cpp
    QuickConnectBar.cpp QuickConnectBar.h
    SnippetPanel.cpp SnippetPanel.h
    CommandPalette.cpp CommandPalette.h
    ${PROJECT_SOURCE_DIR}/resources/termx.qrc
)
if(WIN32)
    # Forward slashes: the resource compiler treats backslashes as escapes.
    set(TERMX_ICON_PATH "${PROJECT_SOURCE_DIR}/resources/icons/termx.ico")
    configure_file(termx.rc.in ${CMAKE_CURRENT_BINARY_DIR}/termx.rc @ONLY)
    list(APPEND TERMX_APP_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/termx.rc termx.manifest)
endif()

add_executable(termx WIN32 ${TERMX_APP_SOURCES})
set_target_properties(termx PROPERTIES AUTORCC ON)
target_link_libraries(termx PRIVATE termx_core Qt6::Widgets Qt6::Gui ${TERMX_SSH2_TARGET} termx_options)
if(WIN32)
    target_link_libraries(termx PRIVATE dwmapi dbghelp)
    set_target_properties(termx PROPERTIES OUTPUT_NAME "TermX")
endif()

install(TARGETS termx RUNTIME DESTINATION .)
