#pragma once

#ifndef FACE_RECOGNITION_STANDALONE_CLI_HPP
#define FACE_RECOGNITION_STANDALONE_CLI_HPP

#include <string>
#include <vector>
#include <unordered_map>

namespace face_recognition_standalone {

struct CliArgs {
    std::string command;                    // run | add | list | remove | clear | help
    std::unordered_map<std::string, std::string> opts;
    std::vector<std::string> positional;
};

CliArgs parse_cli(int argc, char** argv);

void print_usage(const char* prog);

}  // namespace face_recognition_standalone

#endif
