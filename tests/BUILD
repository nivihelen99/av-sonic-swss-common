package(default_visibility = ["//visibility:public"])

cc_test(
    name = "status_code_util_test",
    srcs = ["status_code_util_test.cpp"],
    deps = [
        "//:libswsscommon",
        "@com_github_google_glog//:glog",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "typed_redis_ut",
    srcs = ["typed_redis_ut.cpp"],
    deps = [
        "//:libswsscommon",
        "@com_github_google_glog//:glog", # Assuming glog might be used by dependencies or test infra
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "saiaclschema_ut",
    srcs = ["saiaclschema_ut.cpp"],
    deps = [
        "//:libswsscommon",
        "@com_github_google_glog//:glog",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)