
cmake_minimum_required (VERSION 2.6)
project (MCServer)

include_directories ("${PROJECT_SOURCE_DIR}/../")

file(GLOB SOURCE
    "*.cpp"
)

add_library(HTTPServer ${SOURCE})
