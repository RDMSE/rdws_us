#include "../Services/ServiceGateway.h"
#include "../Services/ServiceMonitor.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>

using namespace servicegateway;

// Global variables for signal handling
ServiceGateway* g_gateway = nullptr;
bool g_running = true;

void signalHandler(const int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << '\n';
    g_running = false;
    if (g_gateway != nullptr) {
        g_gateway->stop();
    }
}

int main() {
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << "=== Service Gateway with Monitoring Interface ===" << '\n';
    
    // Create and start gateway
    ServiceGateway gateway(8080, "/tmp/example_service_gateway.sock");
    g_gateway = &gateway;
    
    if (!gateway.start()) {
        std::cerr << "Failed to start gateway" << '\n';
        return 1;
    }
    
    // Create monitor
    ServiceMonitor monitor(gateway);
    
    std::cout << "\nGateway started with monitoring interface!" << '\n';
    std::cout << "Services can connect via:" << '\n';
    std::cout << "  TCP: localhost:8080" << '\n';
    std::cout << "  UNIX: /tmp/example_service_gateway.sock" << '\n';
    std::cout << "\nMonitoring interface commands:" << '\n';
    std::cout << "  s - Show current status" << '\n';
    std::cout << "  c - Start continuous monitoring" << '\n';
    std::cout << "  h - Show health status" << '\n';
    std::cout << "  f - Save status to file" << '\n';
    std::cout << "  q - Quit" << '\n';
    std::cout << "  help - Show this help" << '\n';
    
    // Interactive command loop
    std::string command;
    while (g_running && gateway.isRunning()) {
        std::cout << "\ngateway> ";
        std::getline(std::cin, command);
        
        if (command == "q" || command == "quit") {
            g_running = false;
            break;
        } if (command == "s" || command == "status") {
            monitor.displayStatus();
        } else if (command == "c" || command == "continuous") {
            std::cout << "Starting continuous monitoring (Press Ctrl+C to stop)..." << '\n';
            std::thread monitorThread([&monitor]() {
                monitor.displayContinuous(5);
            });
            
            // Wait for interrupt
            while (g_running && gateway.isRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            monitorThread.detach(); // Will stop when broker stops
        } else if (command == "h" || command == "health") {
            monitor.showHealthStatus();
        } else if (command == "f" || command == "file") {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::ostringstream filename;
            filename << "broker_status_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << ".txt";
            
            monitor.saveStatusToFile(filename.str());
        } else if (command.starts_with("cap ")) {
            std::string capability = command.substr(4);
            monitor.showServicesByCapability(capability);
        } else if (command.starts_with("machine ")) {
            std::string machine = command.substr(8);
            monitor.showServicesByMachine(machine);
        } else if (command == "help" || command == "?") {
            std::cout << "\nCommands:" << '\n';
            std::cout << "  s, status      - Show current gateway/service status" << '\n';
            std::cout << "  c, continuous  - Start continuous monitoring display" << '\n';
            std::cout << "  h, health      - Show service health status" << '\n';
            std::cout << "  f, file        - Save status to timestamped file" << '\n';
            std::cout << "  cap <name>     - Show services with capability" << '\n';
            std::cout << "  machine <name> - Show services on machine" << '\n';
            std::cout << "  q, quit        - Quit gateway" << '\n';
            std::cout << "  help, ?        - Show this help" << '\n';
        } else if (!command.empty()) {
            std::cout << "Unknown command: " << command << '\n';
            std::cout << "Type 'help' for available commands" << '\n';
        }
    }
    
    std::cout << "\nStopping gateway..." << '\n';
    gateway.stop();
    
    std::cout << "Gateway stopped." << '\n';
    return 0;
}