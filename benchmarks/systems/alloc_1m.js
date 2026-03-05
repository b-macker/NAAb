const start = Date.now();
const objs = Array.from({length: 1000000}, (_, i) => ({id: i, data: new Array(10).fill(0)}));
objs.length = 0;
console.log((Date.now() - start) * 1000);
