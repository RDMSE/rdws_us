//
// FieldService — capabilities: field.list, field.get, field.create, field.update, field.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;

namespace {

rapidjson::Document makeError(const std::string& msg, int code = 400)
{
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "error", alloc);
    doc.AddMember("statusCode", code, alloc);
    doc.AddMember("message", rapidjson::Value(msg.c_str(), alloc), alloc);
    return doc;
}

std::string getPathParam(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember("pathParameters") && req["pathParameters"].IsObject()) {
      if (const auto& pp = req["pathParameters"];
          pp.HasMember(key.c_str()) && pp[key.c_str()].IsString()) {
        return pp[key.c_str()].GetString();
      }
    }
    return {};
}

std::string getQueryParam(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember("queryStringParameters") && req["queryStringParameters"].IsObject()) {
    if (const auto& qp = req["queryStringParameters"];
          qp.HasMember(key.c_str()) && qp[key.c_str()].IsString()) {
        return qp[key.c_str()].GetString();
      }
    }
    return {};
}

rapidjson::Value fieldFromRow(IResultSet& rs, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id",       rapidjson::Value(rs.getString("id").c_str(), alloc),       alloc);
    obj.AddMember("farm_id",  rapidjson::Value(rs.getString("farm_id").c_str(), alloc),  alloc);
    obj.AddMember("name",     rapidjson::Value(rs.getString("name").c_str(), alloc),     alloc);
    if (!rs.isNull("area")) {
      obj.AddMember("area", rapidjson::Value(rs.getString("area").c_str(), alloc), alloc);
    }
    if (!rs.isNull("geometry")) {
      obj.AddMember("geometry", rapidjson::Value(rs.getString("geometry").c_str(), alloc), alloc);
    }
    obj.AddMember("created_at", rapidjson::Value(rs.getString("created_at").c_str(), alloc), alloc);
    if (!rs.isNull("updated_at")) {
      obj.AddMember("updated_at", rapidjson::Value(rs.getString("updated_at").c_str(), alloc), alloc);
    }
    if (!rs.isNull("updated_by")) {
      obj.AddMember("updated_by", rapidjson::Value(rs.getString("updated_by").c_str(), alloc), alloc);
    }
    return obj;
}

} // namespace

class FieldService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

public:
    FieldService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker))
    {
        identity.machineName   = machineName;
        identity.serviceName   = "field_service";
        identity.serviceId     = serviceId;
        identity.version       = "v1.0.0";
        identity.environment   = "prod";
        identity.maxConcurrent = 20;
        identity.capabilities  = {"field.list", "field.get", "field.create", "field.update", "field.delete"};
    }

    bool initialize()
    {
        client = std::make_unique<ServiceClient>(identity, gatewayAddress);
        client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
            return processRequest(req);
        });
        return true;
    }

    void run()
    {
        running.store(true);
        std::cout << "[" << identity.serviceId << "] FieldService starting\n";
        client->run();
        std::cout << "[" << identity.serviceId << "] FieldService stopped\n";
    }

    void shutdown()
    {
        running.store(false);
        if (client) client->stop();
    }

private:
    [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) const
    {
        const std::string cap = request.HasMember("capability") && request["capability"].IsString()
                                    ? request["capability"].GetString() : "";
        std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

        try {
            PostgreSQLDatabase db;
            db.connect();

            if (cap == "field.list") {
              return handleList(request, db);
            }
            if (cap == "field.get") {
              return handleGet(request, db);
            }
            if (cap == "field.create") {
              return handleCreate(request, db);
            }
            if (cap == "field.update") {
              return handleUpdate(request, db);
            }
            if (cap == "field.delete") {
              return handleDelete(request, db);
            }

            return makeError("Unknown capability: " + cap, 404);
        } catch (const std::exception& e) {
            std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
            return makeError(std::string("Internal error: ") + e.what(), 500);
        }
    }

    static rapidjson::Document handleList(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string farmId = getQueryParam(req, "farm_id");

        std::unique_ptr<IResultSet> rs;
        if (farmId.empty()) {
            rs = db.execQuery(
                "SELECT id, farm_id, name, area, ST_AsText(geometry) AS geometry, "
                "created_at, updated_at, updated_by FROM fields ORDER BY id");
        } else {
            rs = db.execQuery(
                "SELECT id, farm_id, name, area, ST_AsText(geometry) AS geometry, "
                "created_at, updated_at, updated_by FROM fields WHERE farm_id = $1 ORDER BY id",
                {farmId});
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        while (rs->next()) {
          arr.PushBack(fieldFromRow(*rs, alloc), alloc);
        }
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", arr, alloc);
        doc.AddMember("total", static_cast<int>(arr.Size()), alloc);
        return doc;
    }

    static rapidjson::Document handleGet(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) {
          return makeError("Missing path parameter: id");
        }

        const auto rs = db.execQuery(
            "SELECT id, farm_id, name, area, ST_AsText(geometry) AS geometry, "
            "created_at, updated_at, updated_by FROM fields WHERE id = $1",
            {id});

        if (!rs->next()) {
          return makeError("Field not found", 404);
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", fieldFromRow(*rs, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, IDatabase& db)
    {
      std::string farmId;
      std::string name;
      if (req.HasMember("farm_id") && req["farm_id"].IsString()) {
        farmId = req["farm_id"].GetString();
      }
      if (req.HasMember("name") && req["name"].IsString()){
        name = req["name"].GetString();
      }

      if (farmId.empty()) {
        return makeError("Missing field: farm_id");
      }
      if (name.empty()) {
        return makeError("Missing field: name");
      }

      std::string area;
      if (req.HasMember("area")) {
          if (req["area"].IsNumber()) {
            area = std::to_string(req["area"].GetDouble());
          }
          else if (req["area"].IsString()) {
            area = req["area"].GetString();
          }
      }

      const auto rs = area.empty() ?
        db.execQuery("INSERT INTO fields (farm_id, name) VALUES ($1, $2) RETURNING id",{farmId, name})
        : db.execQuery("INSERT INTO fields (farm_id, name, area) VALUES ($1, $2, $3) RETURNING id",{farmId, name, area});

      rapidjson::Document doc;
      doc.SetObject();
      auto& alloc = doc.GetAllocator();
      if (!rs->next()) {
        return makeError("Failed to create field", 500);
      }

      doc.AddMember("status", "success", alloc);
      doc.AddMember("statusCode", 201, alloc);
      rapidjson::Value data(rapidjson::kObjectType);
      data.AddMember("id", rapidjson::Value(rs->getString("id").c_str(), alloc), alloc);
      doc.AddMember("data", data, alloc);
      return doc;
    }

    static rapidjson::Document handleUpdate(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) {
          return makeError("Missing path parameter: id");
        }

        std::string name;
        if (req.HasMember("name") && req["name"].IsString()) {
          name = req["name"].GetString();
        }
        if (name.empty()) {
          return makeError("Missing field: name");
        }

        const bool ok =
          db.execCommand("UPDATE fields SET name=$1, updated_at=now() WHERE id=$2", {name, id});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 200 : 500, alloc);
        return doc;
    }

    static rapidjson::Document handleDelete(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) {
          return makeError("Missing path parameter: id");
        }

        const bool ok = db.execCommand("DELETE FROM fields WHERE id = $1", {id});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 204 : 500, alloc);
        return doc;
    }
};

static FieldService* gService = nullptr;

void signalHandler(const int sig)
{
    if (gService && (sig == SIGTERM || sig == SIGINT))
        gService->shutdown();
}

int main(const int argc, char* argv[])
{
    std::string serviceId      = "field_001";
    std::string machineName    = "localhost";
    std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

    if (argc >= 4) {
        serviceId      = argv[1];
        machineName    = argv[2];
        gatewayAddress = argv[3];
    } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
        serviceId   = "field_dev";
        machineName = "dev-machine";
    }

    FieldService service(serviceId, machineName, gatewayAddress);
    gService = &service;
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);

    if (!service.initialize()) {
        std::cerr << "Failed to initialize FieldService\n";
        return 1;
    }
    service.run();
    return 0;
}
