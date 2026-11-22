#include "mini_test.hpp"

#include <cstring>

#include "common/cpu_features.hpp"

int main(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-simd") == 0) lv::set_simd_disabled(true);
        else filter = argv[i];
    }
    return lv_test::run_all(filter);
}
