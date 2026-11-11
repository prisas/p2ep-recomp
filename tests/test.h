#pragma once

// Minimal self-registering test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace p2::test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <class T>
std::string str(const T& v) {
    char buf[64];
    if constexpr (std::is_floating_point_v<T>) {
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        return buf;
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
        return buf;
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(v);
    } else {
        return "?";
    }
}

} // namespace p2::test

#define TEST(name)                                                                                           \
    static void p2_test_##name();                                                                            \
    static ::p2::test::Registrar p2_reg_##name(#name, p2_test_##name);                                       \
    static void p2_test_##name()

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) ::p2::test::fail(__FILE__, __LINE__, #cond);                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        const auto& p2_va = (a);                                                                             \
        const auto& p2_vb = (b);                                                                             \
        if (!(p2_va == p2_vb))                                                                               \
            ::p2::test::fail(__FILE__, __LINE__,                                                             \
                             std::string(#a " == " #b "  [") + ::p2::test::str(p2_va) + " vs " +             \
                                 ::p2::test::str(p2_vb) + "]");                                              \
    } while (0)
