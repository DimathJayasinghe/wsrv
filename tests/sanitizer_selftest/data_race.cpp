// SPDX-License-Identifier: Apache-2.0
// Deliberate bug: two threads increment a plain int with no synchronisation. Must be reported by TSan.
#include <thread>

namespace {
int counter = 0;
void bump() {
    for (int i = 0; i < 1000; ++i) {
        ++counter;
    }
}
}  // namespace

int main() {
    std::thread a(bump);
    std::thread b(bump);
    a.join();
    b.join();
    return counter == 0 ? 1 : 0;
}
