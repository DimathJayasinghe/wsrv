// SPDX-License-Identifier: Apache-2.0
// Deliberate bug: writes one byte past a heap array. Must be reported by AddressSanitizer.
// If this program ever runs "fine", the asan-ubsan preset is not actually instrumenting anything.
int main() {
    char* buffer = new char[8];
    volatile char* p = buffer;  // volatile: stop the optimiser from removing the write
    p[8] = 'x';                 // one past the end
    delete[] buffer;
    return 0;
}
