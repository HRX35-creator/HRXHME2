/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// GPU integration executable; run explicitly on the competition image.
#include "lib/vector/hipvs_bulk_builder.h"
#include <vsag/vsag.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifndef HIPVS_STANDALONE_MAIN
#include <gtest/gtest.h>
#endif

namespace hipvs_bulk_test {
using oceanbase::common::obvsag::HipvsBulkBuilder;
using Clock = std::chrono::steady_clock;
static void require(bool value, const char *what) { if (!value) throw std::runtime_error(what); }
static double seconds(Clock::time_point begin) { return std::chrono::duration<double>(Clock::now()-begin).count(); }
template<class T> std::vector<T> read(const std::string& file, size_t count) {
  std::ifstream in(file,std::ios::binary); std::vector<T> data(count);
  in.read(reinterpret_cast<char*>(data.data()),count*sizeof(T));
  require(in.gcount()==static_cast<std::streamsize>(count*sizeof(T)),"short fixture");return data;
}
struct EvenFilter: vsag::Filter {
  bool CheckValid(int64_t id) const override { return (id-8589934593LL)/17%2==0; }
  float ValidRatio() const override { return 0.5; }
};

int run_integration(int argc,const char** argv) {
 try {
  require(argc==3||argc==4,"usage: test_hipvs_bulk_import /absolute/worker /dataset/directory [M]");
  const int n=10000,dim=128,k=10,nq=100;
  const int degree=argc==4?std::stoi(argv[3]):16;
  auto base=read<float>(std::string(argv[2])+"/base.f32",n*dim);
  auto queries=read<float>(std::string(argv[2])+"/query.f32",nq*dim);
  auto truth=read<int32_t>(std::string(argv[2])+"/gt_100x10.i32",nq*k);
  std::vector<int64_t> ids(n);for(int i=0;i<n;++i)ids[i]=8589934593LL+i*17;
  std::rotate(ids.begin(),ids.begin()+n/3,ids.end());
  const int64_t minimum_id=*std::min_element(ids.begin(),ids.end());
  const int64_t maximum_id=*std::max_element(ids.begin(),ids.end());
  const std::string parameters=std::string(R"({"dtype":"float32","metric_type":"l2","dim":128,"use_old_serial_format":true,"hnsw":{"max_degree":)")+std::to_string(degree)+R"(,"ef_construction":200}})";
  auto create=[&] {auto r=vsag::Factory::CreateIndex("hnsw",parameters);require(r.has_value(),"create native HNSW");return r.value();};
  auto input=vsag::Dataset::Make();input->NumElements(n)->Dim(dim)->Ids(ids.data())->Float32Vectors(base.data())->Owner(false);
  auto cpu=create();auto start=Clock::now();auto built=cpu->Build(input);require(built.has_value()&&built.value().empty(),"CPU build");
  double cpu_seconds=seconds(start);
  auto gpu=create();start=Clock::now();
  HipvsBulkBuilder builder(argv[1],dim,degree,200);
  for(int i=0;i<n;i+=100)builder.append(base.data()+i*dim,ids.data()+i,dim,std::min(100,n-i));
  auto graph=builder.build_native_graph();std::istringstream stream(std::move(graph),std::ios::binary);
  auto loaded=gpu->Deserialize(stream);require(loaded.has_value(),"GPU graph native import");
  require(gpu->GetNumElements()==n,"GPU imported vector count");builder.release_staging();
  auto bounds=gpu->GetMinAndMaxId();
  require(bounds.has_value()&&bounds.value().first==minimum_id&&bounds.value().second==maximum_id,"imported native ID bounds differ");
  double gpu_seconds=seconds(start);
  int cpu_hits=0,gpu_hits=0;
  auto score=[&](const std::shared_ptr<vsag::Index>& index,int qid) {
    auto query=vsag::Dataset::Make();query->NumElements(1)->Dim(dim)->Float32Vectors(queries.data()+qid*dim)->Owner(false);
    auto r=index->KnnSearch(query,k,R"({"hnsw":{"ef_search":128}})");require(r.has_value(),"native KNN failed");
    require(r.value()->GetDim()==k,"short KNN output");std::set<int64_t> seen;int hits=0;
    for(int j=0;j<k;++j) {
      auto id=r.value()->GetIds()[j];require(id>=minimum_id&&id<=maximum_id&&(id-minimum_id)%17==0,"invalid external ID");
      require(seen.insert(id).second,"duplicate query result");require(std::isfinite(r.value()->GetDistances()[j]),"nonfinite distance");
      for(int t=0;t<k;++t)if(id==ids[truth[qid*k+t]])++hits;
    }
    return hits;
  };
  for(int i=0;i<nq;++i){cpu_hits+=score(cpu,i);gpu_hits+=score(gpu,i);}
  require(gpu->SetImmutable().has_value(),"immutable snapshot optimization failed");
  require(score(gpu,0)>0,"immutable snapshot lost query results");
  auto iter_query=vsag::Dataset::Make();iter_query->NumElements(1)->Dim(dim)->Float32Vectors(queries.data())->Owner(false);
  std::string iter_parameters=R"({"hnsw":{"ef_search":512}})";
  vsag::SearchParam iter_params(true,iter_parameters,nullptr,nullptr);
  auto iter_result=gpu->KnnSearch(iter_query,k,iter_params);
  require(iter_result.has_value()&&iter_result.value()->GetDim()==k,"iterative native search returned no results");
  if(iter_params.iter_ctx)delete iter_params.iter_ctx;
  std::ostringstream serialized(std::ios::binary);require(gpu->Serialize(serialized).has_value(),"native serialize failed");
  auto restored=create();std::istringstream persisted(serialized.str(),std::ios::binary);
  require(restored->Deserialize(persisted).has_value(),"native reload failed");
  require(restored->GetNumElements()==n,"restored size");
  int restored_hits=0;for(int i=0;i<nq;++i)restored_hits+=score(restored,i);
  require(restored_hits==gpu_hits,"reload changed recall");
  auto query=vsag::Dataset::Make();query->NumElements(1)->Dim(dim)->Float32Vectors(queries.data())->Owner(false);
  auto filtered=restored->KnnSearch(query,k,R"({"hnsw":{"ef_search":256}})",std::make_shared<EvenFilter>());
  require(filtered.has_value()&&filtered.value()->GetDim()==k,"native filtered search failed");
  EvenFilter filter;for(int i=0;i<k;++i)require(filter.CheckValid(filtered.value()->GetIds()[i]),"filter leaked excluded ID");
  int64_t new_id=maximum_id+1234;std::vector<float> extra(dim,1000.0f);
  auto addition=vsag::Dataset::Make();addition->NumElements(1)->Dim(dim)->Ids(&new_id)->Float32Vectors(extra.data())->Owner(false);
  require(restored->Add(addition).has_value(),"incremental native add failed");
  require(restored->GetNumElements()==n+1,"incremental size");
  auto self=vsag::Dataset::Make();self->NumElements(1)->Dim(dim)->Float32Vectors(extra.data())->Owner(false);
  auto own=restored->KnnSearch(self,1,R"({"hnsw":{"ef_search":256}})");
  require(own.has_value()&&own.value()->GetIds()[0]==new_id,"new point not searchable");
  HipvsBulkBuilder failed("/bin/false",dim,16,200);failed.append(base.data(),ids.data(),dim,1000);
  bool rejected=false;try{failed.build_native_graph();}catch(const std::exception&){rejected=true;}
  require(rejected,"worker failure must not fall back silently");
  HipvsBulkBuilder bad("/bin/true",dim,16,200);bad.append(base.data(),ids.data(),dim,1000);
  rejected=false;try{bad.build_native_graph();}catch(const std::exception&){rejected=true;}
  require(rejected,"empty worker artifact must be rejected");
  std::cout<<"{\"test\":\"component_GPU_to_native_HNSW\",\"n\":"<<n<<",\"dim\":"<<dim
    <<",\"cpu_build_seconds\":"<<cpu_seconds<<",\"gpu_build_import_seconds\":"<<gpu_seconds
    <<",\"cpu_recall\":"<<double(cpu_hits)/(nq*k)<<",\"gpu_recall\":"<<double(gpu_hits)/(nq*k)
    <<",\"roundtrip_filter_incremental_and_failure_checks\":true,\"SQL_end_to_end_test\":false}"<<std::endl;
  return 0;
 } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
} // namespace hipvs_bulk_test

#ifdef HIPVS_STANDALONE_MAIN
int main(int argc, char **argv) { return hipvs_bulk_test::run_integration(argc,const_cast<const char **>(argv)); }
#else
TEST(HipvsBulkBuilder, rejects_invalid_input_without_staging)
{
  using oceanbase::common::obvsag::HipvsBulkBuilder;
  HipvsBulkBuilder builder("/missing/worker", 2, 16, 200);
  const float data[] = {1, 2, 3, 4};
  const int64_t duplicate[] = {10, 10};
  EXPECT_THROW(builder.append(data, duplicate, 3, 1), std::invalid_argument);
  EXPECT_EQ(0, builder.size());
  EXPECT_THROW(builder.append(data, duplicate, 2, 2), std::invalid_argument);
  EXPECT_EQ(0, builder.size());
}

TEST(HipvsBulkBuilder, accumulates_individual_rows_and_preserves_labels)
{
  using oceanbase::common::obvsag::HipvsBulkBuilder;
  HipvsBulkBuilder builder("/missing/worker", 2, 16, 200);
  const float vector[] = {1, 2};
  for (int64_t i = 0; i < 10000; ++i) {
    const int64_t id = 8589934593LL + 17 * i;
    builder.append(vector, &id, 2, 1);
  }
  ASSERT_EQ(10000, builder.size());
  EXPECT_EQ(8589934593LL + 17 * 9999, builder.ids()[9999]);
  EXPECT_EQ(2, builder.vectors()[19999]);
  EXPECT_THROW(builder.build_native_graph(), std::runtime_error);
}

TEST(HipvsBulkImport, DISABLED_gpu_native_roundtrip)
{
  const char *worker = std::getenv("SEEKDB_HIPVS_TEST_WORKER");
  const char *dataset = std::getenv("SEEKDB_HIPVS_TEST_DATASET");
  ASSERT_NE(nullptr, worker) << "Set SEEKDB_HIPVS_TEST_WORKER for this explicit GPU test";
  ASSERT_NE(nullptr, dataset) << "Set SEEKDB_HIPVS_TEST_DATASET for this explicit GPU test";
  const char *args[] = {"gpu_native_roundtrip", worker, dataset};
  EXPECT_EQ(0, hipvs_bulk_test::run_integration(3, args));
}
#endif
