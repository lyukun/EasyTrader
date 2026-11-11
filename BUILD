prebuilt_cc_library(
    name = "folly",
    hdrs = [],
    static_library = "third_party/folly/lib/libfolly.a",
)

prebuilt_cc_library(
    name = "fmt",
    hdrs = [],
    static_library = "third_party/fmt/lib/libfmt.a",
)

prebuilt_cc_library(
    name = "spdlog",
    hdrs = [],
    static_library = "third_party/spdlog/lib/libspdlog.a",
    deps = [":fmt"],
)

cc_library(
    name = "trading_core",
    srcs = [
        "src/trading/adapters/book_sync.cpp",
        "src/trading/adapters/fake_clients.cpp",
        "src/trading/cache/cache.cpp",
        "src/trading/common/live_event_loop.cpp",
        "src/trading/data/data_engine.cpp",
        "src/trading/execution/execution_engine.cpp",
        "src/trading/execution/twap_algorithm.cpp",
        "src/trading/model/order_book.cpp",
        "src/trading/model/trading_state.cpp",
        "src/trading/model/types.cpp",
        "src/trading/portfolio/portfolio.cpp",
        "src/trading/risk/risk_engine.cpp",
        "src/trading/system/trader.cpp",
        "src/trading/system/trading_kernel.cpp",
        "src/trading/system/trading_node.cpp",
        "src/trading/trading/ema_cross_strategy.cpp",
        "src/trading/trading/example_strategy.cpp",
        "src/trading/trading/order_book_imbalance_strategy.cpp",
        "src/trading/trading/twap_example_strategy.cpp",
    ],
    hdrs = glob(["src/trading/**/*.h"]),
    deps = [":folly", ":spdlog"],
    export_incs = ["src"],
)

cc_binary(
    name = "trading_demo",
    srcs = ["src/main.cpp"],
    deps = [":trading_core"],
)

cc_binary(
    name = "core_test",
    srcs = ["tests/core_test.cpp"],
    deps = [":trading_core"],
)

cc_binary(
    name = "book_sync_test",
    srcs = ["tests/book_sync_test.cpp"],
    deps = [":trading_core"],
)

cc_binary(
    name = "twap_demo",
    srcs = ["src/twap_main.cpp"],
    deps = [":trading_core"],
)

cc_binary(
    name = "twap_test",
    srcs = ["tests/twap_test.cpp"],
    deps = [":trading_core"],
)
