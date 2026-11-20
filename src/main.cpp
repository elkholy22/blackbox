#include "options.hpp"
#include "supervisor.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace {

// Checked before the command starts, so a typo in -o does not cost a long run whose dump
// then cannot be written.
void check_dump_target(const std::string& path)
{
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) dir = ".";

    if (::access(dir.c_str(), W_OK | X_OK) != 0 ||
        (::access(path.c_str(), F_OK) == 0 && ::access(path.c_str(), W_OK) != 0)) {
        throw std::runtime_error(
            std::format("cannot write dump to {}: {}", path, std::strerror(errno)));
    }
}

} // namespace

int main(int argc, char** argv)
{
    using namespace blackbox;
    try {
        const Options options = parse_args(std::vector<const char*>(argv, argv + argc));
        if (options.help) {
            std::cout << kUsage;
            return 0;
        }
        if (options.version) {
            std::cout << "blackbox " << BLACKBOX_VERSION << '\n';
            return 0;
        }
        check_dump_target(options.out);
        return run(options);
    } catch (const UsageError& e) {
        std::cerr << "blackbox: " << e.what() << "\nTry 'blackbox --help' for more information.\n";
        return 125;
    } catch (const std::exception& e) {
        std::cerr << "blackbox: " << e.what() << '\n';
        return 125;
    }
}
