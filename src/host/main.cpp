#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("choir %s\n", CHOIR_VERSION);
            return 0;
        }
    }
    return 0;
}
