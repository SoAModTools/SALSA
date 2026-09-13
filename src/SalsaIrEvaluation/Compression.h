#pragma once
#include "Evaluation.h"
#include <map>
namespace salsa::evaluation::compression {
using Tables=std::map<std::string,std::span<const std::byte>>;
Json capture(const std::filesystem::path& directory,const Tables& tables,const Json& source);
Json run(const std::filesystem::path& manifest,std::string_view codec,Controls controls);
void smoke();
}
