OWNER(g:cloud-api)
PROTO_LIBRARY()

# PROTO_NAMESPACE(GLOBAL cloud/bitbucket/private-api)
PY_NAMESPACE(yandex.cloud.priv.access)

GRPC()
SRCS(
    access.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
    rpc/code
    rpc/errdetails
    rpc/status
    type/timeofday
    type/dayofweek
)

END()
