#include "hdcodex/core/display_transform.h"

#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int Usage()
{
    std::cerr << "Usage: hdCodexDisplayTransform <linear-input> <display-output> "
                 "[--exposure <stops>]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
try {
    if (argc != 3 && argc != 5) return Usage();
    float exposure = 0.0F;
    if (argc == 5) {
        if (std::string(argv[3]) != "--exposure") return Usage();
        exposure = std::stof(argv[4]);
        if (!std::isfinite(exposure) || exposure < -20.0F || exposure > 20.0F) {
            std::cerr << "Exposure must be finite and between -20 and 20 stops.\n";
            return 2;
        }
    }

    const HioImageSharedPtr input = HioImage::OpenForReading(
        argv[1], 0, 0, HioImage::Raw, false);
    if (!input || input->GetWidth() <= 0 || input->GetHeight() <= 0) {
        std::cerr << "Could not open linear input image: " << argv[1] << '\n';
        return 1;
    }

    const int width = input->GetWidth();
    const int height = input->GetHeight();
    const std::size_t pixelCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> linear(pixelCount * 4U);
    HioImage::StorageSpec inputStorage;
    inputStorage.width = width;
    inputStorage.height = height;
    inputStorage.depth = 1;
    inputStorage.format = HioFormatFloat32Vec4;
    inputStorage.flipped = false;
    inputStorage.data = linear.data();
    if (!input->Read(inputStorage)) {
        std::cerr << "Could not decode linear input image: " << argv[1] << '\n';
        return 1;
    }

    std::vector<std::uint8_t> display(pixelCount * 4U);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const auto encoded = hdcodex::SceneLinearToDisplaySrgb({
            linear[pixel * 4U], linear[pixel * 4U + 1U],
            linear[pixel * 4U + 2U]}, exposure);
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            display[pixel * 4U + channel] = static_cast<std::uint8_t>(
                std::lround(std::clamp(encoded[channel], 0.0F, 1.0F) * 255.0F));
        }
        const float alpha = std::isfinite(linear[pixel * 4U + 3U])
            ? linear[pixel * 4U + 3U] : 1.0F;
        display[pixel * 4U + 3U] = static_cast<std::uint8_t>(
            std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F));
    }

    const HioImageSharedPtr output = HioImage::OpenForWriting(argv[2]);
    if (!output) {
        std::cerr << "Could not open display output image: " << argv[2] << '\n';
        return 1;
    }
    HioImage::StorageSpec outputStorage;
    outputStorage.width = width;
    outputStorage.height = height;
    outputStorage.depth = 1;
    outputStorage.format = HioFormatUNorm8Vec4;
    outputStorage.flipped = false;
    outputStorage.data = display.data();
    if (!output->Write(outputStorage)) {
        std::cerr << "Could not write display output image: " << argv[2] << '\n';
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Display transform failed: " << error.what() << '\n';
    return 1;
}
