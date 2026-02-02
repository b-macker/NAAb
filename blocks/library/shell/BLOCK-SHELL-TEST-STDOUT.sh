#!/bin/bash
# Test block for Shell stdout verification

test_stdout() {
    echo "Hello from Bash stdout!"
    echo "This should be visible if stdout fix works"
    echo "Function returned successfully"
}

print_info() {
    echo "System Information:"
    echo "User: $USER"
    echo "Home: $HOME"
    echo "PWD: $PWD"
    echo "Shell: $SHELL"
    echo "Done printing info"
}

test_commands() {
    echo "Testing shell commands:"
    echo "Current date:"
    date
    echo "Current directory:"
    pwd
    echo "Done with commands"
}

# If called directly, run all tests
if [ "$1" == "test_stdout" ]; then
    test_stdout
elif [ "$1" == "print_info" ]; then
    print_info
elif [ "$1" == "test_commands" ]; then
    test_commands
fi
