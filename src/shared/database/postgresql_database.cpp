#include "postgresql_database.h"

#include <stdexcept>
#include <tuple>
#include <utility>

namespace rdws::database {

namespace {

template <typename Transaction>
pqxx::result exec_prepared_helper(Transaction& txn, const std::string& stmt_name,
                                  const std::vector<std::string>& params,
                                  const std::string& query) {
    if (params.empty()) {
        return txn.exec(query);
    }
    pqxx::params p;
    for (const auto& param : params) {
        p.append(param);
    }
    return txn.exec(pqxx::prepped{stmt_name}, p);
}

} // namespace

// PostgreSQLResultSet Implementation

PostgreSQLResultSet::PostgreSQLResultSet(pqxx::result res)
    : result(std::move(res)), currentRow(0) {}

bool PostgreSQLResultSet::next() {
    if (currentRow < (pqxx::result::size_type)result.size()) {
        ++currentRow;
        return true;
    }
    return false;
}

bool PostgreSQLResultSet::previous() {
    if (currentRow > 1) {
        --currentRow;
        return true;
    }
    return false;
}

void PostgreSQLResultSet::reset() {
    currentRow = 0;
}

std::string PostgreSQLResultSet::getString(const std::string& columnName) {
    if (currentRow == 0 || currentRow > (pqxx::result::size_type)result.size()) {
        throw std::runtime_error("Invalid row position");
    }
    return result[currentRow - 1][columnName].as<std::string>();
}

int PostgreSQLResultSet::getInt(const std::string& columnName) {
    if (currentRow == 0 || currentRow > (pqxx::result::size_type)result.size()) {
        throw std::runtime_error("Invalid row position");
    }
    return result[currentRow - 1][columnName].as<int>();
}

double PostgreSQLResultSet::getDouble(const std::string& columnName) {
    if (currentRow == 0 || currentRow > (pqxx::result::size_type)result.size()) {
        throw std::runtime_error("Invalid row position");
    }
    return result[currentRow - 1][columnName].as<double>();
}

bool PostgreSQLResultSet::getBool(const std::string& columnName) {
    if (currentRow == 0 || currentRow > (pqxx::result::size_type)result.size()) {
        throw std::runtime_error("Invalid row position");
    }
    return result[currentRow - 1][columnName].as<bool>();
}

bool PostgreSQLResultSet::isNull(const std::string& columnName) {
    if (currentRow == 0 || currentRow > (pqxx::result::size_type)result.size()) {
        throw std::runtime_error("Invalid row position");
    }
    return result[currentRow - 1][columnName].is_null();
}

size_t PostgreSQLResultSet::getColumnCount() {
    return result.columns();
}

std::vector<std::string> PostgreSQLResultSet::getColumnNames() const {
    std::vector<std::string> names;
    names.reserve(result.columns());
    for (auto i = 0; i < (pqxx::result::size_type)result.columns(); ++i) {
        names.emplace_back(result.column_name(i));
    }
    return names;
}

size_t PostgreSQLResultSet::getRowCount() {
    return result.size();
}

// PostgreSQLDatabase Implementation

PostgreSQLDatabase::PostgreSQLDatabase() {
    PostgreSQLDatabase::connect();
}

PostgreSQLDatabase::PostgreSQLDatabase(const rdws::Config& dbConfig) : config(dbConfig) {
    PostgreSQLDatabase::connect();
}

PostgreSQLDatabase::~PostgreSQLDatabase() {
    if (currentTransaction) {
        PostgreSQLDatabase::rollbackTransaction();
    }
    PostgreSQLDatabase::disconnect();
}

std::unique_ptr<IResultSet>
PostgreSQLDatabase::execQuery(const std::string& query,
                              const std::vector<std::string>& parameters) {
    try {
        ensureConnection();

        const std::string stmt_name = "stmt_" + std::to_string(std::hash<std::string>{}(query));
        if (!preparedStatements_.count(stmt_name)) {
            connection->prepare(stmt_name, query);
            preparedStatements_.insert(stmt_name);
        }

        if (currentTransaction) {
            pqxx::result result = exec_prepared_helper(*currentTransaction, stmt_name, parameters, query);
            return std::make_unique<PostgreSQLResultSet>(std::move(result));
        } else {
            pqxx::work txn{*connection};
            pqxx::result result = exec_prepared_helper(txn, stmt_name, parameters, query);
            txn.commit();
            return std::make_unique<PostgreSQLResultSet>(std::move(result));
        }
    } catch (const std::exception& e) {
        lastError = e.what();
        throw std::runtime_error("Query execution failed: " + std::string(e.what()));
    }
}

bool PostgreSQLDatabase::execCommand(const std::string& command,
                                     const std::vector<std::string>& parameters) {
    try {
        ensureConnection();

        const std::string stmt_name = "stmt_" + std::to_string(std::hash<std::string>{}(command));
        if (!preparedStatements_.count(stmt_name)) {
            connection->prepare(stmt_name, command);
            preparedStatements_.insert(stmt_name);
        }

        if (currentTransaction) {
            exec_prepared_helper(*currentTransaction, stmt_name, parameters, command);
        } else {
            pqxx::work txn{*connection};
            exec_prepared_helper(txn, stmt_name, parameters, command);
            txn.commit();
        }
        return true;
    } catch (const std::exception& e) {
        lastError = e.what();
        throw std::runtime_error("Command execution failed: " + std::string(e.what()));
    }
}

bool PostgreSQLDatabase::execBatch(const std::vector<std::string>& commands,
                                   const std::vector<std::vector<std::string>>& parameterSets) {
    if (commands.size() != parameterSets.size()) {
        lastError = "Commands and parameter sets size mismatch";
        return false;
    }

    try {
        ensureConnection();

        const bool wasInTransaction = (currentTransaction != nullptr);
        if (!wasInTransaction) {
            beginTransaction();
        }

        for (size_t i = 0; i < commands.size(); ++i) {
            const std::string stmt_name = "stmt_" + std::to_string(std::hash<std::string>{}(commands[i]));
            if (!preparedStatements_.count(stmt_name)) {
                connection->prepare(stmt_name, commands[i]);
                preparedStatements_.insert(stmt_name);
            }
            exec_prepared_helper(*currentTransaction, stmt_name, parameterSets[i], commands[i]);
        }

        if (!wasInTransaction) {
            commitTransaction();
        }

        return true;
    } catch (const std::exception& e) {
        lastError = e.what();
        if (currentTransaction) {
            rollbackTransaction();
        }
        return false;
    }
}

void PostgreSQLDatabase::beginTransaction() {
    ensureConnection();
    if (currentTransaction) {
        throw std::runtime_error("Transaction already in progress");
    }
    currentTransaction = std::make_unique<pqxx::work>(*connection);
}

void PostgreSQLDatabase::commitTransaction() {
    if (!currentTransaction) {
        throw std::runtime_error("No transaction in progress");
    }
    currentTransaction->commit();
    currentTransaction.reset();
}

void PostgreSQLDatabase::rollbackTransaction() {
    if (!currentTransaction) {
        throw std::runtime_error("No transaction in progress");
    }
    currentTransaction->abort();
    currentTransaction.reset();
}

bool PostgreSQLDatabase::isConnected() {
    return connection && connection->is_open();
}

void PostgreSQLDatabase::connect() {
    try {
        connection = std::make_unique<pqxx::connection>(config.getConnectionString());
        preparedStatements_.clear();
        lastError.clear();
    } catch (const std::exception& e) {
        lastError = e.what();
        throw std::runtime_error("Failed to connect to database: " + std::string(e.what()));
    }
}

void PostgreSQLDatabase::disconnect() {
    if (currentTransaction) {
        rollbackTransaction();
    }
    if (connection) {
        connection->close();
        connection.reset();
    }
    preparedStatements_.clear();
}

std::string PostgreSQLDatabase::getLastError() {
    return lastError;
}

void PostgreSQLDatabase::ensureConnection() {
    if (!isConnected()) {
        connect();
    }
}

} // namespace rdws::database
