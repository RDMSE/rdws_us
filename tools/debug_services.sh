#!/bin/bash

# Service Debug Helper Script
# Helps launch services in development mode and connect test harness

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

PROJECT_DIR=$(dirname "$(dirname "$(realpath "$0")")")
BUILD_DIR="$PROJECT_DIR/build"
SERVICES_DIR="$BUILD_DIR/services"
BIN_DIR="$BUILD_DIR/bin"

print_usage() {
    echo "Usage: $0 [command] [options]"
    echo ""
    echo "Commands:"
    echo "  build                - Build the project"
    echo "  run <service>        - Run service in development mode"
    echo "  debug <service>      - Show debug instructions for service"
    echo "  list                 - List available services"
    echo "  clean                - Clean socket files"
    echo ""
    echo "Examples:"
    echo "  $0 build"
    echo "  $0 run example_service"
    echo "  $0 debug example_service"
    echo ""
    echo "Note: Use service_broker_monitor for service testing/monitoring"
}

check_build() {
    if [ ! -d "$BUILD_DIR" ]; then
        echo -e "${RED}Build directory not found. Run: $0 build${NC}"
        exit 1
    fi
}

build_project() {
    echo -e "${BLUE}Building project...${NC}"
    cd "$PROJECT_DIR"
    
    if [ ! -d "$BUILD_DIR" ]; then
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    fi
    
    cmake --build build
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Build successful!${NC}"
    else
        echo -e "${RED}Build failed!${NC}"
        exit 1
    fi
}

list_services() {
    echo -e "${BLUE}Available services:${NC}"
    if [ -d "$SERVICES_DIR" ]; then
        for service in "$SERVICES_DIR"/*; do
            if [ -x "$service" ]; then
                basename "$service"
            fi
        done
    else
        echo "No services found. Run 'build' first."
    fi
}

run_service_dev() {
    local service_name="$1"
    local service_path="$SERVICES_DIR/$service_name"
    
    check_build
    
    if [ ! -x "$service_path" ]; then
        echo -e "${RED}Service '$service_name' not found at $service_path${NC}"
        echo -e "${YELLOW}Available services:${NC}"
        list_services
        exit 1
    fi
    
    echo -e "${GREEN}Starting $service_name in development mode...${NC}"
    echo -e "${YELLOW}Press Ctrl+C to stop the service${NC}"
    echo ""
    
    cd "$PROJECT_DIR"
    "$service_path" --dev
}

show_debug_instructions() {
    local service_name="$1"
    local service_path="$SERVICES_DIR/$service_name"
    
    echo -e "${BLUE}Debug Instructions for $service_name:${NC}"
    echo ""
    echo -e "${YELLOW}1. In CLion, create a Debug Configuration:${NC}"
    echo "   - Name: $service_name (Debug)"
    echo "   - Target: $service_name"
    echo "   - Program arguments: --dev"
    echo "   - Working directory: $PROJECT_DIR"
    echo ""
    echo -e "${YELLOW}2. Set breakpoints in your service code${NC}"
    echo ""
    echo -e "${YELLOW}3. Start debugging - the service will run standalone${NC}"
    echo ""
    echo -e "${YELLOW}4. In a terminal, run:${NC}"
    echo "   $0 test $service_name"
    echo ""
    echo -e "${YELLOW}5. Send requests through the test harness to trigger your breakpoints${NC}"
    echo ""
    echo -e "${GREEN}Socket location: /tmp/${service_name}_dev.sock${NC}"
}

clean_sockets() {
    echo -e "${BLUE}Cleaning socket files...${NC}"
    rm -f /tmp/*_dev.sock
    echo -e "${GREEN}Socket files cleaned${NC}"
}

# Main script logic
case "$1" in
    "build")
        build_project
        ;;
    "run")
        if [ -z "$2" ]; then
            echo -e "${RED}Please specify a service name${NC}"
            print_usage
            exit 1
        fi
        run_service_dev "$2"
        ;;
    "debug")
        if [ -z "$2" ]; then
            echo -e "${RED}Please specify a service name${NC}"
            print_usage
            exit 1
        fi
        show_debug_instructions "$2"
        ;;
    "list")
        list_services
        ;;
    "clean")
        clean_sockets
        ;;
    *)
        print_usage
        exit 1
        ;;
esac