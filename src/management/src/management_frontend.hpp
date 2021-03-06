#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/thread.hpp>

#include <redox.hpp>
#include <server_http.hpp>

#include "rapidjson/document.h"

#include <clipper/config.hpp>
#include <clipper/datatypes.hpp>
#include <clipper/json_util.hpp>
#include <clipper/logging.hpp>
#include <clipper/persistent_state.hpp>
#include <clipper/redis.hpp>
#include <clipper/selection_policies.hpp>
#include <clipper/util.hpp>

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
using clipper::VersionedModelId;
using clipper::InputType;
using clipper::json::add_string;
using clipper::json::add_bool;
using clipper::json::get_bool;
using clipper::json::get_candidate_models;
using clipper::json::get_int;
using clipper::json::get_string;
using clipper::json::get_string_array;
using clipper::json::json_parse_error;
using clipper::json::json_semantic_error;
using clipper::json::parse_json;
using clipper::json::redis_app_metadata_to_json;
using clipper::json::redis_model_metadata_to_json;
using clipper::json::redis_container_metadata_to_json;
using clipper::json::to_json_string;

namespace management {

const std::string LOGGING_TAG_MANAGEMENT_FRONTEND = "MGMTFRNTD";

const std::string ADMIN_PATH = "^/admin";
const std::string ADD_APPLICATION = ADMIN_PATH + "/add_app$";
const std::string ADD_MODEL = ADMIN_PATH + "/add_model$";
const std::string SET_MODEL_VERSION = ADMIN_PATH + "/set_model_version$";

// const std::string ADD_CONTAINER = ADMIN_PATH + "/add_container$";
const std::string GET_METRICS = ADMIN_PATH + "/metrics$";
const std::string GET_SELECTION_STATE = ADMIN_PATH + "/get_state$";
const std::string GET_ALL_APPLICATIONS = ADMIN_PATH + "/get_all_applications$";
const std::string GET_APPLICATION = ADMIN_PATH + "/get_application$";
const std::string GET_ALL_MODELS = ADMIN_PATH + "/get_all_models$";
const std::string GET_MODEL = ADMIN_PATH + "/get_model$";
const std::string GET_ALL_CONTAINERS = ADMIN_PATH + "/get_all_containers$";
const std::string GET_CONTAINER = ADMIN_PATH + "/get_container$";

const std::string ADD_APPLICATION_JSON_SCHEMA = R"(
  {
   "name" := string,
   "candidate_model_names" := [string],
   "input_type" := "integers" | "bytes" | "floats" | "doubles" | "strings",
   "default_output" := float,
   "latency_slo_micros" := int
  }
)";

const std::string VERBOSE_OPTION_JSON_SCHEMA = R"(
  {
    "verbose" := bool
  }
)";

const std::string GET_APPLICATION_REQUESTS_SCHEMA = R"(
  {
    "name" := string
  }
)";

const std::string GET_MODEL_REQUESTS_SCHEMA = R"(
  {
    "model_name" := string,
    "model_version" := int
  }
)";

const std::string GET_CONTAINER_REQUESTS_SCHEMA = R"(
  {
    "model_name" := string,
    "model_version" := int,
    "replica_id" := int
  }
)";

const std::string ADD_MODEL_JSON_SCHEMA = R"(
  {
   "model_name" := string,
   "model_version" := int,
   "labels" := [string],
   "input_type" := "integers" | "bytes" | "floats" | "doubles" | "strings",
   "container_name" := string,
   "model_data_path" := string
  }
)";

const std::string SET_VERSION_JSON_SCHEMA = R"(
  {
   "model_name" := string,
   "model_version" := int,
  }
)";

const std::string SELECTION_JSON_SCHEMA = R"(
  {
   "app_name" := string,
   "uid" := int,
  }
)";

void respond_http(std::string content, std::string message,
                  std::shared_ptr<HttpServer::Response> response) {
  *response << "HTTP/1.1 " << message
            << "\r\nContent-Length: " << content.length() << "\r\n\r\n"
            << content << "\n";
}

/* Generate a user-facing error message containing the exception
 * content and the expected JSON schema. */
std::string json_error_msg(const std::string& exception_msg,
                           const std::string& expected_schema) {
  std::stringstream ss;
  ss << "Error parsing JSON: " << exception_msg << ". "
     << "Expected JSON schema: " << expected_schema;
  return ss.str();
}

class RequestHandler {
 public:
  RequestHandler(int portno, int num_threads)
      : server_(portno, num_threads), state_db_{} {
    clipper::Config& conf = clipper::get_config();
    while (!redis_connection_.connect(conf.get_redis_address(),
                                      conf.get_redis_port())) {
      clipper::log_error(LOGGING_TAG_MANAGEMENT_FRONTEND,
                         "Management frontend failed to connect to Redis",
                         "Retrying in 1 second...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    while (!redis_subscriber_.connect(conf.get_redis_address(),
                                      conf.get_redis_port())) {
      clipper::log_error(
          LOGGING_TAG_MANAGEMENT_FRONTEND,
          "Management frontend subscriber failed to connect to Redis",
          "Retrying in 1 second...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    server_.add_endpoint(
        ADD_APPLICATION, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Add application POST request");
            std::string result = add_application(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), ADD_APPLICATION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), ADD_APPLICATION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        ADD_MODEL, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Add model POST request");
            std::string result = add_model(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), ADD_MODEL_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), ADD_MODEL_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        SET_MODEL_VERSION, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            rapidjson::Document d;
            parse_json(request->content.string(), d);
            std::string model_name = get_string(d, "model_name");
            int new_model_version = get_int(d, "model_version");

            bool success = set_model_version(model_name, new_model_version);
            if (success) {
              respond_http("SUCCESS", "200 OK", response);
            } else {
              std::string err_msg = "ERROR: Version " +
                                    std::to_string(new_model_version) +
                                    " does not exist for model " + model_name;
              respond_http(err_msg, "400 Bad Request", response);
            }
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), SET_VERSION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), SET_VERSION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_ALL_APPLICATIONS, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get all applications POST request");
            std::string result =
                get_all_applications(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_APPLICATION, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get application info POST request");
            std::string result = get_application(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_APPLICATION_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_APPLICATION_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_ALL_MODELS, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get all models POST request");
            std::string result = get_all_models(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_MODEL, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get model info POST request");
            std::string result = get_model(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_MODEL_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_MODEL_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_ALL_CONTAINERS, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get all containers POST request");
            std::string result = get_all_containers(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), VERBOSE_OPTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_CONTAINER, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            clipper::log_info(LOGGING_TAG_MANAGEMENT_FRONTEND,
                              "Get model info POST request");
            std::string result = get_container(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_CONTAINER_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), GET_CONTAINER_REQUESTS_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
    server_.add_endpoint(
        GET_SELECTION_STATE, "POST",
        [this](std::shared_ptr<HttpServer::Response> response,
               std::shared_ptr<HttpServer::Request> request) {
          try {
            std::string result = get_selection_state(request->content.string());
            respond_http(result, "200 OK", response);
          } catch (const json_parse_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), SELECTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const json_semantic_error& e) {
            std::string err_msg =
                json_error_msg(e.what(), SELECTION_JSON_SCHEMA);
            respond_http(err_msg, "400 Bad Request", response);
          } catch (const std::invalid_argument& e) {
            respond_http(e.what(), "400 Bad Request", response);
          }
        });
  }

  ~RequestHandler() {
    redis_subscriber_.disconnect();
    redis_connection_.disconnect();
  }

  /**
   * Creates an endpoint that listens for requests to add new prediction
   * applications to Clipper.
   *
   * JSON format:
   * {
   *  "name" := string,
   *  "candidate_model_names" := [string],
   *  "input_type" := "integers" | "bytes" | "floats" | "doubles" | "strings",
   *  "default_output" := float,
   *  "latency_slo_micros" := int
   * }
   */
  std::string add_application(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    std::string app_name = get_string(d, "name");
    std::vector<string> candidate_model_names =
        get_string_array(d, "candidate_model_names");
    if (candidate_model_names.size() != 1) {
      std::stringstream ss;
      ss << "Applications must provide exactly 1 candidate model. ";
      ss << app_name << " provided " << candidate_model_names.size();
      std::string error_msg = ss.str();
      clipper::log_error(LOGGING_TAG_MANAGEMENT_FRONTEND, error_msg);
      throw std::invalid_argument(error_msg);
    }
    InputType input_type =
        clipper::parse_input_type(get_string(d, "input_type"));
    std::string default_output = get_string(d, "default_output");
    std::string selection_policy =
        clipper::DefaultOutputSelectionPolicy::get_name();
    int latency_slo_micros = get_int(d, "latency_slo_micros");
    // check if application already exists
    std::unordered_map<std::string, std::string> existing_app_data =
        clipper::redis::get_application(redis_connection_, app_name);
    if (existing_app_data.empty()) {
      if (clipper::redis::add_application(
              redis_connection_, app_name, candidate_model_names, input_type,
              selection_policy, default_output, latency_slo_micros)) {
        return "Success!";
      } else {
        std::stringstream ss;
        ss << "Error adding application " << app_name << " to Redis";
        throw std::invalid_argument(ss.str());
      }
    } else {
      std::stringstream ss;
      ss << "Error application " << app_name << " already exists";
      throw std::invalid_argument(ss.str());
    }
  }

  /**
   * Creates an endpoint that listens for requests to add new models to
   * Clipper.
   *
   * JSON format:
   * {
   *  "model_name" := string,
   *  "model_version" := int,
   *  "labels" := [string]
   *  "input_type" := "integers" | "bytes" | "floats" | "doubles" | "strings",
   *  "container_name" := string,
   *  "model_data_path" := string
   * }
   */
  std::string add_model(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    VersionedModelId model_id = std::make_pair(get_string(d, "model_name"),
                                               get_int(d, "model_version"));
    std::vector<std::string> labels = get_string_array(d, "labels");
    InputType input_type =
        clipper::parse_input_type(get_string(d, "input_type"));
    std::string container_name = get_string(d, "container_name");
    std::string model_data_path = get_string(d, "model_data_path");

    // check if this version of the model has already been deployed
    std::unordered_map<std::string, std::string> existing_model_data =
        clipper::redis::get_model(redis_connection_, model_id);
    if (existing_model_data.empty()) {
      if (clipper::redis::add_model(redis_connection_, model_id, input_type,
                                    labels, container_name, model_data_path)) {
        if (set_model_version(model_id.first, model_id.second)) {
          return "Success!";
        }
      }
      std::stringstream ss;
      ss << "Error adding model " << model_id.first << ":" << model_id.second
         << " to Redis";
      throw std::invalid_argument(ss.str());
    } else {
      std::stringstream ss;
      ss << "Error model " << model_id.first << ":" << model_id.second
         << " already exists";
      throw std::invalid_argument(ss.str());
    }
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * registered Clipper applications.
   *
   * JSON format:
   * {
   *  "verbose" := bool
   * }
   *
   * \return Returns a JSON string that encodes a list with info about
   * registered apps. If `verbose` == False, the encoded list has all registered
   * apps' names. Else, the encoded map contains objects with full app
   * information.
   *
   */
  std::string get_all_applications(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    bool verbose = get_bool(d, "verbose");

    std::vector<std::string> app_names =
        clipper::redis::get_all_application_names(redis_connection_);

    rapidjson::Document response_doc;
    response_doc.SetArray();

    if (verbose) {
      for (const string& app_name : app_names) {
        std::unordered_map<std::string, std::string> app_metadata =
            clipper::redis::get_application(redis_connection_, app_name);
        rapidjson::Document app_doc(&response_doc.GetAllocator());
        redis_app_metadata_to_json(app_doc, app_metadata);
        /* We need to add each app's name to its returned JSON object. */
        add_string(app_doc, "name", app_name);
        response_doc.PushBack(app_doc, response_doc.GetAllocator());
      }
    } else {
      for (const string& app_name : app_names) {
        rapidjson::Value v;
        v.SetString(app_name.c_str(), app_name.length(),
                    response_doc.GetAllocator());
        response_doc.PushBack(v, response_doc.GetAllocator());
      }
    }
    return to_json_string(response_doc);
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * a specified Clipper application.
   *
   * JSON format:
   * {
   *  "name" := string
   * }
   *
   * \return Returns a JSON string encoding a map of the specified application's
   * attribute name-value pairs.
   *
   */
  std::string get_application(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    std::string app_name = get_string(d, "name");
    std::unordered_map<std::string, std::string> app_metadata =
        clipper::redis::get_application(redis_connection_, app_name);

    rapidjson::Document response_doc;
    response_doc.SetObject();

    if (app_metadata.size() > 0) {
      /* We assume that redis::get_application returns an empty map iff no app
       * exists */
      /* If an app does exist, we need to add its name to the map. */
      redis_app_metadata_to_json(response_doc, app_metadata);
      add_string(response_doc, "name", app_name);
    }

    return to_json_string(response_doc);
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * registered Clipper models.
   *
   * JSON format:
   * {
   *  "verbose" := bool
   * }
   *
   * \return Returns a JSON string that encodes a list with info about
   * registered models. If `verbose` == False, the encoded list has all
   * registered
   * models' names. Else, the encoded map contains objects with full model
   * information.
   *
   */
  std::string get_all_models(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    bool verbose = get_bool(d, "verbose");

    std::vector<VersionedModelId> models =
        clipper::redis::get_all_models(redis_connection_);

    rapidjson::Document response_doc;
    response_doc.SetArray();
    // need to maintain references
    std::vector<std::string> model_strs;

    if (verbose) {
      for (auto model : models) {
        std::unordered_map<std::string, std::string> model_metadata =
            clipper::redis::get_model(redis_connection_, model);
        rapidjson::Document model_doc(&response_doc.GetAllocator());
        redis_model_metadata_to_json(model_doc, model_metadata);
        bool is_current_version =
            clipper::redis::get_current_model_version(
                redis_connection_, model.first) == model.second;
        add_bool(model_doc, "is_current_version", is_current_version);
        response_doc.PushBack(model_doc, response_doc.GetAllocator());
      }
    } else {
      for (auto model : models) {
        std::string model_str = clipper::versioned_model_to_str(model);
        rapidjson::Value v;
        v.SetString(model_str.c_str(), model_str.length(),
                    response_doc.GetAllocator());
        response_doc.PushBack(v, response_doc.GetAllocator());
      }
    }
    std::string result = to_json_string(response_doc);
    clipper::log_info_formatted(LOGGING_TAG_MANAGEMENT_FRONTEND,
                                "get_all_models response: {}", result);
    return result;
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * a specified Clipper model.
   *
   * JSON format:
   * {
   *  "model_name" := string,
   *  "model_version" := int,
   * }
   *
   * \return Returns a JSON string encoding a map of the specified model's
   * attribute name-value pairs, including whether it is the currently deployed
   * version of the model.
   *
   */
  std::string get_model(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    std::string model_name = get_string(d, "model_name");
    int model_version = get_int(d, "model_version");
    VersionedModelId model = std::make_pair(model_name, model_version);

    std::unordered_map<std::string, std::string> model_metadata =
        clipper::redis::get_model(redis_connection_, model);

    rapidjson::Document response_doc;
    response_doc.SetObject();

    if (model_metadata.size() > 0) {
      /* We assume that redis::get_model returns an empty map iff no model
       * exists */
      redis_model_metadata_to_json(response_doc, model_metadata);
      bool is_current_version =
          clipper::redis::get_current_model_version(
              redis_connection_, model.first) == model.second;
      add_bool(response_doc, "is_current_version", is_current_version);
    }

    return to_json_string(response_doc);
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * Clipper containers.
   *
   * JSON format:
   * {
   *  "verbose" := bool
   * }
   *
   * \return Returns a JSON string that encodes a list with info about
   * containers.
   * If `verbose` == False, the encoded list has all containers' names (model
   * name, model version, container ID).
   * Else, the encoded map contains objects with full container information.
   *
   */
  std::string get_all_containers(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    bool verbose = get_bool(d, "verbose");

    std::vector<std::pair<VersionedModelId, int>> containers =
        clipper::redis::get_all_containers(redis_connection_);

    rapidjson::Document response_doc;
    response_doc.SetArray();

    if (verbose) {
      for (auto container : containers) {
        std::unordered_map<std::string, std::string> container_metadata =
            clipper::redis::get_container(redis_connection_, container.first,
                                          container.second);
        rapidjson::Document container_doc(&response_doc.GetAllocator());
        redis_container_metadata_to_json(container_doc, container_metadata);
        response_doc.PushBack(container_doc, response_doc.GetAllocator());
      }
    } else {
      for (auto container : containers) {
        std::stringstream ss;
        ss << clipper::versioned_model_to_str(container.first);
        ss << ":";
        ss << container.second;
        std::string container_str = ss.str();
        rapidjson::Value v;
        v.SetString(container_str.c_str(), container_str.length(),
                    response_doc.GetAllocator());
        response_doc.PushBack(v, response_doc.GetAllocator());
      }
    }
    std::string result = to_json_string(response_doc);
    clipper::log_info_formatted(LOGGING_TAG_MANAGEMENT_FRONTEND,
                                "get_all_containers response: {}", result);
    return result;
  }

  /**
   * Creates an endpoint that listens for requests to retrieve info about
   * a specified Clipper container.
   *
   * JSON format:
   * {
   *  "model_name" := string,
   *  "model_version" := int,
   *  "replica_id" := int
   * }
   *
   * \return Returns a JSON string encoding a map of the specified container's
   * attribute name-value pairs.
   *
   */
  std::string get_container(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    std::string model_name = get_string(d, "model_name");
    int model_version = get_int(d, "model_version");
    int replica_id = get_int(d, "replica_id");
    VersionedModelId model = std::make_pair(model_name, model_version);

    std::unordered_map<std::string, std::string> container_metadata =
        clipper::redis::get_container(redis_connection_, model, replica_id);

    rapidjson::Document response_doc;
    response_doc.SetObject();

    if (container_metadata.size() > 0) {
      /* We assume that redis::get_container returns an empty map iff no
       * container exists */
      redis_container_metadata_to_json(response_doc, container_metadata);
    }

    return to_json_string(response_doc);
  }

  /**
   * Creates an endpoint that looks up the debug string
   * for a user's selection policy state for an application.
   *
   * JSON format:
   * {
   *  "app_name" := string,
   *  "uid" := int,
   * }
   */
  std::string get_selection_state(const std::string& json) {
    rapidjson::Document d;
    parse_json(json, d);

    std::string app_name = get_string(d, "app_name");
    int uid = get_int(d, "uid");
    if (uid != clipper::DEFAULT_USER_ID) {
      clipper::log_error_formatted(LOGGING_TAG_MANAGEMENT_FRONTEND,
                                   "Personalized default outputs are not "
                                   "currently supported. Using default UID {} "
                                   "instead",
                                   clipper::DEFAULT_USER_ID);
      uid = clipper::DEFAULT_USER_ID;
    }
    auto app_metadata =
        clipper::redis::get_application(redis_connection_, app_name);
    return app_metadata["default_output"];
  }

  bool set_model_version(const string& model_name,
                         const int new_model_version) {
    std::vector<int> versions =
        clipper::redis::get_model_versions(redis_connection_, model_name);
    bool version_exists = false;
    for (auto v : versions) {
      if (v == new_model_version) {
        version_exists = true;
        break;
      }
    }
    if (version_exists) {
      return clipper::redis::set_current_model_version(
          redis_connection_, model_name, new_model_version);
    } else {
      clipper::log_error_formatted(
          LOGGING_TAG_MANAGEMENT_FRONTEND,
          "Cannot set non-existent version {} for model {}", new_model_version,
          model_name);
      return false;
    }
  }

  void start_listening() { server_.start(); }

 private:
  HttpServer server_;
  redox::Redox redis_connection_;
  redox::Subscriber redis_subscriber_;
  clipper::StateDB state_db_;
};

}  // namespace management
