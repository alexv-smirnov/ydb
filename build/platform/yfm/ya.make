RESOURCES_LIBRARY()

IF(NOT HOST_OS_LINUX AND NOT HOST_OS_WINDOWS AND NOT HOST_OS_DARWIN)
    MESSAGE(FATAL_ERROR Unsupported platform for YFM tool)
ENDIF()

DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
    YFM_TOOL
    sbr:8283075371 FOR DARWIN-ARM64
    sbr:8283075371 FOR DARWIN
    sbr:8283087538 FOR LINUX
    sbr:8283475316 FOR WIN32
)

END()
