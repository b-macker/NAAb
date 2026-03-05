const fs = require('fs');
const path = require('path');
function walkSync(dir) {
    let count = 0;
    for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) count += walkSync(full);
        else count++;
    }
    return count;
}
const start = Date.now();
const home = process.env.HOME || '/tmp';
walkSync(path.join(home, '.naab/language/src'));
console.log((Date.now() - start) * 1000);
