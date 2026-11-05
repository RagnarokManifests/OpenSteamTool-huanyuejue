#pragma once

#include <filesystem>
#include <string>

namespace OSTPlatform::Hash {

    std::string Sha256OfFile(const std::filesystem::path& path);

    // Raw 20-byte SHA-1 of an in-memory buffer (Steam sha_schema form), or "" on failure.
    std::string Sha1RawOfBuffer(const void* data, size_t size);

} // namespace OSTPlatform::Hash
