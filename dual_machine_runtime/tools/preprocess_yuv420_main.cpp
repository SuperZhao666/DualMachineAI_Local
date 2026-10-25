#include "vfdual/image_preprocessor.hpp"
#include <fstream>
#include <iostream>
#include <vector>

// Offline diagnostic: feed exact decoded I420 pixels through production preprocessing.
// Both input and output contain consecutive 416x416 frames, with no file header.
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: VisionForgePreprocessI420 input.i420 output.u16nhwc\n"; return 2; }
    constexpr unsigned side = vfdual::kDefaultMobileModel.input.width;
    constexpr std::size_t plane = side * side;
    std::ifstream input(argv[1], std::ios::binary);
    std::ofstream output(argv[2], std::ios::binary);
    if (!input || !output) return 3;
    std::vector<std::uint8_t> yuv(plane * 3 / 2), tensor(plane * 6);
    std::size_t frames = 0;
    while (input.read(reinterpret_cast<char*>(yuv.data()), yuv.size())) {
        vfdual::Yuv420ImageView view{yuv.data(), yuv.data()+plane, yuv.data()+plane+plane/4,
            side, side, side, side/2, side/2, 1, 1};
        vfdual::LetterboxTransform transform;
        if (!vfdual::preprocess_yuv420_to_model_rgb(view, vfdual::YuvColorMatrix::bt709_limited,
                vfdual::kDefaultMobileModel.input, tensor, transform)) return 4;
        output.write(reinterpret_cast<const char*>(tensor.data()), tensor.size());
        if (!output) return 5;
        ++frames;
    }
    if (input.gcount() != 0 || !input.eof()) return 6;
    std::cout << "frames=" << frames << " width=" << side << " layout=RGB_NHWC_UINT16 scale=1/65535\n";
    return frames == 0 ? 7 : 0;
}
