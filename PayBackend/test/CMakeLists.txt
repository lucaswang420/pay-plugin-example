cmake_minimum_required(VERSION 3.5)
project(PayBackendTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

set(TEST_SRC
    CreatePaymentIntegrationTest.cc  # [PASS] Updated to new API
    ControllerMetricsTest.cc
    IdempotencyIntegrationTest.cc
    test_main.cc
    PayAuthFilterTest.cc
    PayAuthMetricsTest.cc
    PayUtilsTest.cc
    QueryOrderTest.cc
    WechatPayClientTest.cc
    WechatCallbackIntegrationTest.cc  # [PASS] Updated to new API (Phase 3)
    RefundQueryTest.cc  # [PASS] Updated to new API (Phase 3)
    ReconcileSummaryTest.cc  # [PASS] Updated to new API (Phase 3)
)

add_executable(
    ${PROJECT_NAME}
    ${TEST_SRC}
    ../filters/PayAuthFilter.cc
    ../filters/PayAuthMetrics.cc
    ../models/PayCallback.cc
    ../models/PayIdempotency.cc
    ../models/PayLedger.cc
    ../models/PayOrder.cc
    ../models/PayPayment.cc
    ../models/PayRefund.cc
    ../controllers/MetricsController.cc
    ../controllers/PayMetricsController.cc
    ../plugins/PayPlugin.cc
    ../plugins/WechatPayClient.cc
    ../plugins/AlipaySandboxClient.cc
    ../services/PaymentService.cc
    ../services/RefundService.cc
    ../services/CallbackService.cc
    ../services/IdempotencyService.cc
    ../services/ReconciliationService.cc
    ../utils/PayUtils.cc
    ../utils/ConfigLoader.cc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
)

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon OpenSSL::Crypto OpenSSL::SSL)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /FI"${CMAKE_CURRENT_SOURCE_DIR}/../models/orm_compat.h"
        /utf-8
        /bigobj  # Required for large object files (test files with lots of templates)
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
        _CRT_SECURE_NO_WARNINGS
    )
endif()

add_test(
    NAME PayBackendTests
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/run_all_tests.ps1"
            "$<TARGET_FILE:${PROJECT_NAME}>"
)
