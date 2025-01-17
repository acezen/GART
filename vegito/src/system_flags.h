/** Copyright 2020-2023 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VEGITO_SRC_SYSTEM_FLAGS_H_
#define VEGITO_SRC_SYSTEM_FLAGS_H_

#include <gflags/gflags_declare.h>

DECLARE_string(kafka_broker_list);
DECLARE_string(kafka_unified_log_topic);
DECLARE_string(kafka_unified_log_file);  // for tests

DECLARE_string(etcd_endpoint);
DECLARE_string(meta_prefix);
DECLARE_string(v6d_ipc_socket);

DECLARE_string(schema_file_path);
DECLARE_string(table_schema_file_path);

DECLARE_int32(server_num);
DECLARE_int32(server_id);

#endif  // VEGITO_SRC_SYSTEM_FLAGS_H_
