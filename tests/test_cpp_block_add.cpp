// Test C++ Block: Simple Addition Function
// This is a complete, standalone C++ function for testing the C++ executor

#include <iostream>

// Export as C function for easy dlsym lookup
extern "C" {
    int add(int a, int b) {
        std::cout << "[C++ BLOCK] add(" << a << ", " << b << ")" << std::endl;
        int result = a + b;
        std::cout << "[C++ BLOCK] result = " << result << std::endl;
        return result;
    }

    int multiply(int a, int b) {
        std::cout << "[C++ BLOCK] multiply(" << a << ", " << b << ")" << std::endl;
        return a * b;
    }

    double divide(double a, double b) {
        if (b == 0.0) {
            std::cerr << "[C++ BLOCK] Error: Division by zero!" << std::endl;
            return 0.0;
        }
        return a / b;
    }
}
