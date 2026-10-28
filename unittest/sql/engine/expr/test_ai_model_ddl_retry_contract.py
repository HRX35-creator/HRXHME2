"""Host contract for the actual AI model DDL service source.

SQL transaction seams are injected; this is not a seekdb server integration test.
The source override permits the exact pre-change regression control.
"""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "src/rootserver/ob_ai_model_ddl_service.cpp"
PREFIX = r'''
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
constexpr int OB_SUCCESS=0, OB_EAGAIN=-4023, OB_TIMEOUT=-4012;
constexpr int OB_CANCELED=-4067, OB_AI_FUNC_MODEL_EXISTS=-11117;
constexpr int OB_AI_FUNC_MODEL_NOT_FOUND=-11118;
#define OB_FAIL(x) ((ret=(x))!=OB_SUCCESS)
#define OB_SUCC(x) ((x)==OB_SUCCESS)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_UNLIKELY(x) (x)
#define LOG_ERROR(...) do {} while(false)
#define LOG_USER_ERROR(...) do {} while(false)
struct State {
  bool exists=false, startup_without_transaction=false, change_precondition=false;
  int starts=0, refreshes=0, writes=0, commits=0, rollbacks=0, publishes=0;
  int retry_count=0, start_error=0, rollback_error=0, commit_error=0;
  int operation_error=0, publish_error=0, status=0, status_checks=0;
  int64_t remaining=10000000;
  std::vector<int64_t> versions;
} state;
struct Worker {
  int64_t get_timeout_remain() const { return state.remaining; }
  int check_status() const { ++state.status_checks; return state.status; }
} worker;
#define THIS_WORKER worker
namespace oceanbase {
using ObString=std::string;
struct ModelInfo { ObString name="model"; const ObString &get_name() const {return name;} };
struct ObAiModelSchema { int assign(const ModelInfo&) {return 0;} };
ObAiModelSchema model_schema;
struct ObSchemaGetterGuard {
  int64_t version=0;
  int get_ai_model_schema(const ObString&,const ObAiModelSchema *&model) {
    model=state.exists?&model_schema:nullptr;return 0;
  }
  int get_schema_version(int64_t &value) {value=version;return 0;}
};
namespace obcall {
struct ObCreateAiModelArg { ModelInfo model_info_; ObString ddl_stmt_str_; };
struct ObDropAiModelArg {
  ObString name="model",ddl_stmt_str_;
  const ObString &get_ai_model_name() const {return name;}
};
}
namespace rootserver {
struct ObDDLService {
  int service=0,proxy=0;
  int get_runtime_schema_guard_with_version_in_inner_table(ObSchemaGetterGuard &guard) {
    guard.version=++state.refreshes;return 0;
  }
  int &get_schema_service() {return service;}
  int &get_sql_proxy() {return proxy;}
  int publish_schema() {++state.publishes;return state.publish_error;}
};
struct ObDDLSQLTransaction {
  bool started=false;
  explicit ObDDLSQLTransaction(int*) {}
  int start(int*,int64_t version) {
    ++state.starts;state.versions.push_back(version);
    started=!state.startup_without_transaction;
    if(state.retry_count>0) {
      --state.retry_count;
      if(state.change_precondition) state.exists=!state.exists;
      return OB_EAGAIN;
    }
    started=true;return state.start_error;
  }
  bool is_started() const {return started;}
  int end(bool commit) {
    started=false;
    if(commit) {++state.commits;return state.commit_error;}
    ++state.rollbacks;return state.rollback_error;
  }
};
struct ObAiModelDDLOperator {
  explicit ObAiModelDDLOperator(int&) {}
  int create_ai_model(ObAiModelSchema&,const ObString&,ObDDLSQLTransaction&) {
    ++state.writes;return state.operation_error;
  }
  int drop_ai_model(const ObAiModelSchema&,const ObString&,ObDDLSQLTransaction&) {
    ++state.writes;return state.operation_error;
  }
};
struct ObAiModelDDLService {
  ObDDLService &ddl_service_;
  explicit ObAiModelDDLService(ObDDLService &service):ddl_service_(service) {}
  int create_ai_model(const obcall::ObCreateAiModelArg&);
  int drop_ai_model(const obcall::ObDropAiModelArg&);
};
}
}
'''
SUFFIX = r'''
void check(bool value,const char *message) {if(!value) throw std::runtime_error(message);}
int main(int argc,char **argv) {
  if(argc!=3) return 3;
  const bool dropping=std::string(argv[1])=="drop";
  const std::string mode=argv[2];state.exists=dropping;
  if(mode=="retry" || mode=="timeout" || mode=="cancel" || mode=="rollback_error" || mode=="no_transaction" || mode=="changed") state.retry_count=1;
  if(mode=="timeout") state.remaining=0;
  if(mode=="cancel") state.status=OB_CANCELED;
  if(mode=="rollback_error") state.rollback_error=-9001;
  if(mode=="no_transaction") state.startup_without_transaction=true;
  if(mode=="changed") state.change_precondition=true;
  if(mode=="start_error") state.start_error=-9002;
  if(mode=="operator_error") state.operation_error=OB_EAGAIN;
  if(mode=="publish_error") state.publish_error=OB_EAGAIN;
  if(mode=="commit_error") state.commit_error=-9003;
  oceanbase::rootserver::ObDDLService ddl;
  oceanbase::rootserver::ObAiModelDDLService service(ddl);
  const int ret=dropping?service.drop_ai_model({}):service.create_ai_model({});
  try {
    if(mode=="success" || mode=="retry" || mode=="no_transaction") {
      const bool retry=mode!="success";
      check(ret==0 && state.starts==(retry?2:1) && state.refreshes==state.starts,"start conflict was not retried with a fresh schema");
      check(state.writes==1 && state.commits==1 && state.publishes==1,"mutation/commit/publish repeated or skipped");
      check(state.rollbacks==((retry && mode!="no_transaction")?1:0),"retry transaction was not rolled back exactly once");
      if(retry) check(state.versions==std::vector<int64_t>{1,2},"retry reused the old schema version");
    } else if(mode=="changed") {
      check(ret==(dropping?OB_AI_FUNC_MODEL_NOT_FOUND:OB_AI_FUNC_MODEL_EXISTS),"retry did not recheck the current model state");
      check(state.refreshes==2 && state.starts==1 && state.rollbacks==1 && state.writes==0,"changed precondition reached mutation");
    } else {
      const int expected=mode=="timeout"?OB_TIMEOUT:mode=="cancel"?OB_CANCELED:mode=="start_error"?-9002:mode=="commit_error"?-9003:OB_EAGAIN;
      check(ret==expected && state.starts==1,"non-retryable/worker error lost or retried");
      if(mode=="operator_error") check(state.writes==1 && state.rollbacks==1 && state.commits==0 && state.publishes==0,"operator error was replayed");
      else if(mode=="publish_error") check(state.writes==1 && state.commits==1 && state.publishes==1,"post-commit publication was replayed");
      else if(mode=="commit_error") check(state.writes==1 && state.commits==1 && state.publishes==0,"failed commit was replayed or published");
      else check(state.writes==0 && state.rollbacks==1 && state.publishes==0,"startup failure reached a model mutation");
    }
    std::cout<<"PASS "<<argv[1]<<' '<<mode<<'\n';return 0;
  } catch(const std::exception &error) {
    std::cerr<<"FAIL "<<argv[1]<<' '<<mode<<": "<<error.what()<<'\n';return 1;
  }
}
'''


class AiModelDdlRetryContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("c++")
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="ai-model-ddl-retry-")
        folder = Path(cls.temp.name)
        source = Path(os.environ.get("SEEKDB_AI_MODEL_DDL_SOURCE", str(SOURCE))).read_text()
        source = "\n".join(line for line in source.splitlines() if not line.lstrip().startswith("#include"))
        cpp = folder / "contract.cpp"
        cpp.write_text(PREFIX + source + SUFFIX)
        cls.binary = folder / "contract"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all", str(cpp), "-o", str(cls.binary)]
        built = subprocess.run(command, text=True, capture_output=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_retry_and_failure_boundaries(self):
        env = dict(os.environ)
        if platform.system() == "Darwin":
            env["ASAN_OPTIONS"] = "detect_leaks=0"
        modes = ("success", "retry", "no_transaction", "changed", "timeout", "cancel",
                 "rollback_error", "start_error", "operator_error", "publish_error", "commit_error")
        for operation in ("create", "drop"):
            for mode in modes:
                with self.subTest(operation=operation, mode=mode):
                    ran = subprocess.run([str(self.binary), operation, mode], env=env,
                                         text=True, capture_output=True, timeout=10)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
