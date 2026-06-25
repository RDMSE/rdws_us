//
// FieldService — capabilities: field.list, field.get, field.create, field.update, field.delete
//

#include "../../shared/repository/FieldRepository.h"
#include "../../shared/service/FieldService.h"
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
using namespace rdws::field;

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

rapidjson::Value fieldToJson(const Field& f, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id",       rapidjson::Value(f.id.c_str(), alloc),       alloc);
    obj.AddMember("farm_id",  rapidjson::Value(f.farmId.c_str(), alloc),   alloc);
    obj.AddMember("name",     rapidjson::Value(f.name.c_str(), alloc),     alloc);
    if (!f.area.empty())
        obj.AddMember("area", rapidjson::Value(f.area.c_str(), alloc), alloc);
    if (!f.geometry.empty())
        obj.AddMember("geometry", rapidjson::Value(f.geometry.c_str(), alloc), alloc);
    obj.AddMember("created_at", rapidjson::Value(f.createdAt.c_str(), alloc), alloc);
    if (!f.updatedAt.empty())
        obj.AddMember("updated_at", rapidjson::Value(f.updatedAt.c_str(), alloc), alloc);
    if (!f.updatedBy.empty())
        obj.AddMember("updated_by", rapidjson::Value(f.updatedBy.c_str(), alloc), alloc);
    return obj;
}

} // namespace

class AppFieldService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

    // DB/repo/svc — declared in dependency order
    PostgreSQLDatabase          db_;
    FieldRepository             repo_;
    rdws::field::FieldService   svc_;

public:
    AppFieldService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_)
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
        std::cout << "[" << identity.serviceId << "] FieldService stopped\n";
    }

    void shutdown()
    {
        running.store(false);
        if (client) client->stop();
    }

private:
    [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request)
    {
        const std::string cap = request.HasMember("capability") && request["capability"].IsString()
                                    ? request["capability"].GetString() : "";
        std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

        try {
            if (cap == "field.list")   return handleList(request, svc_);
            if (cap == "field.get")    return handleGet(request, svc_);
            if (cap == "field.create") return handleCreate(request, svc_);
            if (cap == "field.update") return handleUpdate(request, svc_);
            if (cap == "field.delete") return handleDelete(request, svc_);

            return makeError("Unknown capability: " + cap, 404);
        } catch (const std::exception& e) {
            std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
            return makeError(std::string("Internal error: ") + e.what(), 500);
        }
    }

    static rapidjson::Document handleList(const rapidjson::Document& req, rdws::field::FieldService& svc)
    {
        const std::string farmId = getQueryParam(req, "farm_id");
        const auto fields = svc.findAll(farmId);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        for (const auto& f : fields)
            arr.PushBack(fieldToJson(f, alloc), alloc);
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        const int total = static_cast<int>(arr.Size());
        doc.AddMember("data", arr, alloc);
        doc.AddMember("total", total, alloc);
        return doc;
    }

    static rapidjson::Document handleGet(const rapidjson::Document& req, rdws::field::FieldService& svc)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) return makeError("Missing path parameter: id");

        const auto field = svc.findById(id);
        if (!field) return makeError("Field not found", 404);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", fieldToJson(*field, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, rdws::field::FieldService& svc)
    {
        std::string farmId;
        std::string name;
        if (req.HasMember("farm_id") && req["farm_id"].IsString())
            farmId = req["farm_id"].GetString();
        if (req.HasMember("name") && req["name"].IsString())
            name = req["name"].GetString();

        if (farmId.empty()) return makeError("Missing field: farm_id");
        if (name.empty())   return makeError("Missing field: name");

        FieldCreate data;
        data.farmId = farmId;
        data.name   = name;
        if (req.HasMember("area")) {
            if (req["area"].IsNumber())
                data.area = std::to_string(req["area"].GetDouble());
            else if (req["area"].IsString())
                data.area = req["area"].GetString();
        }

        const std::string id = svc.create(data);
        if (id.empty()) return makeError("Failed to create field", 500);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 201, alloc);
        rapidjson::Value dataObj(rapidjson::kObjectType);
        dataObj.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
        doc.AddMember("data", dataObj, alloc);
        return doc;
    }

    static rapidjson::Document handleUpdate(const rapidjson::Document& req, rdws::field::FieldService& svc)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) return makeError("Missing path parameter: id");

        std::string name;
        if (req.HasMember("name") && req["name"].IsString())
            name = req["name"].GetString();
        if (name.empty()) return makeError("Missing field: name");

        const bool ok = svc.update(id, {name});
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 200 : 500, alloc);
        return doc;
    }

    static rapidjson::Document handleDelete(const rapidjson::Document& req, rdws::field::FieldService& svc)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) return makeError("Missing path parameter: id");

        const bool ok = svc.remove(id);
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 204 : 500, alloc);
        return doc;
    }
};

static AppFieldService* gService = nullptr;

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

    AppFieldService service(serviceId, machineName, gatewayAddress);
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
