// Test block for stdout verification
function testStdout() {
    console.log("Hello from JavaScript stdout!");
    console.log("This should be visible if stdout fix works");
    return "Function returned successfully";
}

function printNumbers() {
    console.log("Counting: 1, 2, 3, 4, 5");
    for (let i = 1; i <= 5; i++) {
        console.log("Number: " + i);
    }
    return "Done counting";
}
