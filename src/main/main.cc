#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#include <libgen.h>
#include <signal.h>

#include "data/document_parser.h"
#include "data/feature_space_manager.h"
#include "data/query_request_parser.h"
#include "handler/document_handler.h"
#include "handler/query_handler.h"
#include "handler/snapshot_handler.h"
#include "handler/test_handler.h"
#include "index/document_index_manager.h"
#include "index/document_index_view.h"
#include "index/document_update_pipeline.h"
#include "query/simple_query_executor.h"
#include "ranking/direct_model.h"
#include "ranking/feature_mapping_model.h"
#include "ranking/model_manager.h"
#include "ranking/ranking_model.h"
#include "service/server.h"
#include "third_party/rapidjson/document.h"
#include "third_party/rapidjson/filereadstream.h"
#include "utils/json_utils.h"
#include "utils/logger.h"
#include "utils/logger-inl.h"
#include "utils/scope_guard.h"

using std::string;

namespace redgiant {

DECLARE_LOGGER(logger, __FILE__);

// constants used by configuration files
static const unsigned int kConfigParseFlags =
    rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag;
static const char kConfigKeyLoggerConfig    [] = "logger_config";
static const char kConfigKeyFeatureSpaces   [] = "feature_spaces";
static const char kConfigKeyIndex           [] = "index";
static const char kConfigKeyRanking         [] = "ranking";
static const char kConfigKeyServer          [] = "server";

static volatile int g_exit_signal = 0;

static void new_handler_abort() {
  std::abort();
}

void ignore_signal(int signal) {
  (void) signal;
}

void exit_on_signal(int signal) {
  (void) signal;
  g_exit_signal = signal;
}

static int read_config_file(const char* file_name, rapidjson::Document& config) {
  std::FILE* fp = std::fopen(file_name, "r");
  if (!fp) {
    fprintf(stderr, "Cannot open config file %s.\n", file_name);
    return -1;
  }

  char readBuffer[8192];
  rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  if (config.ParseStream<kConfigParseFlags>(is).HasParseError()) {
    fprintf(stderr, "Config file parse error %d at offset %zu.\n",
        (int)config.GetParseError(), config.GetErrorOffset());
    return -1;
  }

  std::fclose(fp);
  return 0;
}

static int init_log_config(const char* file_name, const rapidjson::Value& config) {
  const char* logger_file_name_str = json_get_str(config, kConfigKeyLoggerConfig);
  if (!logger_file_name_str) {
    fprintf(stderr, "Logger configuration not found! Using default configurations.");
    return init_logger(NULL);
  }

  // Copy the file name to a temporary buffer to call "dirname"
  size_t file_name_len = strlen(file_name);
  std::unique_ptr<char[]> file_name_buf(new char[file_name_len + 1]);
  strncpy(file_name_buf.get(), file_name, file_name_len);
  file_name_buf[file_name_len] = 0;
  std::string dir(dirname(file_name_buf.get()));
  if (!dir.empty() && dir.back() != '/') {
    dir += "/";
  }
  // get the file name relative to the configuration file.
  std::string logger_file_name = dir + std::string(logger_file_name_str);
  return init_logger(logger_file_name.c_str());
}

static int server_main(const rapidjson::Value& config) {
  /*
   * Initialize signal handler
   */
  signal(SIGPIPE, ignore_signal);
  signal(SIGHUP, ignore_signal);
  signal(SIGTERM, exit_on_signal);
  signal(SIGINT, exit_on_signal);

  /*
   * Initialization:
   * Features initialization
   */
  const rapidjson::Value* config_feature_spaces = json_get_node(config, kConfigKeyFeatureSpaces);
  if (!config_feature_spaces) {
    LOG_ERROR(logger, "features configuration does not exist!");
    return -1;
  }

  std::shared_ptr<FeatureSpaceManager> feature_spaces = std::make_shared<FeatureSpaceManager>();
  if (feature_spaces->initialize(*config_feature_spaces) < 0) {
    LOG_ERROR(logger, "feature cache parsing failed!");
    return -1;
  }

  /*
   * Initialization:
   * Index initialization
   */
  const rapidjson::Value* config_index = json_get_object(config, kConfigKeyIndex);
  if (!config_index) {
    LOG_ERROR(logger, "index configuration does not exist!");
    return -1;
  }

  int index_initial_buckets    = 100000;
  int index_max_size           = 5000000;
  int index_maintain_interval  = 300;

  if (config_index && json_try_get_value(*config_index, "initial_buckets", index_initial_buckets)) {
    LOG_DEBUG(logger, "index initial buckets: %d", index_initial_buckets);
  } else {
    LOG_DEBUG(logger, "index initial buckets not configured, use default: %d", index_initial_buckets);
  }
  if (config_index && json_try_get_value(*config_index, "max_size", index_max_size)) {
    LOG_DEBUG(logger, "index max size: %d", index_max_size);
  } else {
    LOG_DEBUG(logger, "index max size not configured, use default: %d", index_max_size);
  }
  if (config_index && json_try_get_value(*config_index, "maintain_interval", index_maintain_interval)) {
    LOG_DEBUG(logger, "index maintain interval: %d", index_maintain_interval);
  } else {
    LOG_DEBUG(logger, "index maintain interval not configured, use default: %d", index_maintain_interval);
  }

  bool restore_on_startup = false;
  bool dump_on_exit = false;
  std::string snapshot_prefix = "";

  if (config_index && json_try_get_value(*config_index, "restore_on_startup", restore_on_startup)) {
    LOG_DEBUG(logger, "index restore on startup: %s", restore_on_startup ? "true" : "false");
  } else {
    LOG_DEBUG(logger, "index restore on startup not configured, use default: %s", restore_on_startup ? "true" : "false");
  }
  if (config_index && json_try_get_value(*config_index, "dump_on_exit", dump_on_exit)) {
    LOG_DEBUG(logger, "index dump on exit: %s", dump_on_exit ? "true" : "false");
  } else {
    LOG_DEBUG(logger, "index dump on exit not configured, use default: %s", dump_on_exit ? "true" : "false");
  }
  if (config_index && json_try_get_value(*config_index, "snapshot_prefix", snapshot_prefix)) {
    LOG_DEBUG(logger, "index snapshot prefix: %s", snapshot_prefix.c_str());
  } else {
    LOG_DEBUG(logger, "index snapshot prefix not configured, use default: %s", snapshot_prefix.c_str());
  }

  std::unique_ptr<DocumentIndexManager> index;
  if (restore_on_startup) {
    LOG_INFO(logger, "loading index from snapshot %s", snapshot_prefix.c_str());
    try {
      index.reset(new DocumentIndexManager(
          index_initial_buckets, index_max_size, snapshot_prefix));
    } catch (std::ios_base::failure& e) {
      LOG_ERROR(logger, "failed restore index. reason:%s", e.what());
      // continue
    }
  }

  if (!index) {
    LOG_INFO(logger, "creating an empty index ...");
    index.reset(new DocumentIndexManager(
        index_initial_buckets, index_max_size));
  }

  index->start_maintain(index_maintain_interval, index_maintain_interval);
  ScopeGuard feed_index_guard([&index, dump_on_exit, &snapshot_prefix] {
    if (dump_on_exit) {
      LOG_INFO(logger, "dumping index to snapshot %s", snapshot_prefix.c_str());
      try {
        index->dump(snapshot_prefix);
      } catch (std::ios_base::failure& e) {
        LOG_ERROR(logger, "failed dump index. reason:%s", e.what());
        // continue
      }
    }
    LOG_INFO(logger, "index maintain thread stopping...");
    index->stop_maintain();
  });

  unsigned int document_update_thread_num = 4;
  unsigned int document_update_queue_size = 2048;
  unsigned int default_ttl = 86400;

  if (config_index && json_try_get_value(*config_index, "update_thread_num", document_update_thread_num)) {
    LOG_DEBUG(logger, "feed document pipeline thread num: %u", document_update_thread_num);
  } else {
    LOG_DEBUG(logger, "feed document pipeline thread num not configured, use default: %u", document_update_thread_num);
  }
  if (config_index && json_try_get_value(*config_index, "update_queue_size", document_update_queue_size)) {
    LOG_DEBUG(logger, "feed document pipeline queue size: %u", document_update_queue_size);
  } else {
    LOG_DEBUG(logger, "feed document pipeline queue size not configured, use default: %u", document_update_queue_size);
  }
  if (config_index && json_try_get_value(*config_index, "default_ttl", default_ttl)) {
    LOG_DEBUG(logger, "document update default ttl: %u", default_ttl);
  } else {
    LOG_DEBUG(logger, "document update default ttl not configured, use default: %u", default_ttl);
  }

  DocumentUpdatePipeline document_update_pipeline(document_update_thread_num, document_update_queue_size, index.get());
  document_update_pipeline.start();
  ScopeGuard document_update_pipeline_guard([&document_update_pipeline] {
    LOG_INFO(logger, "feed document pipeline stopping...");
    document_update_pipeline.stop();
  });

  DocumentIndexView index_view(index.get(), &document_update_pipeline);

  /*
   * Initialization
   * Query and ranking models
   */
  ModelManagerFactory model_manager_factory;
  model_manager_factory.register_model_factory(std::make_shared<DirectModelFactory>());
  model_manager_factory.register_model_factory(std::make_shared<FeatureMappingModelFactory>(feature_spaces));

  const rapidjson::Value* config_ranking = json_get_node(config, kConfigKeyRanking);
  if (!config_ranking) {
    LOG_ERROR(logger, "ranking model config does not exist!");
    return -1;
  }

  std::unique_ptr<RankingModel> model = model_manager_factory.create_model(*config_ranking);
  if (!model) {
    LOG_ERROR(logger, "ranking model initialization failed!");
    return -1;
  }

  /*
   * Initialization:
   * Server initialization
   */
  LOG_INFO(logger, "server initializing ...");
  int server_port = 19980;
  uint server_thread_num = 4;
  uint max_req_per_thread = 0;

  const rapidjson::Value* config_server = json_get_object(config, kConfigKeyServer);
  if (config_server && json_try_get_value(*config_server, "port", server_port)) {
    LOG_DEBUG(logger, "server port: %d", server_port);
  } else {
    LOG_DEBUG(logger, "server port not configured, use default: %d", server_port);
  }
  if (config_server && json_try_get_value(*config_server, "thread_num", server_thread_num)) {
    LOG_DEBUG(logger, "server thread num: %u", server_thread_num);
  } else {
    LOG_DEBUG(logger, "server thread num not configured, use default: %u", server_thread_num);
  }
  if (config_server && json_try_get_value(*config_server, "max_request_per_thread", max_req_per_thread)) {
    LOG_DEBUG(logger, "max requests per server thread: %u", max_req_per_thread);
  } else {
    LOG_DEBUG(logger, "max requests per server thread not configured, use default: %u", max_req_per_thread);
  }

  Server server(server_port, server_thread_num, max_req_per_thread);
  server.bind("/test", std::make_shared<TestHandlerFactory>());
  server.bind("/document", std::make_shared<FeedDocumentHandlerFactory>(
      std::make_shared<DocumentParserFactory>(feature_spaces),
      &index_view, default_ttl));
  server.bind("/query", std::make_shared<QueryHandlerFactory>(
      std::make_shared<QueryRequestParserFactory>(feature_spaces),
      std::make_shared<SimpleQueryExecutorFactory>(index.get(), model.get())));
  server.bind("/snapshot", std::make_shared<SnapshotHandlerFactory>(&index_view, snapshot_prefix));

  if (server.initialize() < 0) {
    LOG_ERROR(logger, "server initialization failed!");
    return -1;
  }

  if (server.start() < 0) {
    LOG_ERROR(logger, "failed to start server!");
    return -1;
  }

  ScopeGuard server_guard([&server] {
    LOG_INFO(logger, "server exiting...");
    server.stop();
  });

  LOG_INFO(logger, "service started successfully.");

  /*
   * Main loop: wait for exit signal.
   */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);

  sigset_t orig_mask;
  sigprocmask(SIG_BLOCK, &mask, &orig_mask);
  g_exit_signal = 0;
  while (!g_exit_signal) {
    sigsuspend(&orig_mask);
  }

  /*
   * Exit: everything exits elegantly
   */
  LOG_INFO(logger, "received signal %d. exiting.", g_exit_signal);
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  std::set_new_handler(new_handler_abort);

  if (argc != 2) {
    fprintf(stderr, "Usage: %s config_file\n", argv[0]);
    return -1;
  }

  rapidjson::Document config;
  if (read_config_file(argv[1], config) < 0) {
    fprintf(stderr, "Failed to open config file %s.\n", argv[1]);
    return -1;
  }

  if (init_log_config(argv[1], config) < 0) {
    fprintf(stderr, "Failed to initialize log config.\n");
    return -1;
  }

  int ret = -1;
  try {
    ret = server_main(config);
  } catch (...) {
    // don't make this happen
    ret = -1;
    LOG_ERROR(logger, "unkown error happened");
  }

  if (ret >= 0) {
    LOG_INFO(logger, "exit successfully.");
  } else {
    LOG_INFO(logger, "exit with failure.");
  }
  return ret;
}

} /* namespace redgiant */

int main(int argc, char** argv) {
  return redgiant::main(argc, argv);
}
