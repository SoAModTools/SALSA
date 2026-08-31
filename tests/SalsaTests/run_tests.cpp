#include "SalsaCore/Application/ApplicationInfo.h"

#include <iostream>
#include <string_view>

int main() {
    if (salsa::core::applicationName() != std::string_view{"SALSA"}) {
        std::cerr << "Bootstrap test failed: unexpected application name.\n";
        return 1;
    }

    std::cout << "SALSA bootstrap test passed.\n";
    return 0;
}
