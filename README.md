# rdws_us

Modular C++ prooject with multiple supbprojects in `src/`, each with:

- internal library (`<name>_core`)
- binary exe (`<name>_app`)
- unit tests (`<name>_test`)

## Current structure

- `CMakeLists.txt`: root configiration and googletest dependency 
- `src/greeting_app/`: example subproject
- `src/service_broker/`: main service broker subproject
- `tools/new_subproject.sh`: new subprject generator

## Dependencies

| Biblioteca | Utilização | Instalação |
|---|---|---|
| `cmake` ≥ 3.20 | Build system | `sudo apt install cmake` |
| `gdb` | Debug (VS Code) | `sudo apt install gdb` |
| `libssl-dev` | Auth middleware — HMAC-SHA256 para JWT (HS256) | `sudo apt install libssl-dev` |

As restantes dependências (GoogleTest, cpp-httplib, spdlog, RapidJSON, tl::expected, valijson) são descarregadas automaticamente via CMake FetchContent na primeira build.

## Build and tests

```bash
# Instalar dependência de sistema necessária
sudo apt install libssl-dev

cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Debugging (VS Code)

Requires `gdb` installed (`sudo apt install gdb`).

Build in Debug mode first:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
```

The `.vscode/launch.json` has three configurations:

- **Debug service_broker_app** — inicia o broker na porta `8080` e socket `/tmp/service_broker.sock`
- **Debug example_service --dev** — inicia um service em modo dev, conecta no broker via socket
- **Debug Broker + Example Service** — sobe os dois simultaneamente (compound)

**Ordem de inicialização:** o broker deve estar no ar antes de qualquer service conectar.
Para garantir isso, inicie as configurações manualmente em sequência ou use o compound e aguarde o broker logar `ServiceBroker started successfully!` antes de interagir com o service.

## Create a new subproject 

```bash
./tools/new_subproject.sh auth
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Result:

- `src/auth/CMakeLists.txt`
- `src/auth/auth.hpp`
- `src/auth/auth.cpp`
- `src/auth/main.cpp`
- `src/auth/tests/CMakeLists.txt`
- `src/auth/tests/auth_test.cpp`

