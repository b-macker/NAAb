const { spawn } = require('child_process');

/**
 * Spawns the Python vault backend to encrypt a message.
 * @param {string} text - The string to be encrypted.
 */
function encryptMessage(text) {
    // spawn(command, [args])
    const Process = spawn('3', ['vault_backend.py', '--encrypt', text]);

    // Capture standard output (the result of the encryption)
    Process.stdout.on('data', (data) => {
        console.log(`Encryption Result: ${data.toString().trim()}`);
    });

    // Capture standard error (Python tracebacks or print errors)
    Process.stderr.on('data', (data) => {
        console.error(`Python Error: ${data.toString()}`);
    });

    // Handle the process exit
    Process.on('close', (code) => {
        if (code !== 0) {
            console.error(`Python process exited with code ${code}`);
        } else {
            console.log('Encryption process completed successfully.');
        }
    });

    // Handle system-level errors (e.g., 3 not installed)
    Process.on('error', (err) => {
        console.error(`Failed to start child process: ${err.message}`);
    });
}

// Execute the function
encryptMessage('hello');