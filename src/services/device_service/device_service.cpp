//
// DeviceService — capabilities: device.list, device.get, device.create, device.update, device.delete
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
        if (pp.HasMember(key.c_str()) && pp[key.c_str()].IsString())
            return pp[key.c_str()].GetString();
    }
    return {};
}

std::string getQueryParam(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember("queryStringParameters") && req["queryStringParameters"].IsObject()) {
        const auto& qp = req["queryStringParameters"];
        if (qp.HasMember(key.c_str()) && qp[key.c_str()].IsString())
            return qp[key.c_str()].GetString();
    }
    return {};
}

rapidjson::Value deviceFromRow(IResultSet& rs, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id",       rapidjson::Value(rs.getString("id").c_str(), alloc),       alloc);
    obj.AddMember("field_id", rapidjson::Value(rs.getString("field_id").c_str(), alloc), alloc);
    obj.AddMember("type",     rapidjson::Value(rs.getString("type").c_str(), alloc),     alloc);
    obj.AddMember("status",   rapidjson::Value(rs.getString("status").c_str(), alloc),   alloc);
    if (!rs.isNull("installation_date"))
        obj.AddMember("installation_date", rapidjson::Value(rs.getString("installation_date").c_str(), alloc), alloc);
    if (!rs.isNull("location"))
        obj.AddMember("location", rapidjson::Value(rs.getString("location").c_str(), alloc), alloc);
    obj.AddMember("created_at", rapidjson::Value(rs.getString("created_at").c_str(), alloc), alloc);
    if (!rs.isNull("updated_at"))
        obj.AddMember("updated_at", rapidjson::Value(rs.getString("updated_at").c_str(), alloc), alloc);
    if (!rs.isNull("updated_by"))
        obj.AddMember("updated_by", rapidjson::Value(rs.getString("updated_by").c_str(), alloc), alloc);
    return obj;
}

std::string getStr(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember(key.c_str()) && req[key.c_str()].IsString())
        return req[key.c_str()].GetString();
    return {};
}

} // namespace

class DeviceService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

public:
    DeviceService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker))
    {
        identity.machineName   = machineName;
        identity.serviceName   = "device_service";
        identity.serviceId     = serviceId;
        identity.version       = "v1.0.0";
        identity.environment   = "prod";
        identity.maxConcurrent = 20;
        identity.capabilities  = {"device.list", "device.get", "device.create", "device.update", "device.delete"};
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
        std::cout << "[" << identity.serviceId << "] DeviceService starting\n";
        client->run();
        std::cout << "[" << identity.serviceId << "] DeviceService stopped\n";
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

            if (cap == "device.list")   return handleList(request, db);
            if (cap == "device.get")    return handleGet(request, db);
            if (cap == "device.create") return handleCreate(request, db);
            if (cap == "device.update") return handleUpdate(request, db);
            if (cap == "device.delete") return handleDelete(request, db);

            return makeError("Unknown capability: " + cap, 404);
        } catch (const std::exception& e) {
            std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
            return makeError(std::string("Internal error: ") + e.what(), 500);
        }
    }

    static rapidjson::Document handleList(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string fieldId = getQueryParam(req, "field_id");

        std::unique_ptr<IResultSet> rs;
        if (fieldId.empty()) {
            rs = db.execQuery(
                "SELECT id, field_id, type, status, installation_date, "
                "ST_AsText(location) AS location, created_at, updated_at, updated_by "
                "FROM devices ORDER BY id");
        } else {
            rs = db.execQuery(
                "SELECT id, field_id, type, status, installation_date, "
                "ST_AsText(location) AS location, created_at, updated_at, updated_by "
                "FROM devices WHERE field_id = $1 ORDER BY id",
                {fieldId});
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        while (rs->next())
            arr.PushBack(deviceFromRow(*rs, alloc), alloc);

        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", arr, alloc);
        doc.AddMember("total", static_cast<int>(arr.Size()), alloc);
        return doc;
    }

    static rapidjson::Document handleGet(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) return makeError("Missing path parameter: id");

        auto rs = db.execQuery(
            "SELECT id, field_id, type, status, installation_date, "
            "ST_AsText(location) AS location, created_at, updated_at, updated_by "
            "FROM devices WHERE id = $1",
            {id});

        if (!rs->next()) return makeError("Device not found", 404);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", deviceFromRow(*rs, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string fieldId = getStr(req, "field_id");
        const std::string type    = getStr(req, "type");
        const std::string status  = getStr(req, "status");

        if (fieldId.empty()) return makeError("Missing field: field_id");
        if (type.empty())    return makeError("Missing field: type");

        const std::string deviceStatus = status.empty() ? "active" : status;

        auto rs = db.execQuery(
            "INSERT INTO devices (field_id, type, status) VALUES ($1, $2, $3) RETURNING id",
            {fieldId, type, deviceStatus});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        if (!rs->next()) return makeError("Failed to create device", 500);

        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 201, alloc);
        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("id", rapidjson::Value(rs->getString("id").c_str(), alloc), alloc);
        doc.AddMember("data", data, alloc);
        return doc;
    }

    static rapidjson::Document handleUpdate(const rapidjson::Document& req, IDatabase& db)
    {
        const std::string id     = getPathParam(req, "id");
        const std::string type   = getStr(req, "type");
        const std::string status = getStr(req, "status");

        if (id.empty())     return makeError("Missing path parameter: id");
        if (type.empty())   return makeError("Missing field: type");
        if (status.empty()) return makeError("Missing field: status");

        const bool ok = db.execCommand(
            "UPDATE devices SET type=$1, status=$2, updated_at=now() WHERE id=$3",
            {type, status, id});

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
        if (id.empty()) return makeError("Missing path parameter: id");

        const bool ok = db.execCommand("DELETE FROM devices WHERE id = $1", {id});

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 204 : 500, alloc);
        return doc;
    }
};

static DeviceService* gService = nullptr;

void signalHandler(int sig)
{
    if (gService && (sig == SIGTERM || sig == SIGINT))
        gService->shutdown();
}

int main(int argc, char* argv[])
{
    std::string serviceId      = "device_001";
    std::string machineName    = "localhost";
    std::string gatewayAddress = "unix:///tmp/service_gateway.sock";

    if (argc >= 4) {
        serviceId      = argv[1];
        machineName    = argv[2];
        gatewayAddress = argv[3];
    } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
        serviceId   = "device_dev";
        machineName = "dev-machine";
    }

    DeviceService service(serviceId, machineName, gatewayAddress);
    gService = &service;
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);

    if (!service.initialize()) {
        std::cerr << "Failed to initialize DeviceService\n";
        return 1;
    }
    service.run();
    return 0;
}
