// Helix Vessel: " + func_name + " (CPP)
#include <iostream>
#include <cmath>
#include <iomanip>
#include <string>

double process(double v)  {
    for(int i=0; i<100; i++)  { v = std::sqrt(v * v + 0.01);  }
    return v;
 }

int main(int argc, char** argv)  {
    if (argc < 2)  {
        std::cout << " {\"status\": \"READY\", \"target\": \"CPP\" }" << std::endl;
        return 0;
     }
    double val = std::stod(argv[1]);
    std::cout << std::fixed << std::setprecision(15) << process(val);
    return 0;
 }
