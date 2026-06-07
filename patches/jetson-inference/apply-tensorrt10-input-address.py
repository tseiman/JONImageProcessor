#!/usr/bin/env python3
from pathlib import Path


path = Path("c/tensorNet.cpp")
text = path.read_text()

replacements = [
    (
        "#if 0 && NV_TENSORRT_MAJOR >= 10\n"
        "        if( !mContext->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
        "#if NV_TENSORRT_MAJOR >= 10\n"
        "        if( !mContext->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    ),
    (
        "const size_t bindingSize = sizeDims(validateDims(engine->getTensorShape(output_blobs[n].c_str()))) * mMaxBatchSize * sizeof(float);",
        "const char* bindingName = engine->getIOTensorName(n);\n"
        "        const size_t bindingSize = sizeDims(validateDims(engine->getTensorShape(bindingName))) * mMaxBatchSize * sizeof(float);",
    ),
    (
        "\t\tLogVerbose(LOG_TRT \"allocated %zu bytes for unused binding %u\\n\", bindingSize, n);",
        "    #if NV_TENSORRT_MAJOR >= 10\n"
        "        if( !mContext->setTensorAddress(bindingName, mBindings[n]) )\n"
        "        {\n"
        "            LogError(LOG_TRT \"failed to set tensor address for unused binding %s (%zu bytes)\\n\", bindingName, bindingSize);\n"
        "            return false;\n"
        "        }\n"
        "    #endif\n"
        "\t\tLogVerbose(LOG_TRT \"allocated %zu bytes for unused binding %u\\n\", bindingSize, n);",
    ),
]

for old, new in replacements:
    if new in text:
        continue
    if old not in text:
        raise SystemExit(f"Expected source fragment not found in {path}")
    text = text.replace(old, new, 1)

path.write_text(text)
print("Applied TensorRT 10 input tensor address patch to c/tensorNet.cpp")
