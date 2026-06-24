//
// SensorService — capabilities: sensor.list, sensor.get, sensor.create, sensor.update, sensor.delete
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

rapidjson::Document makeError(const std::string& msg, const int code = 400)
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

std::string getStr(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember(key.c_str()) && req[key.c_str()].IsString()) {
      return req[key.c_str()].GetString();
    }
    return {};
}

rapidjson::Value sensorFromRow(IResultSet& rs, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id",        rapidjson::Value(rs.getString("id").c_str(), alloc),        alloc);
    obj.AddMember("device_id", rapidjson::Value(rs.getString("device_id").c_str(), alloc), alloc);
    obj.AddMember("type",      rapidjson::Value(rs.getString("type").c_str(), alloc),      alloc);
    obj.AddMember("unit",      rapidjson::Value(rs.getString("unit").c_str(), alloc),      alloc);
    if (!rs.isNull("location")) {
      obj.AddMember("location", rapidjson::Value(rs.getString("location").c_str(), alloc), alloc);
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

class SensorService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

public:
    SensorService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker))
    {
        identity.machineName   = machineName;
        identity.serviceName   = "sensor_service";
        identity.serviceId     = serviceId;
        identity.version       = "v1.0.0";
        identity.environment   = "prod";
        identity.maxConcurrent = 20;
        identity.capabilities  = {"sensor.list", "sensor.get", "sensor.create", "sensor.update", "sensor.delete"};
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
        std::cout << "[" << identity.serviceId << "] SensorService starting\n";
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
        std::cout << "[" << identity.serviceId << "] SensorService stopped\n";
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

            if (cap == "sensor.list") {
              return handleList(request, db);
            }
            if (cap == "sensor.get") {
              return handleGet(request, db);
            }
            if (cap == "sensor.create") {
              return handleCreate(request, db);
            }
            if (cap == "sensor.update") {
              return handleUpdate(request, db);
            }
            if (cap == "sensor.delete") {
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
        const std::string deviceId = getQueryParam(req, "device_id");

        const auto rs = deviceId.empty() ?
          db.execQuery("SELECT id, device_id, type, unit, ST_AsText(location) AS location, "
            "created_at, updated_at, updated_by FROM sensors ORDER BY id")
          : db.execQuery("SELECT id, device_id, type, unit, ST_AsText(location) AS location, "
            "created_at, updated_at, updated_by FROM sensors WHERE device_id = $1 ORDER BY id",
            {deviceId});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        while (rs->next()) {
          arr.PushBack(sensorFromRow(*rs, alloc), alloc);
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

        auto rs = db.execQuery(
            "SELECT id, device_id, type, unit, ST_AsText(location) AS location, "
            "created_at, updated_at, updated_by FROM sensors WHERE id = $1",
            {id});

        if (!rs->next()) {
          return makeError("Sensor not found", 404);
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", sensorFromRow(*rs, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string deviceId = getStr(req, "device_id");
        const std::string type     = getStr(req, "type");
        const std::string unit     = getStr(req, "unit");

        if (deviceId.empty()) {
          return makeError("Missing field: device_id");
        }
        if (type.empty()) {
          return makeError("Missing field: type");
        }
        if (unit.empty()) {
          return makeError("Missing field: unit");
        }

        auto rs = db.execQuery(
            "INSERT INTO sensors (device_id, type, unit) VALUES ($1, $2, $3) RETURNING id",
            {deviceId, type, unit});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        if (!rs->next()) {
          return makeError("Failed to create sensor", 500);
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
        const std::string id   = getPathParam(req, "id");
        const std::string type = getStr(req, "type");
        const std::string unit = getStr(req, "unit");

        if (id.empty()) {
          return makeError("Missing path parameter: id");
        }
        if (type.empty()) {
          return makeError("Missing field: type");
        }
        if (unit.empty()) {
          return makeError("Missing field: unit");
        }

        const bool ok = db.execCommand(
            "UPDATE sensors SET type=$1, unit=$2, updated_at=now() WHERE id=$3",
            {type, unit, id});

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

        const bool ok = db.execCommand("DELETE FROM sensors WHERE id = $1", {id});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 204 : 500, alloc);
        return doc;
    }
};

static SensorService* gService = nullptr;

void signalHandler(const int sig)
{
    if (gService && (sig == SIGTERM || sig == SIGINT))
        gService->shutdown();
}

int main(const int argc, char* argv[])
{
    std::string serviceId      = "sensor_001";
    std::string machineName    = "localhost";
    std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

    if (argc >= 4) {
        serviceId      = argv[1];
        machineName    = argv[2];
        gatewayAddress = argv[3];
    } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
        serviceId   = "sensor_dev";
        machineName = "dev-machine";
    }

    SensorService service(serviceId, machineName, gatewayAddress);
    gService = &service;
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);

    if (!service.initialize()) {
        std::cerr << "Failed to initialize SensorService\n";
        return 1;
    }
    service.run();
    return 0;
}
