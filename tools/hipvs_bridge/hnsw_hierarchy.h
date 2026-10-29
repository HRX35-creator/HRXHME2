/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <hnswlib/hnswlib.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace seekdb_hipvs {
struct HierarchyStats { uint64_t components; uint64_t upper_nodes; };
template<class T> inline void write(FILE *out, const T &value) {
  if (fwrite(&value, sizeof(value), 1, out) != 1) throw std::runtime_error("HNSW output write failed");
}
inline void write_bytes(FILE *out, const void *data, size_t size) {
  if (fwrite(data, 1, size, out) != size) throw std::runtime_error("HNSW output write failed");
}

// Exact SCCs of the GPU graph. Every component gets an upper-layer entry, so a
// dense but disconnected cluster cannot disappear from the searchable index.
// This traverses existing edges; it does not reconstruct the base graph on CPU.
inline std::vector<uint32_t> component_entries(const std::vector<uint32_t> &graph, uint32_t n, uint32_t g)
{
  std::vector<uint64_t> offsets(static_cast<size_t>(n) + 1, 0);
  for (auto v : graph) {
    if (v >= n) throw std::runtime_error("invalid GPU graph neighbor");
    ++offsets[v + 1];
  }
  for (uint32_t i = 0; i < n; ++i) offsets[i + 1] += offsets[i];
  std::vector<uint64_t> cursor(offsets);
  std::vector<uint32_t> reverse(graph.size());
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = 0; j < g; ++j) reverse[cursor[graph[static_cast<size_t>(i)*g+j]]++] = i;
  cursor.clear(); cursor.shrink_to_fit();
  std::vector<uint8_t> visited(n, 0);
  std::vector<uint32_t> order; order.reserve(n);
  std::vector<std::pair<uint32_t,uint32_t>> stack;
  for (uint32_t start = 0; start < n; ++start) {
    if (visited[start]) continue;
    visited[start] = 1; stack.emplace_back(start,0);
    while (!stack.empty()) {
      auto node = stack.back().first;
      if (stack.back().second == g) { order.push_back(node); stack.pop_back(); continue; }
      auto edge = stack.back().second++;
      auto next = graph[static_cast<size_t>(node)*g+edge];
      if (!visited[next]) { visited[next]=1; stack.emplace_back(next,0); }
    }
  }
  std::fill(visited.begin(), visited.end(), 0);
  std::vector<uint32_t> entries, todo;
  for (auto it=order.rbegin();it!=order.rend();++it) {
    if (visited[*it]) continue;
    entries.push_back(*it); visited[*it]=1; todo.push_back(*it);
    while (!todo.empty()) {
      const auto node=todo.back();todo.pop_back();
      for(uint64_t p=offsets[node];p<offsets[node+1];++p) {
        const auto next=reverse[p];
        if(!visited[next]) {visited[next]=1;todo.push_back(next);}
      }
    }
  }
  return entries;
}

inline HierarchyStats serialize_hierarchy(FILE *out, std::vector<uint32_t> &graph,
    const std::vector<float> &vectors, uint32_t n, uint32_t dim, uint32_t m, uint32_t ef)
{
  const uint32_t g=m*2;
  auto entries=component_entries(graph,n,g);
  const uint64_t components=entries.size();
  std::vector<bool> selected(n,false);
  for(auto v:entries) selected[v]=true;
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint32_t> sample(0,m-1);
  for(uint32_t i=0;i<n;++i) if(sample(rng)==0) selected[i]=true;
  entries.clear();
  for(uint32_t i=0;i<n;++i) if(selected[i]) entries.push_back(i);
  hnswlib::L2Space<float> space(dim);
  // Its level zero becomes our level one. Keep the requested M at every upper
  // layer while limiting this auxiliary graph's base connections to M as well.
  hnswlib::HierarchicalNSW<float> upper(&space,entries.size(),m,ef,42);
  upper.maxM0_=m;
  std::vector<uint32_t> position(n,std::numeric_limits<uint32_t>::max());
  for(uint32_t j=0;j<entries.size();++j) {
    const uint32_t row=entries[j];
    upper.addPoint(vectors.data()+static_cast<size_t>(row)*dim,row);
    position[row]=j;
  }
  const uint64_t offset0=0,capacity=n,count=n,data_offset=4+uint64_t(g)*4;
  const uint64_t label_offset=data_offset+uint64_t(dim)*4,stride=label_offset+8;
  const int32_t maxlevel=upper.maxlevel_+1;
  const uint32_t entry=upper.getExternalLabel(upper.enterpoint_node_);
  std::vector<uint32_t> counts(n,g);
  std::vector<uint32_t> links;
  std::vector<std::pair<double,uint32_t>> nearest;
  // Pure kNN/CAGRA clusters can be disconnected. Promote the sparse routing
  // graph's links into the base graph at selected entries. Retain at least M
  // original local neighbors and never exceed the common HNSW base budget 2M.
  for(uint32_t row:entries) {
    auto *aux=upper.get_linklist0(position[row]);
    const uint32_t aux_count=upper.getListCount(aux);
    const auto *aux_ids=reinterpret_cast<const uint32_t*>(aux)+1;
    links.clear();
    for(uint32_t j=0;j<aux_count;++j) {
      const uint32_t v=upper.getExternalLabel(aux_ids[j]);
      if(v!=row && std::find(links.begin(),links.end(),v)==links.end())links.push_back(v);
    }
    if(links.size()>m)throw std::runtime_error("routing degree exceeds M");
    nearest.clear();
    for(uint32_t j=0;j<g;++j) {
      const uint32_t v=graph[static_cast<size_t>(row)*g+j];
      if(v==row || std::find(links.begin(),links.end(),v)!=links.end())continue;
      double d=0;
      for(uint32_t c=0;c<dim;++c) {
        const double diff=double(vectors[static_cast<size_t>(row)*dim+c])-vectors[static_cast<size_t>(v)*dim+c];
        d+=diff*diff;
      }
      nearest.emplace_back(d,v);
    }
    std::sort(nearest.begin(),nearest.end());
    for(auto v:nearest) {
      if(links.size()==g)break;
      if(std::find(links.begin(),links.end(),v.second)==links.end())links.push_back(v.second);
    }
    counts[row]=links.size();
    std::copy(links.begin(),links.end(),graph.begin()+static_cast<size_t>(row)*g);
  }
  // An index may be approximate, but no component may be unreachable from its
  // actual entry point. Fail explicitly instead of publishing an incomplete graph.
  std::vector<bool> reachable(n,false);std::vector<uint32_t> pending{entry};reachable[entry]=true;
  uint64_t reached=0;
  while(!pending.empty()) {
    auto row=pending.back();pending.pop_back();++reached;
    for(uint32_t j=0;j<counts[row];++j) {
      auto v=graph[static_cast<size_t>(row)*g+j];
      if(!reachable[v]){reachable[v]=true;pending.push_back(v);}
    }
  }
  if(reached!=n)throw std::runtime_error("routing graph does not reach every vector");
  const uint64_t maxm=m,maxm0=g,degree=m,efc=std::max(m,ef);
  const double mult=1.0/std::log(double(m));
  write(out,offset0);write(out,capacity);write(out,count);write(out,stride);write(out,label_offset);write(out,data_offset);
  write(out,maxlevel);write(out,entry);write(out,maxm);write(out,maxm0);write(out,degree);write(out,mult);write(out,efc);
  for(uint32_t row=0;row<n;++row) {
    write(out,counts[row]);
    write_bytes(out,graph.data()+static_cast<size_t>(row)*g,g*sizeof(uint32_t));
    write_bytes(out,vectors.data()+static_cast<size_t>(row)*dim,dim*sizeof(float));
    const uint64_t label=row;write(out,label);
  }
  std::vector<uint32_t> neighbors(m,0);
  for(uint32_t row=0;row<n;++row) {
    if(!selected[row]) {const uint32_t zero=0;write(out,zero);continue;}
    const uint32_t node=position[row],levels=upper.element_levels_[node]+1;
    const uint32_t length=levels*(4+4*m);write(out,length);
    for(uint32_t level=0;level<levels;++level) {
      auto* links=level==0?upper.get_linklist0(node):upper.get_linklist(node,level);
      const uint32_t degree=upper.getListCount(links);
      if(degree>m) throw std::runtime_error("upper-layer degree exceeds HNSW M");
      write(out,degree);
      std::fill(neighbors.begin(),neighbors.end(),0);
      const auto* source=reinterpret_cast<const uint32_t*>(links)+1;
      for(uint32_t j=0;j<degree;++j) neighbors[j]=upper.getExternalLabel(source[j]);
      write_bytes(out,neighbors.data(),m*sizeof(uint32_t));
    }
  }
  if(fflush(out)!=0)throw std::runtime_error("flush HNSW output failed");
  return {components,entries.size()};
}
}
