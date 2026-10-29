#pragma once

#include "resilientpos/util/UuidGenerator.hpp"

#include <filesystem>
#include <string>
#include <system_error>

namespace resilientpos::test {

// A fresh database file in the temp directory, deleted (with its WAL files)
// when the test ends.
class TempDatabase {
public:
    TempDatabase()
        : path_{std::filesystem::temp_directory_path() /
                ("resilientpos-test-" + resilientpos::util::UuidGenerator::generate() + ".db")}
    {
    }

    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;

    ~TempDatabase()
    {
        std::error_code ignored;
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path_.string() + suffix, ignored);
        }
    }

    [[nodiscard]]
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace resilientpos::test
