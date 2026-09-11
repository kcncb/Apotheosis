cmake_minimum_required(VERSION 3.24)
project(ApotheosisUI LANGUAGES CXX)

# The functional UI is linked into the root ai target together with runtime
# sources. This directory is only a convenience entry for visual preview.
# Recursive globbing previously pulled in multiple main() files and runtime
# pages without their Windows/CUDA dependencies.
add_subdirectory(preview)
