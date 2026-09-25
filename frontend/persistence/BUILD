#
# Copyright 2026 The Stelaxis Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

load("@com_google_protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@com_google_protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//:__subpackages__"],
)

licenses(["notice"])

proto_library(
    name = "persistence_proto",
    srcs = ["persistence.proto"],
    deps = [
        "@com_google_googleapis//google/spanner/v1:spanner_proto",
        "@com_google_protobuf//:struct_proto",
    ],
)

cc_proto_library(
    name = "persistence_cc_proto",
    deps = [":persistence_proto"],
)

cc_library(
    name = "file_system",
    srcs = ["file_system.cc"],
    hdrs = ["file_system.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "mem_file_system",
    testonly = True,
    srcs = ["mem_file_system.cc"],
    hdrs = ["mem_file_system.h"],
    deps = [
        ":file_system",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_library(
    name = "log",
    srcs = ["log.cc"],
    hdrs = ["log.h"],
    deps = [
        ":file_system",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/crc:crc32c",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_library(
    name = "codec",
    srcs = ["codec.cc"],
    hdrs = ["codec.h"],
    deps = [
        ":persistence_cc_proto",
        "//backend/database",
        "//backend/database:database_log",
        "//backend/datamodel:key",
        "//backend/datamodel:key_range",
        "//backend/schema/catalog:proto_bundle",
        "//backend/schema/printer:print_ddl",
        "//backend/storage:recording_storage",
        "//frontend/converters:types",
        "//frontend/converters:values",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/spanner/v1:spanner_cc_grpc",
        "@com_google_googlesql//googlesql/base:status",
        "@com_google_googlesql//googlesql/public:type",
        "@com_google_googlesql//googlesql/public:value",
    ],
)

cc_library(
    name = "manager",
    srcs = ["manager.cc"],
    hdrs = ["manager.h"],
    deps = [
        ":codec",
        ":file_system",
        ":log",
        ":persistence_cc_proto",
        "//backend/database",
        "//backend/database:database_log",
        "//backend/datamodel:key",
        "//backend/datamodel:key_range",
        "//backend/schema/catalog:schema",
        "//backend/schema/updater:schema_updater",
        "//backend/storage",
        "//backend/storage:iterator",
        "//backend/storage:recording_storage",
        "//common:clock",
        "//frontend/collections:catalog_persistence",
        "//frontend/collections:database_manager",
        "//frontend/collections:instance_manager",
        "//frontend/common:uris",
        "//frontend/entities:database",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/spanner/admin/database/v1:database_cc_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_cc_grpc",
        "@com_google_googlesql//googlesql/base:status",
    ],
)

cc_test(
    name = "log_test",
    srcs = ["log_test.cc"],
    deps = [
        ":file_system",
        ":log",
        ":mem_file_system",
        "@com_google_absl//absl/crc:crc32c",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googlesql//googlesql/base/testing:status_matchers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "persistence_test",
    size = "medium",
    srcs = ["persistence_test.cc"],
    deps = [
        ":manager",
        ":mem_file_system",
        "//backend/access:read",
        "//backend/access:write",
        "//backend/database",
        "//backend/datamodel:key",
        "//backend/datamodel:key_range",
        "//backend/datamodel:key_set",
        "//backend/schema/catalog:schema",
        "//backend/schema/printer:print_ddl",
        "//backend/schema/updater:schema_updater",
        "//backend/storage:iterator",
        "//backend/transaction:read_only_transaction",
        "//backend/transaction:read_write_transaction",
        "//common:clock",
        "//frontend/collections:database_manager",
        "//frontend/collections:instance_manager",
        "//frontend/entities:database",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/spanner/admin/database/v1:database_cc_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_cc_grpc",
        "@com_google_googlesql//googlesql/base/testing:status_matchers",
        "@com_google_googlesql//googlesql/public:json_value",
        "@com_google_googlesql//googlesql/public:value",
        "@com_google_googletest//:gtest_main",
    ],
)
