// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <vector>
#include <fstream>
#include <memory>

#include <inference_engine.hpp>

#include <samples/common.hpp>
#include <samples/classification_results.h>

using namespace InferenceEngine;

/**
 * \brief Parse image size provided as string in format WIDTHxHEIGHT
 * @return parsed width and height
 */
std::pair<size_t, size_t> parseImageSize(const std::string& size_string) {
    auto delimiter_pos = size_string.find("x");
    if (delimiter_pos == std::string::npos
     || delimiter_pos >= size_string.size() - 1
     || delimiter_pos == 0) {
        std::stringstream err;
        err << "Incorrect format of image size parameter, expected WIDTHxHEIGHT, "
               "actual: " << size_string;
        throw std::runtime_error(err.str());
    }

    size_t width = static_cast<size_t>(
        std::stoull(size_string.substr(0, delimiter_pos)));
    size_t height = static_cast<size_t>(
        std::stoull(size_string.substr(delimiter_pos + 1, size_string.size())));

    if (width == 0 || height == 0) {
        throw std::runtime_error(
            "Incorrect format of image size parameter, width and height must not be equal to 0");
    }

    if (width % 2 != 0 || height % 2 != 0) {
        throw std::runtime_error("Unsupported image size, width and height must be even numbers");
    }

    return {width, height};
}

/**
 * \brief Read image data from file
 * @return buffer containing the image data
 */
std::unique_ptr<unsigned char[]> readImageDataFromFile(const std::string& image_path, size_t size) {
    std::ifstream file(image_path, std::ios_base::ate | std::ios_base::binary);
    if (!file.good() || !file.is_open()) {
        std::stringstream err;
        err << "Cannot access input image file. File path: " << image_path;
        throw std::runtime_error(err.str());
    }

    const size_t file_size = file.tellg();
    if (file_size < size) {
        std::stringstream err;
        err << "Invalid read size provided. File size: " << file_size << ", to read: " << size;
        throw std::runtime_error(err.str());
    }
    file.seekg(0);

    std::unique_ptr<unsigned char[]> data(new unsigned char[size]);
    file.read(reinterpret_cast<char*>(data.get()), size);
    return data;
}

/**
 * \brief Sets batch size of the network to the specified value
 */
void setBatchSize(CNNNetwork& network, size_t batch) {
    ICNNNetwork::InputShapes inputShapes = network.getInputShapes();
    for (auto& shape : inputShapes) {
        auto& dims = shape.second;
        if (dims.empty()) {
            throw std::runtime_error("Network's input shapes have empty dimensions");
        }
        dims[0] = batch;
    }
    network.reshape(inputShapes);
}

/**
* @brief The entry point of the Inference Engine sample application
*/
int main(int argc, char *argv[]) {
    try {
        // ------------------------------ Parsing and validatiing input arguments------------------------------
        if (argc != 5) {
            std::cout << "Usage : ./hello_nv12_input_classification <path_to_model> <path_to_image> <image_size> <device_name>"
                      << std::endl;
            return EXIT_FAILURE;
        }

        const std::string input_model{argv[1]};
        const std::string input_image_path{argv[2]};
        size_t input_width = 0, input_height = 0;
        std::tie(input_width, input_height) = parseImageSize(argv[3]);
        const std::string device_name{argv[4]};
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load inference engine ------------------------------------------------
        Core ie;
        // -----------------------------------------------------------------------------------------------------

        // 2. Read a model in OpenVINO Intermediate Representation (.xml and .bin files) or ONNX (.onnx file) format
        CNNNetwork network = ie.ReadNetwork(input_model);
        setBatchSize(network, 1);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Configure input and output -------------------------------------------
        // --------------------------- Prepare input blobs -----------------------------------------------------
        if (network.getInputsInfo().empty()) {
            std::cerr << "Network inputs info is empty" << std::endl;
            return EXIT_FAILURE;
        }
        InputInfo::Ptr input_info = network.getInputsInfo().begin()->second;
        std::string input_name = network.getInputsInfo().begin()->first;

        input_info->setLayout(Layout::NCHW);
        input_info->setPrecision(Precision::U8);
        // set input resize algorithm to enable input autoresize
        input_info->getPreProcess().setResizeAlgorithm(ResizeAlgorithm::RESIZE_BILINEAR);
        // set input color format to ColorFormat::NV12 to enable automatic input color format
        // pre-processing
        input_info->getPreProcess().setColorFormat(ColorFormat::NV12);

        // --------------------------- Prepare output blobs ----------------------------------------------------
        if (network.getOutputsInfo().empty()) {
            std::cerr << "Network outputs info is empty" << std::endl;
            return EXIT_FAILURE;
        }
        DataPtr output_info = network.getOutputsInfo().begin()->second;
        std::string output_name = network.getOutputsInfo().begin()->first;

        output_info->setPrecision(Precision::FP32);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 4. Loading a model to the device ----------------------------------------
        ExecutableNetwork executable_network = ie.LoadNetwork(network, device_name);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 5. Create an infer request ----------------------------------------------
        InferRequest infer_request = executable_network.CreateInferRequest();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Prepare input --------------------------------------------------------
        // read image with size converted to NV12 data size: height(NV12) = 3 / 2 * logical height
        auto image_buf = readImageDataFromFile(input_image_path, input_width * (input_height * 3 / 2));

        // --------------------------- Create a blob to hold the NV12 input data -------------------------------
        // Create tensor descriptors for Y and UV blobs
        InferenceEngine::TensorDesc y_plane_desc(InferenceEngine::Precision::U8,
            {1, 1, input_height, input_width}, InferenceEngine::Layout::NHWC);
        InferenceEngine::TensorDesc uv_plane_desc(InferenceEngine::Precision::U8,
            {1, 2, input_height / 2, input_width / 2}, InferenceEngine::Layout::NHWC);
        const size_t offset = input_width * input_height;

        // Create blob for Y plane from raw data
        Blob::Ptr y_blob = make_shared_blob<uint8_t>(y_plane_desc, image_buf.get());
        // Create blob for UV plane from raw data
        Blob::Ptr uv_blob = make_shared_blob<uint8_t>(uv_plane_desc, image_buf.get() + offset);
        // Create NV12Blob from Y and UV blobs
        Blob::Ptr input = make_shared_blob<NV12Blob>(y_blob, uv_blob);

        // --------------------------- Set the input blob to the InferRequest ----------------------------------
        infer_request.SetBlob(input_name, input);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 7. Do inference ---------------------------------------------------------
        /* Running the request synchronously */
        infer_request.Infer();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 8. Process output -------------------------------------------------------
        Blob::Ptr output = infer_request.GetBlob(output_name);

        // Print classification results
        ClassificationResult classificationResult(output, {input_image_path});
        classificationResult.print();
        // -----------------------------------------------------------------------------------------------------
    } catch (const std::exception & ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "This sample is an API example, for any performance measurements "
                 "please use the dedicated benchmark_app tool" << std::endl;
    return EXIT_SUCCESS;
}