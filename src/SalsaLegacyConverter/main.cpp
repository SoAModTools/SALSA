#include "LegacyConverter.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string_view>

namespace {

void usage() {
    std::cerr << "usage: SalsaLegacyConverter convert <input.prj> <output-directory> "
                 "[--retain-original] [--disable-resource-limits]\n";
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc < 4 || std::wstring_view(argv[1]) != L"convert") { usage(); return 2; }
    salsa::legacy::ConversionRequest request{};
    request.input = argv[2];
    request.output = argv[3];
    for (int index = 4; index < argc; ++index) {
        const auto option = std::wstring_view(argv[index]);
        if (option == L"--retain-original") request.retainOriginal = true;
        else if (option == L"--disable-resource-limits") request.disableResourceLimits = true;
        else { usage(); return 2; }
    }
    salsa::legacy::ConversionOutcome outcome{};
    try {
        outcome = salsa::legacy::convertLegacyProject(request,
            [](const std::string_view phase, const std::uint64_t completed,
               const std::uint64_t total, const std::string_view current) {
                nlohmann::ordered_json event{{"protocol", "jahorta.salsa.legacy-converter-events"},
                    {"version", 1}, {"type", "progress"}, {"phase", phase},
                    {"completed", completed}, {"total", total}, {"current", current}};
                std::cout << event.dump() << '\n' << std::flush;
            });
    } catch (const std::bad_alloc&) {
        std::error_code ignored; std::filesystem::remove_all(request.output, ignored);
        outcome = {salsa::legacy::ConversionOutcome::Status::Failed,
            "The conversion resource limit was reached while writing the capsule.", {}};
    } catch (const std::exception&) {
        std::error_code ignored; std::filesystem::remove_all(request.output, ignored);
        outcome = {salsa::legacy::ConversionOutcome::Status::Failed,
            "The native converter failed without finalizing a capsule.", {}};
    }
    nlohmann::ordered_json result{{"protocol", "jahorta.salsa.legacy-converter-events"},
        {"version", 1}, {"type", "result"}, {"message", outcome.message},
        {"capsuleId", outcome.capsuleId}};
    switch (outcome.status) {
    case salsa::legacy::ConversionOutcome::Status::Ready: result["status"] = "ready"; break;
    case salsa::legacy::ConversionOutcome::Status::ActionRequired: result["status"] = "action-required"; break;
    case salsa::legacy::ConversionOutcome::Status::Rejected: result["status"] = "rejected"; break;
    case salsa::legacy::ConversionOutcome::Status::Failed: result["status"] = "failed"; break;
    }
    std::cout << result.dump() << '\n' << std::flush;
    if (outcome.status == salsa::legacy::ConversionOutcome::Status::Ready
        || outcome.status == salsa::legacy::ConversionOutcome::Status::ActionRequired) return 0;
    return outcome.status == salsa::legacy::ConversionOutcome::Status::Rejected ? 4 : 5;
}
