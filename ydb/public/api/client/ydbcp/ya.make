OWNER(g:cloud-api)
PROTO_LIBRARY()

# PROTO_NAMESPACE(GLOBAL cloud/bitbucket/private-api)
PY_NAMESPACE(yandex.cloud.priv.ydb.v1)

GRPC()
SRCS(
    backup.proto
    backup_service.proto
    cluster_service.proto
    console_service.proto
    database.proto
    database_service.proto
    location.proto
    location_service.proto
#    operation_service.proto
#    quota_service.proto
#    resource_preset.proto
#    resource_preset_service.proto
#    storage_type.proto
#    storage_type_service.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
    rpc/code
    rpc/errdetails
    rpc/status
    type/timeofday
    type/dayofweek
)

PEERDIR(
    ydb/public/api/client/yc_private/access
    ydb/public/api/client/yc_private/operation
)

END()

