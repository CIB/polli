#include "polli/Db.h"
#include "polli/Jit.h"
#include "polli/Options.h"
#include "polli/log.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"

#include <pqxx/pqxx>
#include <ctime>
#include <iostream>
#include <numeric>
#include <set>
#include <stdlib.h>
#include <string>
#include <thread>

using namespace pqxx;
using namespace llvm;

namespace papi {
#include <papi.h>
}

namespace polli {
namespace opt {
static std::string Experiment;
static cl::opt<std::string, true>
    ExperimentX("polli-db-experiment",
                cl::desc("Name of the experiment we are running under."),
                cl::location(Experiment), cl::init("unknown"),
                cl::cat(PolyJIT_Runtime));

static std::string ExperimentUUID;
static cl::opt<std::string, true>
    ExperimentUUIDX("polli-db-experiment-uuid", cl::desc("Experiment UUID."),
                    cl::location(ExperimentUUID),
                    cl::init("00000000-0000-0000-0000-000000000000"),
                    cl::cat(PolyJIT_Runtime));

static std::string Project;
static cl::opt<std::string, true>
    ProjectX("polli-db-project", cl::desc("The project we are running under."),
             cl::location(Project), cl::init("unknown"),
             cl::cat(PolyJIT_Runtime));

static std::string Domain;
static cl::opt<std::string, true>
    DomainX("polli-db-domain", cl::desc("The domain we are running under."),
            cl::location(Domain), cl::init("unknown"),
            cl::cat(PolyJIT_Runtime));

static std::string Group;
static cl::opt<std::string, true>
    GroupX("polli-db-group", cl::desc("The group we are running under."),
           cl::location(Group), cl::init("unknown"), cl::cat(PolyJIT_Runtime));

static std::string SourceUri;
static cl::opt<std::string, true> SourceUriX(
    "polli-db-src-uri", cl::desc("The src_uri we are running under."),
    cl::location(SourceUri), cl::init("unknown"), cl::cat(PolyJIT_Runtime));

static std::string Argv0;
static cl::opt<std::string, true>
    Argv0X("polli-db-argv", cl::desc("The command we are executing."),
           cl::location(SourceUri), cl::init("unknown"),
           cl::cat(PolyJIT_Runtime));

static bool EnableDatabase;
static cl::opt<bool, true> EnableDatabaseX(
    "polli-db-enable", cl::desc("Enable database communication."),
    cl::location(EnableDatabase), cl::init(false), cl::cat(PolyJIT_Runtime));

static bool ExecuteAtExit;
static cl::opt<bool, true> ExecuteAtExitX(
    "polli-db-execute-atexit", cl::desc("Enable execution of atexit handler."),
    cl::location(ExecuteAtExit), cl::init(false), cl::cat(PolyJIT_Runtime));

static std::string DbHost;
static cl::opt<std::string, true>
    DbHostX("polli-db-host", cl::desc("DB Hostname"), cl::location(DbHost),
            cl::init("localhost"), cl::cat(PolyJIT_Runtime));

static int DbPort;
static cl::opt<int, true> DbPortX("polli-db-port", cl::desc("DB Port"),
                                  cl::location(DbPort), cl::init(5432),
                                  cl::cat(PolyJIT_Runtime));

static std::string DbUsername;
static cl::opt<std::string, true> DbUsernameX("polli-db-username",
                                              cl::desc("DB Username"),
                                              cl::location(DbUsername),
                                              cl::init("benchbuild"),
                                              cl::cat(PolyJIT_Runtime));

static std::string DbPassword;
static cl::opt<std::string, true> DbPasswordX("polli-db-password",
                                              cl::desc("DB Password"),
                                              cl::location(DbPassword),
                                              cl::init("benchbuild"),
                                              cl::cat(PolyJIT_Runtime));
static std::string DbName;
static cl::opt<std::string, true> DbNameX("polli-db-name", cl::desc("DB Name"),
                                          cl::location(DbName),
                                          cl::init("benchbuild"),
                                          cl::cat(PolyJIT_Runtime));

static std::string RunGroupUUID;
static cl::opt<std::string, true>
    DbRunGroupUUIDX("polli-db-run-group", cl::desc("DB RunGroup (UUID)"),
                    cl::location(RunGroupUUID),
                    cl::init("00000000-0000-0000-0000-000000000000"),
                    cl::cat(PolyJIT_Runtime));

static int RunID;
static cl::opt<int, true> DbRunIdX("polli-db-run-id",
                                   cl::desc("DB RunGroup (UUID)"),
                                   cl::location(RunID), cl::init(0),
                                   cl::cat(PolyJIT_Runtime));
}

std::string now() {
  char buf[sizeof "YYYY-MM-DDTHH:MM:SS"];
  time_t now;
  time(&now);

  strftime(buf, sizeof buf, "%F %T", localtime(&now));
  return std::string(buf);
}

static bool enable_tracking() {
  return opt::EnableDatabase;
}

static pqxx::result submit(const std::string &Query,
                           pqxx::work &w) throw(pqxx::syntax_error) {
  pqxx::result res;
  try {
    res = w.exec(Query);
  } catch (pqxx::data_exception e) {
    std::cerr << "pgsql: Encountered the following error:\n";
    std::cerr << e.what();
    std::cerr << "\n";
    std::cerr << e.query();
    throw e;
  }
  return res;
}


class DBConnection {
  std::unique_ptr<pqxx::connection> c;
  std::string ConnectionString;

  std::string Experiment;
  std::string ExperimentUUID;

  std::string Project;
  std::string Domain;
  std::string Group;
  std::string SourceURI;
  std::string Argv0;

  std::string RunGroupUUID;
  int RunID;

  void connect() {
    if (!enable_tracking())
      return;

    c = std::unique_ptr<pqxx::connection>(
        new pqxx::connection(ConnectionString));
  }

public:
  explicit DBConnection(std::string Experiment, std::string ExperimentUUID,
                        std::string Project, std::string Domain,
                        std::string Group, std::string SourceURI,
                        std::string Argv0, std::string RunGroupUUID, int RunID)
      : Experiment(Experiment), ExperimentUUID(ExperimentUUID),
        Project(Project), Domain(Domain), Group(Group), SourceURI(SourceURI),
        Argv0(Argv0), RunGroupUUID(RunGroupUUID), RunID(RunID) {
    std::string CONNECTION_FMT_STR =
        "user={} port={} host={} dbname={} password={}";
    ConnectionString =
        fmt::format(CONNECTION_FMT_STR, opt::DbUsername, opt::DbPort,
                    opt::DbHost, opt::DbName, opt::DbPassword);
  }

  void prepare() {
    if (c) {
      std::string SELECT_RUN =
          "SELECT id,type,timestamp FROM papi_results WHERE run_id=$1 ORDER BY "
          "timestamp;";
      std::string SELECT_SIMPLE_RUN = "SELECT id,type,start,duration,name,tid "
                                      "FROM benchbuild_events WHERE run_id=$1 "
                                      "ORDER BY "
                                      "start;";
      std::string DELETE_SIMPLE_RUN =
          "DELETE FROM benchbuild_events WHERE run_id=$1";
      std::string SELECT_RUN_IDs = "SELECT id FROM run WHERE run_group = $1;";
      std::string SELECT_RUN_GROUPS =
          "SELECT DISTINCT run_group FROM run WHERE experiment_group = $1;";

      c->prepare("select_run", SELECT_RUN);
      c->prepare("select_simple_run", SELECT_SIMPLE_RUN);
      c->prepare("delete_simple_run", DELETE_SIMPLE_RUN);
      c->prepare("select_run_ids", SELECT_RUN_IDs);
      c->prepare("select_run_groups", SELECT_RUN_GROUPS);
    }
  }

  pqxx::connection &operator->() {
    if (c)
      return *c;
    connect();
    return *c;
  }

  pqxx::connection &operator*() {
    if (c)
      return *c;
    connect();
    return *c;
  }

  uint64_t prepareRun(pqxx::work &w) {
    std::string SEARCH_PROJECT_SQL =
        "SELECT name FROM project WHERE name = '{}';";

    std::string NEW_PROJECT_SQL =
        "INSERT INTO project (name, description, src_url, domain, group_name) "
        "VALUES ('{}', '{}', '{}', '{}', '{}');";

    std::string NEW_RUN_SQL =
        "INSERT INTO run (\"end\", command, "
        "project_name, experiment_name, run_group, experiment_group) "
        "VALUES (TIMESTAMP '{}', '{}', "
        "'{}', '{}', '{}', '{}') RETURNING id;";

    pqxx::result project_exists =
        submit(fmt::format(SEARCH_PROJECT_SQL, Project), w);

    if (project_exists.affected_rows() == 0)
      submit(fmt::format(NEW_PROJECT_SQL, Project, Project,
                         SourceURI, Domain, Group),
             w);

    uint64_t run_id = 0;
    if (!opt::RunID) {
      pqxx::result r =
          submit(fmt::format(NEW_RUN_SQL, now(), Argv0, Project, Experiment,
                             RunGroupUUID, ExperimentUUID),
                 w);
      r[0]["id"].to(run_id);
    } else {
      run_id = RunID;
    }

    return run_id;
  }

  ~DBConnection() {
    if (c && c->is_open())
      c->disconnect();
    c.reset(nullptr);
  }
};

struct DBCreator {
  static void *call() {
    return new DBConnection(
        opt::Experiment, opt::ExperimentUUID, opt::Project, opt::Domain,
        opt::Group, opt::SourceUri, opt::Argv0, opt::RunGroupUUID, opt::RunID);
  }
};

static llvm::ManagedStatic<DBConnection, DBCreator> DB;

struct Event {
  std::string Name;
  uint64_t ID;
  uint64_t Time;
};

namespace db {
void ValidateOptions() {
  // This needs to be supported via environment variable too
  // because there is no way for the tool 'benchbuild' to provide
  // the run_id as program argument for now.
  if (opt::RunID == 0) {
    if (const char *run_id = std::getenv("BB_DB_RUN_ID")) {
      opt::RunID = run_id ? std::stoi(run_id) : 0;
    }
  }

  DB->prepare();
}
void StoreRun(const EventMapTy &Events, const EventMapTy &Entries,
              const RegionMapTy &Regions) {
  if (!enable_tracking())
    return;

  pqxx::work w(**DB);
  uint64_t run_id = DB->prepareRun(w);

  std::string NEW_RUN_RESULT_SQL = "INSERT INTO regions (name, id, "
                                   "duration, events, run_id) "
                                   "VALUES";

  if (Events.size() <= 0)
    return;

  int cnt = 0;
  std::stringstream vals;
  for (auto KV : Events) {
    if (cnt > 0)
      vals << ",";
    vals << fmt::format(" ('{:s}', {:d}, {:d}, {:d}, {:d})",
                        Regions.at(KV.first), KV.first, KV.second,
                        Entries.at(KV.first), run_id);
    cnt++;
  }
  vals << ";";
  submit(NEW_RUN_RESULT_SQL + vals.str(), w);
  vals.clear();
  vals.flush();
  w.commit();
}

void StoreTransformedScop(const std::string &FnName,
                          const std::string &IslAstStr,
                          const std::string &ScheduleTreeStr) {
  if (!enable_tracking())
    return;

  pqxx::work w(**DB);
  uint64_t run_id = DB->prepareRun(w);

  std::string SCHEDULE_SQL = "INSERT INTO schedules (function, schedule, "
                             "run_id) VALUES ('{:s}', '{:s}', {:d});";
  std::string AST_SQL = "INSERT INTO isl_asts (function, ast, run_id) VALUES "
                        "('{:s}', '{:s}', {:d});";

  submit(fmt::format(SCHEDULE_SQL, FnName, ScheduleTreeStr, run_id), w);
  submit(fmt::format(AST_SQL, FnName, IslAstStr, run_id), w);
  w.commit();
}
}

namespace tracing {
static ManagedStatic<TraceData> TD;

void enter_region(uint64_t id, const char *name) {
  uint64_t time = papi::PAPI_get_real_usec();
  if (!TD->Events.count(id))
    TD->Events[id] = 0;
  if (!TD->Entries.count(id))
    TD->Entries[id] = 0;
  if (!TD->Regions.count(id))
    TD->Regions[id] = name;
  TD->Events[id] -= time;
  TD->Entries[id] += 1;
}

void exit_region(uint64_t id) {
  uint64_t time = papi::PAPI_get_real_usec();
  TD->Events[id] += time;
}

TraceData::~TraceData() {
  if (!polli::opt::ExecuteAtExit)
    return;

  std::cerr << fmt::format("Submitting: {:d} events", Events.size()) << "\n";
  polli::db::StoreRun(Events, Entries, Regions);
}

void setup_tracing() {
  cl::ParseEnvironmentOptions("profile-scops", "PJIT_ARGS", "");
  opt::ValidateOptions();
  db::ValidateOptions();
  papi::PAPI_library_init(PAPI_VER_CURRENT);
}
}
}
