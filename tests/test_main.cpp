#include <cstdio>
#include <cstring>

#include "test_framework.h"

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 ||
            std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: p4gw_tests [-v|--verbose]\n"
                        "  default: per-area summary\n"
                        "  -v:      also list every individual test\n");
            return 0;
        } else {
            std::printf("unknown option: %s (try --help)\n", argv[i]);
            return 2;
        }
    }
    return testfw::runAll(verbose);
}
