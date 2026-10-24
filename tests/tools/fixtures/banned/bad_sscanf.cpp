// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

int parse(const char* s) {
    int v = 0;
    sscanf(s, "%d", &v);
    return v;
}
