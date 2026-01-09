// NAAb Block: BLOCK-JS-FORMAT
// JavaScript Formatting Block
// Provides text formatting and templating

// Simple template function
function template(data) {
    let lines = [];
    lines.push("=".repeat(40));
    lines.push("Statistics Report");
    lines.push("=".repeat(40));

    for (const [key, value] of Object.entries(data)) {
        const label = key.charAt(0).toUpperCase() + key.slice(1);
        lines.push(`${label}: ${value}`);
    }

    lines.push("=".repeat(40));
    return lines.join('\n');
}

// Format number with commas
function formatNumber(num) {
    return num.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

// Format as currency
function formatCurrency(amount, currency = '$') {
    const formatted = Math.abs(amount).toFixed(2);
    const withCommas = formatNumber(formatted);
    return amount >= 0 ? `${currency}${withCommas}` : `-${currency}${withCommas}`;
}

// Format percentage
function formatPercent(value, decimals = 2) {
    return (value * 100).toFixed(decimals) + '%';
}

// Pad string to length
function padLeft(str, length, char = ' ') {
    return str.toString().padStart(length, char);
}

function padRight(str, length, char = ' ') {
    return str.toString().padEnd(length, char);
}

// Create table row
function tableRow(cells, separator = ' | ') {
    return cells.join(separator);
}

// Word wrap text
function wordWrap(text, maxWidth) {
    const words = text.split(' ');
    const lines = [];
    let currentLine = '';

    for (const word of words) {
        if ((currentLine + word).length > maxWidth) {
            if (currentLine) lines.push(currentLine.trim());
            currentLine = word + ' ';
        } else {
            currentLine += word + ' ';
        }
    }

    if (currentLine) lines.push(currentLine.trim());
    return lines.join('\n');
}

// Create JSON-like output
function toJSON(obj) {
    return JSON.stringify(obj, null, 2);
}
