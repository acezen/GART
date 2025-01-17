/** Copyright 2020-2023 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef ANALYTICAL_ENGINE_APPS_GART_PROPERTY_SSSP_H_
#define ANALYTICAL_ENGINE_APPS_GART_PROPERTY_SSSP_H_

#include <grape/utils/atomic_ops.h>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "grape/grape.h"

#include "core/app/app_base.h"
#include "core/context/gart_vertex_data_context.h"
#include "core/parallel/gart_parallel.h"
#include "core/utils/gart_vertex_array.h"

namespace gs {

/**
 * A sssp implementation for labeled graph
 * @tparam FRAG_T
 */
template <typename FRAG_T>
class PropertySSSPContext : public gs::GartLabeledVertexDataContext<FRAG_T> {
  using vid_t = typename FRAG_T::vid_t;
  using oid_t = typename FRAG_T::oid_t;

 public:
  explicit PropertySSSPContext(const FRAG_T& fragment)
      : gs::GartLabeledVertexDataContext<FRAG_T>(fragment) {}

  void Init(grape::DefaultMessageManager& messages, oid_t src_oid) {
    auto& frag = this->fragment();
    auto vertex_label_num = frag.vertex_label_num();
    source_id = src_oid;
    result.resize(vertex_label_num);
    updated.resize(vertex_label_num);
    updated_next.resize(vertex_label_num);

    for (auto v_label = 0; v_label < vertex_label_num; v_label++) {
      auto vertices_iter = frag.Vertices(v_label);
      updated[v_label].Init(&frag, vertices_iter, false);
      updated_next[v_label].Init(&frag, vertices_iter, false);
      result[v_label].Init(&frag, vertices_iter,
                           std::numeric_limits<int>::max());
    }
  }

  void Output(std::ostream& os) override {
    auto& frag = this->fragment();
    auto v_label_num = frag.vertex_label_num();
    std::ofstream out("output_frag_sssp_" + std::to_string(frag.fid()) +
                      ".txt");
    for (auto v_label = 0; v_label < v_label_num; v_label++) {
      auto vertices_iter = frag.InnerVertices(v_label);
      while (vertices_iter.valid()) {
        auto v = vertices_iter.vertex();
        auto v_data = result[v_label][v];
        out << frag.GetId(v) << " " << v_data << std::endl;
        vertices_iter.next();
      }
    }
  }

  std::vector<gart::GartVertexArray<gart::vid_t, int>> result;
  std::vector<gart::GartVertexArray<gart::vid_t, bool>> updated;
  std::vector<gart::GartVertexArray<gart::vid_t, bool>> updated_next;
  oid_t source_id;
};

template <typename FRAG_T>
class PropertySSSP : public AppBase<FRAG_T, PropertySSSPContext<FRAG_T>> {
 public:
  INSTALL_DEFAULT_WORKER(PropertySSSP<FRAG_T>, PropertySSSPContext<FRAG_T>,
                         FRAG_T)

  using vertex_t = typename fragment_t::vertex_t;

  static constexpr bool need_split_edges = false;
  static constexpr grape::MessageStrategy message_strategy =
      grape::MessageStrategy::kSyncOnOuterVertex;
  static constexpr grape::LoadStrategy load_strategy =
      grape::LoadStrategy::kBothOutIn;

  void PEval(const fragment_t& frag, context_t& ctx,
             message_manager_t& messages) {
    auto e_label_num = frag.edge_label_num();
    bool is_native = false;
    vertex_t src_vertex;
    is_native = frag.InnerVertexGid2Vertex(ctx.source_id, src_vertex);
    if (is_native) {
      auto src_label = frag.vertex_label(src_vertex);
      ctx.result[src_label][src_vertex] = 0;
      for (auto e_label = 0; e_label < e_label_num; e_label++) {
        auto edge_iter = frag.GetOutgoingAdjList(src_vertex, e_label);
        while (edge_iter.valid()) {
          auto dst_vertex = edge_iter.neighbor();
          auto dst_label = frag.vertex_label(dst_vertex);
          int e_data = edge_iter.template get_data<int>(0);
          ctx.result[dst_label][dst_vertex] =
              std::min(ctx.result[dst_label][dst_vertex], e_data);
          if (frag.IsInnerVertex(dst_vertex)) {
            ctx.updated[dst_label][dst_vertex] = true;
          } else {
            messages.SyncStateOnOuterVertex(frag, dst_vertex,
                                            ctx.result[dst_label][dst_vertex]);
          }
          edge_iter.next();
        }
      }
    }

    messages.ForceContinue();
  }

  void IncEval(const fragment_t& frag, context_t& ctx,
               message_manager_t& messages) {
    auto v_label_num = frag.vertex_label_num();
    auto e_label_num = frag.edge_label_num();
    int val;
    vertex_t v;
    while (messages.GetMessage<fragment_t, int>(frag, v, val)) {
      auto v_label = frag.vertex_label(v);
      if (ctx.result[v_label][v] > val) {
        ctx.result[v_label][v] = val;
        ctx.updated[v_label][v] = true;
      }
    }

    for (auto v_label = 0; v_label < v_label_num; v_label++) {
      ctx.updated[v_label].Swap(ctx.updated_next[v_label]);
      ctx.updated[v_label].SetValue(false);
    }

    for (auto v_label = 0; v_label < v_label_num; v_label++) {
      auto inner_vertices_iter = frag.InnerVertices(v_label);
      while (inner_vertices_iter.valid()) {
        auto src = inner_vertices_iter.vertex();
        if (ctx.updated_next[v_label][src] == false) {
          inner_vertices_iter.next();
          continue;
        }
        int dist_src = ctx.result[v_label][src];
        for (auto e_label = 0; e_label < e_label_num; e_label++) {
          auto edge_iter = frag.GetOutgoingAdjList(src, e_label);
          while (edge_iter.valid()) {
            auto dst = edge_iter.neighbor();
            auto dst_label = frag.vertex_label(dst);
            int e_data = edge_iter.template get_data<int>(0);
            int new_dist_dst = dist_src + e_data;
            if (new_dist_dst < ctx.result[dst_label][dst]) {
              ctx.result[dst_label][dst] = new_dist_dst;
              if (frag.IsInnerVertex(dst)) {
                ctx.updated[dst_label][dst] = true;
              } else {
                messages.SyncStateOnOuterVertex(frag, dst, new_dist_dst);
              }
            }
            edge_iter.next();
          }
        }
        inner_vertices_iter.next();
      }
    }
  }
};

}  // namespace gs

#endif  // ANALYTICAL_ENGINE_APPS_GART_PROPERTY_SSSP_H_
