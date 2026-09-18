#!/bin/sh
set -e

echo "Installing neo"
# Ensure gcc and make exist
if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc is required to build neo."
    exit 1
fi

# Compile and place in system path
gcc -Wall -Wextra -O2 -o /tmp/neo mycode.c
sudo mv /tmp/neo /usr/local/bin/neo
sudo chmod 755 /usr/local/bin/neo

echo "Successfully installed! Run 'neo <filename>' to use."
echo "Reload Your Terminal Shell Configuration to start using!!";
echo "Thank You For Using us!"
