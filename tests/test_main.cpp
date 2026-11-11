#include "tests/test.h"

#include <cstring>

namespace p2::test {

namespace {
int g_failures_in_case = 0;
}

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& message) {
    ++g_failures_in_case;
    std::printf("    FAILED %s:%d: %s\n", file, line, message.c_str());
}

} // namespace p2::test

int main(int argc, char** argv) {
    using namespace p2::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed = 0;
    for (const Case& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        g_failures_in_case = 0;
        c.fn();
        ++ran;
        if (g_failures_in_case) {
            ++failed;
            std::printf("[FAIL] %s\n", c.name);
        }
    }
    std::printf("%d test(s), %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
