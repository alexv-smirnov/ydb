GTEST_UGLY()

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/grpc
    ${ARCADIA_ROOT}/contrib/libs/grpc
)

PEERDIR(
    contrib/libs/grpc/src/proto/grpc/health/v1
    contrib/libs/grpc/src/proto/grpc/core
    contrib/libs/grpc/src/proto/grpc/testing
    contrib/libs/grpc/src/proto/grpc/testing/duplicate
    contrib/libs/grpc/test/core/util
    contrib/libs/grpc/test/cpp/end2end
    contrib/libs/grpc/test/cpp/util
)

NO_COMPILER_WARNINGS()

SRCDIR(
    contrib/libs/grpc/test/cpp/end2end
)

SRCS(
    health_service_end2end_test.cc
)

END()
