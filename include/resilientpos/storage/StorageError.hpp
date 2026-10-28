#pragma once

#include <stdexcept>

namespace resilientpos::storage {

// Raised when the local store cannot complete an operation
// (database unavailable, constraint violation, unknown transaction, ...).
class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace resilientpos::storage
