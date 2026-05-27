# ServiceBroker Architecture Implementation

Este documento descreve a nova arquitetura **ServiceBroker** implementada para o projeto rdws_us, que transforma o loader de um gerenciador de processos para um **proxy/broker de serviços**.

## 🏗️ **Arquitetura**

### **Conceitos Principais**

1. **ServiceBroker**: Servidor central que aceita conexões TCP e UNIX socket
2. **ServiceRegistry**: Sistema de registro e indexação de serviços
3. **ServiceIdentity**: Identificação rica dos serviços conectados  
4. **ServiceClient**: Biblioteca cliente para serviços se conectarem ao broker
5. **ServiceMonitor**: Interface de monitoramento em tempo real

### **Fluxo de Operação**

```
1. ServiceBroker inicia → Abre TCP:8080 + UNIX:service_broker.sock
2. Serviço inicia → Conecta ao broker → Envia identificação completa
3. Broker registra → Indexa capacidades → Confirma registro
4. Cliente faz request → Broker roteia → Serviço processa → Responde
5. Health check contínuo via ping/pong
```

## 🚀 **Componentes Implementados**

### **1. ServiceIdentity** 
Estrutura completa de identificação:
- `machineName`, `serviceName`, `serviceId`, `version`
- `capabilities[]` - Lista de funcionalidades (ex: "greeting", "translation")
- `environment` - "dev", "staging", "prod"
- `maxConcurrent` - Capacidade máxima
- Estatísticas de runtime (load, requests, errors, ping)

### **2. ServiceRegistry**
Sistema de indexação inteligente:
- Índice por **capability** → Encontra serviços por funcionalidade
- Índice por **machine** → Afinidade de localização  
- Índice por **environment** → Segregação por ambiente
- **Load balancing**: Round-robin, least-loaded, fastest-response, random
- **Health checking** automático

### **3. ServiceBroker** 
Broker multi-protocolo:
- **TCP Listener** (porta 8080) - Para serviços remotos
- **UNIX Socket Listener** (/tmp/service_broker.sock) - Para serviços locais
- **Protocolo JSON** para comunicação
- **Connection management** thread-safe
- **Request routing** com load balancing

### **4. ServiceClient**
Biblioteca cliente para serviços:
- **Auto-reconnect** em caso de falha
- **Ping/Pong** automático para health check
- **Request handler** configurável
- Suporte a **TCP** e **UNIX socket**

### **5. ServiceMonitor**
Interface de monitoramento visual:
- **Status em tempo real** do broker e serviços
- **Tabelas formatadas** com conexões ativas
- **Índice de capabilities** 
- **Health status** detalhado
- **Continuous monitoring** com refresh automático

## 📋 **Protocolo de Comunicação**

### **Handshake de Identificação**
```json
// Cliente → Broker
{
  "type": "IDENTIFY",
  "identity": {
    "machineName": "server-01",
    "serviceName": "greeting_service", 
    "serviceId": "greeting_001",
    "version": "v1.0.0",
    "capabilities": ["greeting", "translation"],
    "environment": "dev",
    "maxConcurrent": 5
  }
}

// Broker → Cliente  
{
  "type": "ACKNOWLEDGED",
  "serviceId": "greeting_001",
  "status": "registered"
}
```

### **Health Check (Ping/Pong)**
```json
// Cliente → Broker (a cada 30s)
{
  "type": "PING",
  "serviceId": "greeting_001", 
  "stats": {
    "currentLoad": 2,
    "totalRequests": 150,
    "errorCount": 1
  }
}

// Broker → Cliente
{
  "type": "PONG",
  "timestamp": 1677123456789
}
```

## 🛠️ **Como Usar**

### **1. Iniciar o Broker**

```bash
# Compilar
cmake -S . -B build
cmake --build build

# Executar broker com interface de monitoramento
./build/src/loader/service_broker_monitor

# Ou broker simples
./build/src/loader/service_broker_example
```

### **2. Conectar um Serviço**

```bash
# Conectar serviço de exemplo via UNIX socket
./build/src/loader/example_service_client

# Conectar via TCP
./build/src/loader/example_service_client "tcp://localhost:8080" "server-02" "greeting_002"
```

### **3. Monitoramento**

Interface interativa no `service_broker_monitor`:
- `s` - Status atual
- `c` - Monitoramento contínuo  
- `h` - Health status
- `cap greeting` - Serviços com capability "greeting"
- `machine server-01` - Serviços na máquina "server-01"
- `f` - Salvar status em arquivo

## 💡 **Vantagens da Nova Arquitetura**

### **🔹 Flexibilidade de Deploy**
- Serviços podem rodar em containers, máquinas remotas
- Não dependem do lifecycle do loader

### **🔹 Desenvolvimento Mais Fácil** 
- Debug individual de cada serviço
- Não precisa reiniciar o loader para testar

### **🔹 Escalabilidade**
- Load balancing automático entre instâncias
- Registro dinâmico de novos serviços

### **🔹 Robustez**
- Auto-recovery via reconnection
- Health monitoring contínuo
- Remoção automática de serviços unhealthy

### **🔹 Observabilidade**
- Monitoring em tempo real
- Métricas de performance e load
- Rastreamento de capabilities

## 🎯 **Casos de Uso**

### **Roteamento Inteligente**
```cpp
// Encontrar melhor serviço para greeting
auto serviceId = registry.selectBestService("greeting", LoadBalancing::LEAST_LOADED);

// Preferir serviços locais  
auto localServices = registry.findServicesByMachine("localhost");

// Serviços por ambiente
auto devServices = registry.findServicesByEnvironment("dev");
```

### **Desenvolvimento Multi-Ambiente**
- **Dev**: Serviços locais via UNIX socket
- **Staging**: Mix de local + remoto via TCP  
- **Prod**: Serviços distribuídos com load balancing

### **Microserviços Heterogêneos**  
Serviços em diferentes linguagens conectam ao mesmo broker:
- Python ML service
- Go API gateway  
- C++ compute service
- Node.js frontend service

## 🔧 **Configuração**

Services podem escolher protocolo na conexão:
```cpp
// UNIX socket (local, mais rápido)
ServiceClient client(identity, "unix:///tmp/service_broker.sock");

// TCP (remoto, mais flexível)  
ServiceClient client(identity, "tcp://broker-host:8080");
```

Broker configurável:
```cpp  
ServiceBroker broker(8080, "/tmp/service_broker.sock");  // Customizar portas/paths
```

Esta implementação transforma o projeto em um verdadeiro **Service Mesh Controller** local, oferecendo flexibilidade máxima para desenvolvimento e deploy! 🚀