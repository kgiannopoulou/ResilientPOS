#pragma once

#include <stdexcept>

namespace resilientpos::storage {

// Raised when the local store cannot complete an operation
// (database unavailable, constraint violation, unknown transaction, ...).
class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The database stayed locked by other writers for the whole busy timeout.
// Unlike other storage failures this one is temporary: try again later.
class StorageBusyError : public StorageError {
public:
    using StorageError::StorageError;
};

} // namespace resilientpos::storage
