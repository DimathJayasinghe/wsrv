// SPDX-License-Identifier: Apache-2.0
// Deliberate bug: signed integer overflow is undefined behaviour. Must be reported by UBSan.
#include <climits>

int main() {
    volatile int x = INT_MAX;
    x = x + 1;
    return 0;
}
