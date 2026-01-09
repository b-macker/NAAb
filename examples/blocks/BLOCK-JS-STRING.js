// NAAb Block: BLOCK-JS-STRING
// JavaScript String Utilities Block
// Provides string manipulation functions

// Convert string to uppercase
function toUpper(str) {
    return str.toUpperCase();
}

// Convert string to lowercase
function toLower(str) {
    return str.toLowerCase();
}

// Format string with placeholders
function format(template, ...args) {
    let result = template;
    args.forEach((arg, index) => {
        result = result.replace('{}', arg);
    });
    return result;
}

// Repeat string n times
function repeat(str, count) {
    return str.repeat(count);
}

// Reverse a string
function reverse(str) {
    return str.split('').reverse().join('');
}

// Check if string starts with prefix
function startsWith(str, prefix) {
    return str.startsWith(prefix);
}

// Check if string ends with suffix
function endsWith(str, suffix) {
    return str.endsWith(suffix);
}

// Trim whitespace
function trim(str) {
    return str.trim();
}

// Split string by delimiter
function split(str, delimiter) {
    return str.split(delimiter);
}

// Join array of strings
function join(arr, separator) {
    return arr.join(separator);
}
