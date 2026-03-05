const N = 100;
const A = Array.from({length: N}, (_, i) => Array.from({length: N}, (_, j) => (i*7+j*13) % 97));
const B = Array.from({length: N}, (_, i) => Array.from({length: N}, (_, j) => (i*11+j*17) % 89));
const start = Date.now();
const C = Array.from({length: N}, (_, i) => Array.from({length: N}, (_, j) => {
    let sum = 0;
    for (let k = 0; k < N; k++) sum += A[i][k] * B[k][j];
    return sum;
}));
console.log((Date.now() - start) * 1000);
