// NAAb Block: BLOCK-CPP-MATH
// C++ Math Utilities Block
// Provides basic mathematical operations

#include <cmath>

extern "C" {

// Add two numbers
int add(int a, int b) {
    return a + b;
}

// Subtract two numbers
int subtract(int a, int b) {
    return a - b;
}

// Multiply two numbers
int multiply(int a, int b) {
    return a * b;
}

// Divide two numbers (integer division)
int divide(int a, int b) {
    if (b == 0) return 0;
    return a / b;
}

// Power function
double power(double base, double exponent) {
    return std::pow(base, exponent);
}

// Square root
double sqrt_val(double x) {
    return std::sqrt(x);
}

// Absolute value
int abs_val(int x) {
    return std::abs(x);
}

// Maximum of two numbers
int max_val(int a, int b) {
    return (a > b) ? a : b;
}

// Minimum of two numbers
int min_val(int a, int b) {
    return (a < b) ? a : b;
}

} // extern "C"
