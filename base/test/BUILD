package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//visibility:private"],
)

cc_library(
    name = "util",
    srcs = ["bazel_util.cc"],
    hdrs = [
        "bazel_util.h",
        "seeded_test.h",
    ],
    deps = [
        "@bazel_tools//tools/cpp/runfiles",
        "@googletest//:gtest",
        "//base",
    ],
    testonly = True,
    visibility = ["//visibility:public"],
)

cc_test(
    name = "arithmetic",
    srcs = ["arithmetic.cc"],
    deps = [
        "//base",
        ":util",
        "@googletest//:gtest_main",
    ],
    size = "small",
)

cc_test(
    name = "modulus_remainder",
    srcs = ["modulus_remainder.cc"],
    deps = [
        "//base",
        "@googletest//:gtest_main",
    ],
    size = "small",
)

cc_test(
    name = "set",
    srcs = ["set.cc"],
    deps = [
        "//base",
        "@googletest//:gtest_main",
    ],
    size = "small",
)

cc_test(
    name = "thread_pool",
    srcs = ["thread_pool.cc"],
    deps = [
        "//base:thread_pool_impl",
        "@googletest//:gtest_main",
    ],
    size = "small",
)

cc_test(
    name = "arithmetic_benchmark",
    srcs = ["arithmetic_benchmark.cc"],
    deps = [
        "//base",
        "@google_benchmark//:benchmark_main",
    ],
    args=["--benchmark_min_time=1x"],
    size = "small",
)


cc_test(
    name = "thread_pool_benchmark",
    srcs = ["thread_pool_benchmark.cc"],
    deps = [
        "//base:thread_pool_impl",
        "@google_benchmark//:benchmark_main",
    ],
    args=["--benchmark_min_time=1x"],
    size = "small",
)

cc_test(
    name = "atomic_wait_benchmark",
    srcs = ["atomic_wait_benchmark.cc"],
    deps = [
        "//base",
        "@google_benchmark//:benchmark_main",
    ],
    args=["--benchmark_min_time=1x"],
    size = "small",
)
