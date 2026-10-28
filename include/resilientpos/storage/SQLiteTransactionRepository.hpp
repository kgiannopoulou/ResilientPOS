#pragma once

#include "resilientpos/storage/TransactionRepository.hpp"

#include <memory>
#include <string>

struct sqlite3;

namespace resilientpos::storage {

// TransactionRepository backed by a local SQLite file.
//
// The database is the till's source of truth while offline: every write is
// committed to disk before the call returns, so a crash or power cut right
// after a sale does not lose it.
class SQLiteTransactionRepository final : public TransactionRepository {
public:
    // Opens (or creates) the database at `path` and ensures the schema exists.
    // Pass ":memory:" for a throwaway in-memory database.
    explicit SQLiteTransactionRepository(const std::string& path);

    void save(const core::Transaction& transaction) override;

    [[nodiscard]]
    std::optional<core::Transaction>
    findById(const std::string& id) override;

    [[nodiscard]]
    std::vector<core::Transaction>
    findPending() override;

    void updateStatus(
        const std::string& id,
        core::TransactionStatus status
    ) override;

private:
    struct Closer {
        void operator()(sqlite3* db) const noexcept;
    };

    std::unique_ptr<sqlite3, Closer> db_;
};

} // namespace resilientpos::storage
