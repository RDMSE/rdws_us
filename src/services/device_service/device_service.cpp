//
// DeviceService — capabilities: device.list, device.get, device.create, device.update, device.delete
//

#include "../../shared/repository/DeviceRepository.h"
#include "../../shared/service/DeviceService.h"
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
using namespace rdws::device;

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

std::string getStr(const rapidjson::Document& req, const std::string& key)
{
    if (req.HasMember(key.c_str()) && req[key.c_str()].IsString())
        return req[key.c_str()].GetString();
    return {};
}

rapidjson::Value deviceToJson(const Device& d, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id",       rapidjson::Value(d.id.c_str(), alloc),       alloc);
    obj.AddMember("field_id", rapidjson::Value(d.fieldId.c_str(), alloc),  alloc);
    obj.AddMember("type",     rapidjson::Value(d.type.c_str(), alloc),     alloc);
    obj.AddMember("status",   rapidjson::Value(d.status.c_str(), alloc),   alloc);
    if (!d.installationDate.empty()) {
        obj.AddMember("installation_date", rapidjson::Value(d.installationDate.c_str(), alloc), alloc);
    }
    if (!d.location.empty()) {
        obj.AddMember("location", rapidjson::Value(d.location.c_str(), alloc), alloc);
    }
    obj.AddMember("created_at", rapidjson::Value(d.createdAt.c_str(), alloc), alloc);
    if (!d.updatedAt.empty()) {
        obj.AddMember("updated_at", rapidjson::Value(d.updatedAt.c_str(), alloc), alloc);
    }
    if (!d.updatedBy.empty()) {
        obj.AddMember("updated_by", rapidjson::Value(d.updatedBy.c_str(), alloc), alloc);
    }
    return obj;
}

} // namespace

class AppDeviceService
{
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string gatewayAddress;
    std::atomic<bool> running{false};

    // DB/repo/svc — declared in dependency order
    PostgreSQLDatabase            db_;
    DeviceRepository              repo_;
    rdws::device::DeviceService   svc_;

public:
    AppDeviceService(const std::string& serviceId, const std::string& machineName, std::string broker)
        : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_)
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
        std::cout << "[" << identity.serviceId << "] DeviceService stopped\n";
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
            if (cap == "device.list")   return handleList(request, svc_);
            if (cap == "device.get")    return handleGet(request, svc_);
            if (cap == "device.create") return handleCreate(request, svc_);
            if (cap == "device.update") return handleUpdate(request, svc_);
            if (cap == "device.delete") return handleDelete(request, svc_);

            return makeError("Unknown capability: " + cap, 404);
        } catch (const std::exception& e) {
            std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
            return makeError(std::string("Internal error: ") + e.what(), 500);
        }
    }

    static rapidjson::Document handleList(const rapidjson::Document& req, rdws::device::DeviceService& svc)
    {
        const std::string fieldId = getQueryParam(req, "field_id");
        const auto devices = svc.findAll(fieldId);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        rapidjson::Value arr(rapidjson::kArrayType);
        for (const auto& d : devices)
            arr.PushBack(deviceToJson(d, alloc), alloc);
        const int total = static_cast<int>(arr.Size());

        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", arr, alloc);
        doc.AddMember("total", total, alloc);
        return doc;
    }

    static rapidjson::Document handleGet(const rapidjson::Document& req, rdws::device::DeviceService& svc)
    {
        const std::string id = getPathParam(req, "id");
        if (id.empty()) return makeError("Missing path parameter: id");

        const auto device = svc.findById(id);
        if (!device) return makeError("Device not found", 404);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 200, alloc);
        doc.AddMember("data", deviceToJson(*device, alloc), alloc);
        return doc;
    }

    static rapidjson::Document handleCreate(const rapidjson::Document& req, rdws::device::DeviceService& svc)
    {
        const std::string fieldId = getStr(req, "field_id");
        const std::string type    = getStr(req, "type");
        const std::string status  = getStr(req, "status");

        if (fieldId.empty()) return makeError("Missing field: field_id");
        if (type.empty())    return makeError("Missing field: type");

        const std::string id = svc.create({fieldId, type, status});
        if (id.empty()) return makeError("Failed to create device", 500);

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", "success", alloc);
        doc.AddMember("statusCode", 201, alloc);
        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
        doc.AddMember("data", data, alloc);
        return doc;
    }

    static rapidjson::Document handleUpdate(const rapidjson::Document& req, rdws::device::DeviceService& svc)
    {
        const std::string id     = getPathParam(req, "id");
        const std::string type   = getStr(req, "type");
        const std::string status = getStr(req, "status");

        if (id.empty())     return makeError("Missing path parameter: id");
        if (type.empty())   return makeError("Missing field: type");
        if (status.empty()) return makeError("Missing field: status");

        const bool ok = svc.update(id, {type, status});
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("status", ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc), alloc);
        doc.AddMember("statusCode", ok ? 200 : 500, alloc);
        return doc;
    }

    static rapidjson::Document handleDelete(const rapidjson::Document& req, rdws::device::DeviceService& svc)
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

static AppDeviceService* gService = nullptr;

void signalHandler(int sig)
{
    if (gService && (sig == SIGTERM || sig == SIGINT))
        gService->shutdown();
}

int main(int argc, char* argv[])
{
    std::string serviceId      = "device_001";
    std::string machineName    = "localhost";
    std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

    if (argc >= 4) {
        serviceId      = argv[1];
        machineName    = argv[2];
        gatewayAddress = argv[3];
    } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
        serviceId   = "device_dev";
        machineName = "dev-machine";
    }

    AppDeviceService service(serviceId, machineName, gatewayAddress);
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
