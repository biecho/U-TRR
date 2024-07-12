FROM --platform=linux/amd64 ubuntu:22.04

# Set an environment variable to prevent any interactive time zone config and user prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Run a series of commands within the Ubuntu image:
# 1. Update the package lists for the Ubuntu package manager
# 2. Install packages that are essential for building C/C++ applications and managing builds:
#    - build-essential: Includes the GCC compiler, make utility, and other essential tools
#    - gdb: GNU Debugger for debugging applications
#    - cmake: Cross-platform tool to manage the build process
#    - git: Version control system commonly used for source code management
# 3. Clean up the package list to reduce container size. This is done by removing the apt cache in /var/lib/apt/lists
RUN apt-get update && apt-get install -y \
    build-essential \
    gdb \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]
