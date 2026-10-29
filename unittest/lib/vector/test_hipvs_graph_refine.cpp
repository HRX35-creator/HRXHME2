/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_graph_refine.h"
#include "hipvs_graph_topology.h"
#include <iostream>
#include <stdexcept>
using oceanbase::common::hipvs::GraphRefine;
using Bytes=std::vector<unsigned char>;
template<class T> void put(Bytes &b,size_t p,T v) {std::memcpy(b.data()+p,&v,sizeof(v));}
template<class T> T get(const Bytes &b,size_t p) {T v;std::memcpy(&v,b.data()+p,sizeof(v));return v;}
void check(bool ok) {if(!ok) throw std::runtime_error("refinement invariant failed");}
int main() try {
  for(uint32_t m:{2u,16u,128u}) for(uint32_t dim:{1u,7u,128u}) for(bool disconnected:{false,true}) {
    uint32_t degree=2*m, component=degree+3,n=component*(disconnected?2:1);
    const size_t vec=4+degree*4,label=vec+dim*4,stride=label+8;
    Bytes raw(96+n*(stride+4),0); std::vector<float> values(n*dim);
    put<uint64_t>(raw,16,n);put<uint64_t>(raw,24,stride);put<uint64_t>(raw,32,label);
    put<uint64_t>(raw,40,vec);put<uint32_t>(raw,52,0);
    for(uint32_t u=0;u<n;++u) {
      const auto p=96+u*stride; put<uint32_t>(raw,p,degree);
      for(uint32_t j=0;j<degree;++j) put<uint32_t>(raw,p+4+4*j,(u/component)*component+(u+j+1)%component);
      for(uint32_t d=0;d<dim;++d) values[u*dim+d]=std::sin(float(u*dim+d));
      std::memcpy(raw.data()+p+vec,values.data()+u*dim,dim*4);
      put<int64_t>(raw,p+label,-(int64_t(1)<<55)-u);
    }
    auto before=raw;
    auto shape=[&](const Bytes &b) {std::vector<std::vector<uint32_t>> edges(n);
      for(uint32_t u=0;u<n;++u) for(uint32_t j=0;j<degree;++j) edges[u].push_back(get<uint32_t>(b,96+u*stride+4+4*j));
      return hipvs_diagnostic::analyze_graph(edges,0);};
    check(GraphRefine::refine(raw,values,dim,m)==nullptr);
    check(shape(raw).reachable_from_entry>=shape(before).reachable_from_entry);
    auto repeat=before;check(GraphRefine::refine(repeat,values,dim,m)==nullptr);check(repeat==raw);
    check(std::equal(raw.begin(),raw.begin()+96,before.begin()));
    check(std::equal(raw.begin()+96+n*stride,raw.end(),before.begin()+96+n*stride));
    for(uint32_t u=0;u<n;++u) {
      auto p=96+u*stride;
      check(get<uint32_t>(raw,p)==degree);
      check(std::equal(raw.begin()+p+vec,raw.begin()+p+stride,before.begin()+p+vec));
      std::vector<uint32_t> neighbors;
      for(uint32_t j=0;j<degree;++j) {auto v=get<uint32_t>(raw,p+4+4*j);check(v<n&&v!=u);neighbors.push_back(v);}
      std::sort(neighbors.begin(),neighbors.end());check(std::adjacent_find(neighbors.begin(),neighbors.end())==neighbors.end());
    }
    auto reject=[&](Bytes b,std::vector<float> v,uint32_t d,uint32_t mm) {auto saved=b;check(GraphRefine::refine(b,v,d,mm)!=nullptr);check(b==saved);};
    reject(Bytes(before.begin(),before.begin()+80),values,dim,m);
    auto bad=before;bad.pop_back();reject(bad,values,dim,m);
    bad=before;put<uint32_t>(bad,96+(n-1)*stride+4,n);reject(bad,values,dim,m);
    bad=before;put<uint32_t>(bad,96+8,get<uint32_t>(bad,96+4));reject(bad,values,dim,m);
    bad=before;put<uint32_t>(bad,52,n);reject(bad,values,dim,m);
    reject(before,values,0,m);reject(before,values,dim,0);
    auto nonfinite=values;nonfinite.back()=std::numeric_limits<float>::infinity();reject(before,nonfinite,dim,m);
  }
  std::cout<<"PASS: 18 shapes; reachable coverage, stable IDs/vectors, unique edges, deterministic output and atomic rejection\n";
} catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
