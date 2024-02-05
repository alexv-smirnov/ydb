PROTO_LIBRARY()

GRPC()
SRCS(
    operation.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
    rpc/status
)

END()

