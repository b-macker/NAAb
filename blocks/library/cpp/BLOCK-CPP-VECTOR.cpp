// NAAb Block: BLOCK-CPP-VECTOR
// C++ Vector Math Block
// Provides fast numerical operations on arrays

#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>

extern "C" {

// Sum all elements in array
int sum(const int* arr, int size) {
    int total = 0;
    for (int i = 0; i < size; i++) {
        total += arr[i];
    }
    return total;
}

// Calculate average of array
double average(const int* arr, int size) {
    if (size == 0) return 0.0;
    int total = sum(arr, size);
    return static_cast<double>(total) / size;
}

// Find maximum value in array
int max(const int* arr, int size) {
    if (size == 0) return 0;
    int max_val = arr[0];
    for (int i = 1; i < size; i++) {
        if (arr[i] > max_val) max_val = arr[i];
    }
    return max_val;
}

// Find minimum value in array
int min(const int* arr, int size) {
    if (size == 0) return 0;
    int min_val = arr[0];
    for (int i = 1; i < size; i++) {
        if (arr[i] < min_val) min_val = arr[i];
    }
    return min_val;
}

// Calculate product of all elements
int product(const int* arr, int size) {
    int result = 1;
    for (int i = 0; i < size; i++) {
        result *= arr[i];
    }
    return result;
}

// Calculate standard deviation
double stddev(const int* arr, int size) {
    if (size == 0) return 0.0;

    double avg = average(arr, size);
    double sum_sq = 0.0;

    for (int i = 0; i < size; i++) {
        double diff = arr[i] - avg;
        sum_sq += diff * diff;
    }

    return std::sqrt(sum_sq / size);
}

// Count elements greater than threshold
int count_greater(const int* arr, int size, int threshold) {
    int count = 0;
    for (int i = 0; i < size; i++) {
        if (arr[i] > threshold) count++;
    }
    return count;
}

// Dot product of two arrays
int dot_product(const int* a, const int* b, int size) {
    int result = 0;
    for (int i = 0; i < size; i++) {
        result += a[i] * b[i];
    }
    return result;
}

} // extern "C"
