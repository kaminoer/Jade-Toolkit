// rli_rescale_cli — run the native zone atmosphere grade (for the harness).
//
//   rli_rescale_cli <file.bf> <brightness> <tintR> <tintG> <tintB> <contrast>
//                   <district_filter|-> <lights 0|1> <textures 0|1> [--log]
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "jade/RliRescale.hpp"

int main(int argc, char** argv) {
    if ((argc != 10 && argc != 11) ||
        (argc == 11 && std::string(argv[10]) != "--log")) {
        std::cerr << "usage: rli_rescale_cli <file.bf> <brightness> <tintR> <tintG>"
                     " <tintB> <contrast> <district|-> <lights 0|1> <textures 0|1>"
                     " [--log]\n";
        return 2;
    }
    double brightness = std::strtod(argv[2], nullptr);
    std::array<int, 3> tint{int(std::strtol(argv[3], nullptr, 10)),
                            int(std::strtol(argv[4], nullptr, 10)),
                            int(std::strtol(argv[5], nullptr, 10))};
    double contrast = std::strtod(argv[6], nullptr);
    std::string district = argv[7];
    if (district == "-") district.clear();
    bool lights = std::string(argv[8]) == "1";
    bool textures = std::string(argv[9]) == "1";
    jade::RescaleLogFn log;
    if (argc == 11)
        log = [](const std::string& line) { std::cout << line << '\n'; };

    jade::RescaleStats st = jade::rescale_zone_in_bf(argv[1], brightness, tint,
                                                     contrast, district, lights,
                                                     textures, std::move(log));
    if (!st.ok) {
        std::cerr << "rli_rescale_cli: error: " << st.error << "\n";
        return 1;
    }
    std::printf("RESCALE bins=%u gaos=%u lights=%u textures=%u\n",
                st.bins, st.gaos, st.lights, st.textures);
    return 0;
}
