#include "resilientpos/core/Transaction.hpp"

#include "resilientpos/util/UuidGenerator.hpp"

namespace resilientpos::core {

Transaction Transaction::create(Money amount)
{
    return Transaction{
        .id = util::UuidGenerator::generate(),
        .amount = amount,
        .status = TransactionStatus::Created,
        .createdAt = std::chrono::system_clock::now(),
        .syncAttempts = 0,
    };
}

} // namespace resilientpos::core
