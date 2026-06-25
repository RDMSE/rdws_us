//
// FarmService — capabilities: farm.list, farm.get, farm.create, farm.update, farm.delete
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
        const auto& pp = req["pathParameters"];
        if (pp.HasMember(key.c_str()) && pp[key.c_str()].IsString()) {
          return pp[key.c_str()].GetString();
        }
    }
    return {};
}

std::string getQueryParam(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember("queryStringParameters") && req["queryStringParameters"].IsObject()) {
        const auto& qp = req["queryStringParameters"];
        if (qp.HasMember(key.c_str()) && qp[key.c_str()].IsString()) {
          return qp[key.c_str()].GetString();
        }
    }
    return {};
}

// Build a farm JSON object from the current row of a result set
rapidjson::Value farmFromRow(IResultSet& rs, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id",         rapidjson::Value(rs.getString("id").c_str(), alloc),         alloc);
  obj.AddMember("name",       rapidjson::Value(rs.getString("name").c_str(), alloc),       alloc);
  obj.AddMember("location",   rapidjson::Value(rs.isNull("location") ? "" : rs.getString("location").c_str(), alloc), alloc);
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

class FarmService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

public:
    FarmService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker))
    {
        identity.machineName   = machineName;
        identity.serviceName   = "farm_service";
        identity.serviceId     = serviceId;
        identity.version       = "v1.0.0";
        identity.environment   = "prod";
        identity.maxConcurrent = 20;
        identity.capabilities  = {"farm.list", "farm.get", "farm.create", "farm.update", "farm.delete"};
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
        std::cout << "[" << identity.serviceId << "] FarmService starting\n";
        while (running.load()) {
            client->run();
            if (!running.load()) break;
            std::cerr << "[" << identity.serviceId << "] Reconnecting in 3s...\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            client = std::make_unique<ServiceClient>(identity, gatewayAddress);
            client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
                return processRequest(req);
            });
        }
        std::cout << "[" << identity.serviceId << "] FarmService stopped\n";
    }

    void shutdown()
    {
        running.store(false);
        if (client) {
          client->stop();
        }
    }

private:
    [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) const
    {
        const std::string cap = request.HasMember("capability") && request["capability"].IsString()
                                    ? request["capability"].GetString() : "";
        std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

        try {
            PostgreSQLDatabase db;

            if (cap == "farm.list") {
              return handleList(request, db);
            }
            if (cap == "farm.get") {
              return handleGet(request, db);
            }
            if (cap == "farm.create") {
              return handleCreate(request, db);
            }
            if (cap == "farm.update") {
              return handleUpdate(request, db);
            }
            if (cap == "farm.delete") {
              return handleDelete(request, db);
            }

            return makeError("Unknown capability: " + cap, 404);
        } catch (const std::exception& e) {
            std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
            return makeError(std::string("Internal error: ") + e.what(), 500);
        }
    }

    static rapidjson::Document handleList(const rapidjson::Document& /*req*/, IDatabase& db)
    {
        auto rs = db.execQuery(
            "SELECT id, name, ST_AsText(location) AS location, created_at, updated_at, updated_by "
            "FROM farms ORDER BY id");

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        while (rs->next()) {
          arr.PushBack(farmFromRow(*rs, alloc), alloc);
        }

        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        const int total = static_cast<int>(arr.Size());
        doc.AddMember("data", arr, alloc);
        doc.AddMember("total", total, alloc);
        return doc;
    }

    static rapidjson::Document handleGet(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) {
          return makeError("Missing path parameter: id");
        }

        const auto rs = db.execQuery(
            "SELECT id, name, ST_AsText(location) AS location, created_at, updated_at, updated_by "
            "FROM farms WHERE id = $1",
            {id});

        if (!rs->next()) {
          return makeError("Farm not found", 404);
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", farmFromRow(*rs, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, IDatabase& db)
    {
        std::string name;
        if (req.HasMember("name") && req["name"].IsString()) {
          name = req["name"].GetString();
        }
        if (name.empty()) {
          return makeError("Missing field: name");
        }
        std::string locationWkt;
        if (req.HasMember("location") && req["location"].IsObject()) {
          if (const auto& loc = req["location"]; loc.HasMember("lat") && loc.HasMember("lng")) {
            const double lat = loc["lat"].GetDouble();
            const double lng = loc["lng"].GetDouble();
            locationWkt = "POINT(" + std::to_string(lng) + " " + std::to_string(lat) + ")";
          }
        }

        const std::string sql = locationWkt.empty()
            ? "INSERT INTO farms (name) VALUES ($1) RETURNING id"
            : "INSERT INTO farms (name, location) VALUES ($1, ST_SetSRID(ST_GeomFromText($2),4326)) RETURNING id";

        const std::vector<std::string> params = locationWkt.empty()
            ? std::vector{name}
            : std::vector{name, locationWkt};

        auto rs = db.execQuery(sql, params);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        if (!rs->next()) {
          return makeError("Failed to create farm", 500);
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
        std::string locationWkt;
        if (req.HasMember("location") && req["location"].IsObject()) {
          if (const auto& loc = req["location"]; loc.HasMember("lat") && loc.HasMember("lng")) {
            const double lat = loc["lat"].GetDouble();
            const double lng = loc["lng"].GetDouble();
            locationWkt = "POINT(" + std::to_string(lng) + " " + std::to_string(lat) + ")";
          }
        }

        const std::string sql = locationWkt.empty()
            ? "UPDATE farms SET name=$1, updated_at=now() WHERE id=$2"
            : "UPDATE farms SET name=$1, location=ST_SetSRID(ST_GeomFromText($2),4326), updated_at=now() WHERE id=$3";

        const std::vector<std::string> params = locationWkt.empty()
            ? std::vector<std::string>{name, id}
            : std::vector<std::string>{name, locationWkt, id};

        const bool ok = db.execCommand(sql, params);

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
        const bool ok = db.execCommand("DELETE FROM farms WHERE id = $1", {id});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 204 : 500, alloc);
        return doc;
    }
};

static FarmService* gService = nullptr;

void signalHandler(int sig)
{
    if (gService && (sig == SIGTERM || sig == SIGINT))
        gService->shutdown();
}

int main(int argc, char* argv[])
{
    std::string serviceId      = "farm_001";
    std::string machineName    = "localhost";
    std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

    if (argc >= 4) {
        serviceId      = argv[1];
        machineName    = argv[2];
        gatewayAddress = argv[3];
    } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
        serviceId   = "farm_dev";
        machineName = "dev-machine";
    }

    FarmService service(serviceId, machineName, gatewayAddress);
    gService = &service;
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);

    if (!service.initialize()) {
        std::cerr << "Failed to initialize FarmService\n";
        return 1;
    }
    service.run();
    return 0;
}
