add_library(termx_core STATIC
    transport/Transport.cpp transport/Transport.h
    transport/SocketUtil.cpp transport/SocketUtil.h
    transport/SshTransport.cpp transport/SshTransport.h
    transport/SshContext.h transport/SftpTypes.cpp transport/SftpTypes.h
    transport/SshForwarding.cpp transport/SshForwarding.h
    transport/SftpEngine.cpp transport/SftpEngine.h
    transport/SocksProtocol.cpp transport/SocksProtocol.h
    transport/TelnetProtocol.cpp transport/TelnetProtocol.h
    transport/TelnetTransport.cpp transport/TelnetTransport.h
    transport/SerialTransport.cpp transport/SerialTransport.h
    transport/LocalShellTransport.cpp transport/LocalShellTransport.h
    terminal/VtScreen.cpp terminal/VtScreen.h
    terminal/AnsiStripper.cpp terminal/AnsiStripper.h
    terminal/SessionLogger.cpp terminal/SessionLogger.h
    terminal/TerminalSession.cpp terminal/TerminalSession.h
    history/CommandCapture.cpp history/CommandCapture.h
    history/HistoryStore.cpp history/HistoryStore.h
    serial/SerialPortWatcher.cpp serial/SerialPortWatcher.h
    serial/DeviceProfiles.cpp serial/DeviceProfiles.h
    session/SessionConfig.cpp session/SessionConfig.h
    session/PortForward.cpp session/PortForward.h
    session/SessionStore.cpp session/SessionStore.h
    session/QuickConnect.cpp session/QuickConnect.h
    session/SessionImport.cpp session/SessionImport.h
    session/SnippetStore.cpp session/SnippetStore.h
    session/CredentialStore.cpp session/CredentialStore.h
    util/AppPaths.cpp util/AppPaths.h
    util/JsonFile.cpp util/JsonFile.h
    util/SecureWipe.cpp util/SecureWipe.h
    util/FuzzyMatch.cpp util/FuzzyMatch.h
)
target_include_directories(termx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(termx_core
    PUBLIC Qt6::Core Qt6::Network Qt6::SerialPort vterm termx_options
    PRIVATE ${TERMX_SSH2_TARGET} SQLite::SQLite3)
if(WIN32)
    target_link_libraries(termx_core PRIVATE ws2_32 crypt32)
else()
    target_link_libraries(termx_core PRIVATE util)
endif()
