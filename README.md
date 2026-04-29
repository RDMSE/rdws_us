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

## Build and tests

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

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

