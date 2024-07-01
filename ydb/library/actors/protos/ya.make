PROTO_LIBRARY()

# trigger rebuild

SRCS(
    actors.proto
    interconnect.proto
    services_common.proto
    unittests.proto
)

EXCLUDE_TAGS(GO_PROTO)

END()
