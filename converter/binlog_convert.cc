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

#include <fstream>

#include "vineyard/common/util/json.h"

#include "flags.h"           // NOLINT(build/include_subdir)
#include "kafka_producer.h"  // NOLINT(build/include_subdir)
#include "vegito/src/fragment/id_parser.h"

using json = vineyard::json;

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // init kafka consumer and producer
  RdKafka::Conf* conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
  std::string rdkafka_err;
  if (conf->set("metadata.broker.list", FLAGS_read_kafka_broker_list,
                rdkafka_err) != RdKafka::Conf::CONF_OK) {
    LOG(ERROR) << "Failed to set metadata.broker.list: " << rdkafka_err;
  }
  if (conf->set("group.id", "gart_consumer", rdkafka_err) !=
      RdKafka::Conf::CONF_OK) {
    LOG(ERROR) << "Failed to set group.id: " << rdkafka_err;
  }
  if (conf->set("enable.auto.commit", "false", rdkafka_err) !=
      RdKafka::Conf::CONF_OK) {
    LOG(ERROR) << "Failed to set enable.auto.commit: " << rdkafka_err;
  }
  if (conf->set("auto.offset.reset", "earliest", rdkafka_err) !=
      RdKafka::Conf::CONF_OK) {
    LOG(ERROR) << "Failed to set auto.offset.reset: " << rdkafka_err;
  }

  RdKafka::Consumer* consumer = RdKafka::Consumer::create(conf, rdkafka_err);
  if (!consumer) {
    LOG(ERROR) << "Failed to create consumer: " << rdkafka_err << std::endl;
    exit(1);
  }

  RdKafka::Conf* tconf = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);

  RdKafka::Topic* topic = RdKafka::Topic::create(
      consumer, fLS::FLAGS_read_kafka_topic, tconf, rdkafka_err);
  int32_t partition = 0;
  int64_t start_offset = RdKafka::Topic::OFFSET_BEGINNING;

  RdKafka::ErrorCode resp = consumer->start(topic, partition, start_offset);

  std::shared_ptr<KafkaProducer> producer = std::make_shared<KafkaProducer>(
      fLS::FLAGS_write_kafka_broker_list, fLS::FLAGS_write_kafka_topic);
  KafkaOutputStream ostream(producer);

  if (resp != RdKafka::ERR_NO_ERROR) {
    LOG(ERROR) << "Failed to start consumer: " << RdKafka::err2str(resp);
    exit(1);
  }

  // init graph schema
  std::map<std::string, int> vertex_tables;
  std::map<std::string, int> edge_tables;
  std::map<std::string, std::vector<std::string>> required_properties;
  std::map<std::string, std::string> vertex_label_columns;
  std::map<std::string, std::pair<std::string, std::string>> edge_label_columns;
  std::vector<std::pair<int, int>> edge_label2src_dst_labels;
  std::map<std::string, int> vertex_label2ids;

  std::vector<std::map<std::string, int64_t>> string_oid2gid_maps;
  std::vector<std::map<int64_t, int64_t>> int64_oid2gid_maps;
  std::vector<uint64_t> vertex_nums;
  std::vector<std::vector<uint64_t>> vertex_nums_per_fragment;

  std::ifstream rg_mapping_file_stream(FLAGS_rg_mapping_file_path);
  if (!rg_mapping_file_stream.is_open()) {
    LOG(ERROR) << "RGMapping file (" << FLAGS_rg_mapping_file_path
               << ") open failed."
               << "Not exist or permission denied.";
    exit(1);
  }

  json rg_mapping;
  try {
    rg_mapping = json::parse(rg_mapping_file_stream);
  } catch (json::parse_error& e) {
    LOG(ERROR) << "RGMapping file (" << FLAGS_rg_mapping_file_path
               << ") parse failed."
               << "Error message: " << e.what();
    exit(1);
  }

  auto types = rg_mapping["types"];
  auto vertex_label_num = rg_mapping["vertexLabelNum"].get<int>();

  gart::IdParser<int64_t> id_parser;
  id_parser.Init(FLAGS_numbers_of_subgraphs, vertex_label_num);

  string_oid2gid_maps.resize(vertex_label_num);
  int64_oid2gid_maps.resize(vertex_label_num);
  vertex_nums.resize(vertex_label_num, 0);
  vertex_nums_per_fragment.resize(vertex_label_num);

  for (auto idx = 0; idx < vertex_label_num; idx++) {
    vertex_nums_per_fragment[idx].resize(FLAGS_numbers_of_subgraphs, 0);
  }

  for (uint64_t idx = 0; idx < types.size(); idx++) {
    auto type = types[idx]["type"].get<std::string>();
    auto id = types[idx]["id"].get<int>();
    auto table_name = types[idx]["table_name"].get<std::string>();
    auto label = types[idx]["label"].get<std::string>();
    if (type == "VERTEX") {
      vertex_tables.emplace(table_name, id);
      vertex_label_columns.emplace(
          table_name, types[idx]["id_column_name"].get<std::string>());
      vertex_label2ids.emplace(label, id);
    } else if (type == "EDGE") {
      edge_tables.emplace(table_name, id - vertex_label_num);
      auto src_label = types[idx]["rawRelationShips"]
                           .at(0)["srcVertexLabel"]
                           .get<std::string>();
      auto dst_label = types[idx]["rawRelationShips"]
                           .at(0)["dstVertexLabel"]
                           .get<std::string>();
      edge_label_columns.emplace(table_name,
                                 std::make_pair(types[idx]["rawRelationShips"]
                                                    .at(0)["src_column_name"]
                                                    .get<std::string>(),
                                                types[idx]["rawRelationShips"]
                                                    .at(0)["dst_column_name"]
                                                    .get<std::string>()));
      auto src_label_id = vertex_label2ids.find(src_label)->second;
      auto dst_label_id = vertex_label2ids.find(dst_label)->second;
      edge_label2src_dst_labels.push_back(
          std::make_pair(src_label_id, dst_label_id));
    }
    auto properties = types[idx]["propertyDefList"];
    std::vector<std::string> required_prop_names;
    for (uint64_t prop_id = 0; prop_id < properties.size(); prop_id++) {
      auto prop_name = properties[prop_id]["column_name"].get<std::string>();
      required_prop_names.emplace_back(prop_name);
    }
    required_properties.emplace(table_name, required_prop_names);
  }

  // start to process log
  int log_count = 0;
  while (1) {
    RdKafka::Message* msg = consumer->consume(topic, partition, 1000);
    int str_len = static_cast<int>(msg->len());

    // skip empty message to avoid JSON parser error
    if (str_len == 0) {
      continue;
    }

    const char* str_addr = static_cast<const char*>(msg->payload());
    std::string line;
    line.assign(str_addr, str_addr + str_len);
    std::string content;
    bool is_edge = false;
    json log = json::parse(line);
    std::string type = log["type"].get<std::string>();
    if (type == "insert") {
      std::string table_name = log["table"].get<std::string>();
      if (vertex_tables.find(table_name) != vertex_tables.end()) {
        content = "add_vertex";
      } else if (edge_tables.find(table_name) != edge_tables.end()) {
        is_edge = true;
        content = "add_edge";
      } else {
        continue;
      }

      auto data = log["data"];
      content =
          content + "|" + std::to_string(log_count / FLAGS_logs_per_epoch);
      if (is_edge) {
        content = content + "|" +
                  std::to_string(edge_tables.find(table_name)->second);
      }

      if (is_edge == false) {
        auto vid_col = vertex_label_columns.find(table_name)->second;
        auto vertex_label_id = vertex_tables.find(table_name)->second;
        int64_t fid = vertex_nums[vertex_label_id] % FLAGS_numbers_of_subgraphs;
        int64_t offset = vertex_nums_per_fragment[vertex_label_id][fid];
        vertex_nums[vertex_label_id]++;
        vertex_nums_per_fragment[vertex_label_id][fid]++;
        auto gid = id_parser.GenerateId(fid, vertex_label_id, offset);
        content = content + "|" + std::to_string(gid);
        if (data[vid_col].is_number_integer()) {
          int64_oid2gid_maps[vertex_label_id].emplace(
              data[vid_col].get<int64_t>(), gid);
        } else if (data[vid_col].is_string()) {
          string_oid2gid_maps[vertex_label_id].emplace(
              data[vid_col].get<std::string>(), gid);
        }
      } else {
        auto edge_col = edge_label_columns.find(table_name)->second;
        std::string src_name = edge_col.first;
        std::string dst_name = edge_col.second;
        auto edge_label_id = edge_tables.find(table_name)->second;
        int src_label_id = edge_label2src_dst_labels[edge_label_id].first;
        int dst_label_id = edge_label2src_dst_labels[edge_label_id].second;
        std::string src_vid, dst_vid;
        if (data[src_name].is_number_integer()) {
          src_vid = std::to_string(int64_oid2gid_maps[src_label_id]
                                       .find(data[src_name].get<int64_t>())
                                       ->second);
        } else if (data[src_name].is_string()) {
          src_vid = std::to_string(string_oid2gid_maps[src_label_id]
                                       .find(data[src_name].get<std::string>())
                                       ->second);
        }
        if (data[dst_name].is_number_integer()) {
          dst_vid = std::to_string(int64_oid2gid_maps[dst_label_id]
                                       .find(data[dst_name].get<int64_t>())
                                       ->second);
        } else if (data[dst_name].is_string()) {
          dst_vid = std::to_string(string_oid2gid_maps[dst_label_id]
                                       .find(data[dst_name].get<std::string>())
                                       ->second);
        }
        content = content + "|" + src_vid + "|" + dst_vid;
      }

      auto iter = required_properties.find(table_name);
      auto required_prop_names = iter->second;
      for (size_t prop_id = 0; prop_id < required_prop_names.size();
           prop_id++) {
        auto prop_name = required_prop_names[prop_id];
        auto prop_value = data[prop_name];
        std::string prop_str;
        if (prop_value.is_string()) {
          prop_str = prop_value.get<std::string>();
        } else if (prop_value.is_number_integer()) {
          prop_str = std::to_string(prop_value.get<int>());
        } else if (prop_value.is_number_float()) {
          prop_str = std::to_string(prop_value.get<float>());
        } else {
          continue;
        }
        content = content + "|" + prop_str;
      }
      std::stringstream ss;
      ss << content;
      ostream << ss.str() << std::flush;
    } else if (type == "delete") {
      // TODO(wanglei): add delete vertex and edge support
    } else if (type == "update") {
      // TODO(wanglei): add update vertex and edge support
    } else {
      continue;
    }
    log_count++;
  }
}
