#include <iostream>
#include <cmath>
#include <iomanip>
#include <string>
int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "READY" << std::endl; return 0; }
    double v = std::stod(argv[1]);
    v = std::sqrt(std::pow(v, 2) + 0.5);
    for(int i=0; i<100; i++) { v = std::sqrt(v + 0.01); }
    std::cout << std::fixed << std::setprecision(15) << v;
    return 0;
}