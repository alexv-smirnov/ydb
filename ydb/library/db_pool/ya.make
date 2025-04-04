LIBRARY()

SRCS(
    db_pool.cpp
)

PEERDIR(
    ydb/library/actors/core
    library/cpp/monlib/dynamic_counters
    ydb/library/db_pool/protos
    ydb/library/security
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/table
)

END()

