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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "glog/logging.h"

#include <etcd/Client.hpp>
#include <etcd/Response.hpp>
#include "grape/grape.h"
#include "grape/util.h"
#include "vineyard/client/client.h"
#include "vineyard/common/util/json.h"
#include "vineyard/graph/fragment/arrow_fragment.h"

#include "core/context/i_context.h"
#include "core/context/labeled_vertex_property_context.h"
#include "core/loader/arrow_fragment_loader.h"
#include "core/utils/transform_utils.h"
#include "flags.h"
#include "interfaces/fragment/gart_fragment.h"

using GraphType = gart::GartFragment<uint64_t, uint64_t>;
using json = vineyard::json;

uint64_t get_latest_epoch(const grape::CommSpec& comm_spec,
                          std::shared_ptr<etcd::Client> etcd_client) {
  uint64_t write_epoch = std::numeric_limits<uint64_t>::max();

  if (comm_spec.fid() == 0) {
    for (uint idx = 0; idx < comm_spec.fnum(); idx++) {
      std::string latest_epoch_str =
          FLAGS_meta_prefix + "gart_latest_epoch_p" + std::to_string(idx);
      etcd::Response response = etcd_client->get(latest_epoch_str).get();
      assert(response.is_ok());
      uint64_t latest_epoch = std::stoull(response.value().as_string());
      if (latest_epoch < write_epoch) {
        write_epoch = latest_epoch;
      }
    }
    for (uint idx = 1; idx < comm_spec.fnum(); idx++) {
      MPI_Send(&write_epoch, 1, MPI_UNSIGNED_LONG, idx, 0, MPI_COMM_WORLD);
    }
  } else {
    MPI_Recv(&write_epoch, 1, MPI_UNSIGNED_LONG, 0, 0, MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);
  }

  MPI_Barrier(comm_spec.comm());
  return write_epoch;
}

int main(int argc, char** argv) {
  grape::InitMPIComm();
  {
    grape::CommSpec comm_spec;
    comm_spec.Init(MPI_COMM_WORLD);

    // vineyard::ObjectID fragment_id;
    std::shared_ptr<GraphType> fragment(
        new gart::GartFragment<uint64_t, uint64_t>());
    grape::gflags::ParseCommandLineFlags(&argc, &argv, true);
    std::shared_ptr<etcd::Client> etcd_client =
        std::make_shared<etcd::Client>(FLAGS_etcd_endpoint);
    std::string schema_key =
        FLAGS_meta_prefix + "gart_schema_p" + std::to_string(comm_spec.fid());
    etcd::Response response = etcd_client->get(schema_key).get();
    assert(response.is_ok());
    std::string edge_config_str = response.value().as_string();
    uint64_t write_epoch = get_latest_epoch(comm_spec, etcd_client);
    schema_key = FLAGS_meta_prefix + "gart_blob_m" + std::to_string(0) + "_p" +
                 std::to_string(comm_spec.fid()) + "_e" +
                 std::to_string(write_epoch);
    response = etcd_client->get(schema_key).get();
    assert(response.is_ok());
    std::string config_str = response.value().as_string();
    json config = json::parse(config_str);
    json edge_config = json::parse(edge_config_str);

    fragment->Init(config, edge_config);

    MPI_Barrier(comm_spec.comm());

    auto vertex_label_num = fragment->vertex_label_num();
    auto edge_label_num = fragment->edge_label_num();

    for (auto v_label = 0; v_label < vertex_label_num; v_label++) {
      auto inner_vertices_iter = fragment->InnerVertices(v_label);
      while (inner_vertices_iter.valid()) {
        auto src = inner_vertices_iter.vertex();
        std::cout << "fid = " << fragment->fid()
                  << " src label = " << fragment->vertex_label(src)
                  << " src offset = " << fragment->GetOffset(src)
                  << " src data = "
                  << fragment->template GetData<int64_t>(src, 0) << std::endl;
        for (auto elabel = 0; elabel < edge_label_num; elabel++) {
          auto edge_iter = fragment->GetOutgoingAdjList(src, elabel);
          while (edge_iter.valid()) {
            auto dst = edge_iter.neighbor();
            std::cout << "fid = " << fragment->fid()
                      << " src label = " << fragment->vertex_label(src)
                      << " src offset = " << fragment->GetOffset(src)
                      << " dst label = " << fragment->vertex_label(dst)
                      << " dst offset = " << fragment->GetOffset(dst)
                      << std::endl;
            edge_iter.next();
          }
        }
        inner_vertices_iter.next();
      }
    }

    MPI_Barrier(comm_spec.comm());
  }

  grape::FinalizeMPIComm();

  return 0;
}
