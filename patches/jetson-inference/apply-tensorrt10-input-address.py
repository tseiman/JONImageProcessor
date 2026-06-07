#!/usr/bin/env python3
from pathlib import Path


path = Path("c/tensorNet.cpp")
text = path.read_text()

old_unused_binding_context_block = (
    "    #if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !mContext->setTensorAddress(bindingName, mBindings[n]) )\n"
    "        {\n"
    "            LogError(LOG_TRT \"failed to set tensor address for unused binding %s (%zu bytes)\\n\", bindingName, bindingSize);\n"
    "            return false;\n"
    "        }\n"
    "    #endif\n"
)

text = text.replace(old_unused_binding_context_block, "")

def replace_once(old: str, new: str, required: bool = True) -> None:
    global text
    if new in text:
        return
    if old not in text:
        if required:
            raise SystemExit(f"Expected source fragment not found in {path}")
        return
    text = text.replace(old, new, 1)


replace_once(
    "#if 0 && NV_TENSORRT_MAJOR >= 10\n"
    "        if( !mContext->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !context->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    required=False,
)

replace_once(
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !mContext->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !context->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    required=False,
)

replace_once(
    "LogError(LOG_TRT \"failed to set input tensor address for %s (%zu bytes)\\n\", inputSize, input_blobs[n].c_str());",
    "LogError(LOG_TRT \"failed to set input tensor address for %s (%zu bytes)\\n\", input_blobs[n].c_str(), inputSize);",
    required=False,
)

replace_once(
    "if( !mContext->setTensorAddress(output_blobs[n].c_str(), outputCUDA) )",
    "if( !context->setTensorAddress(output_blobs[n].c_str(), outputCUDA) )",
    required=False,
)

replace_once(
    "LogError(LOG_TRT \"failed to set input tensor address for %s (%zu bytes)\\n\", outputSize, output_blobs[n].c_str());",
    "LogError(LOG_TRT \"failed to set output tensor address for %s (%zu bytes)\\n\", output_blobs[n].c_str(), outputSize);",
    required=False,
)

replace_once(
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        nvinfer1::Dims outputDims = engine->getTensorShape(output_blobs[n].c_str());\n"
    "\t#elif NV_TENSORRT_MAJOR > 1",
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        nvinfer1::Dims outputDims = engine->getTensorShape(output_blobs[n].c_str());\n"
    "\n"
    "        if( mModelType == MODEL_ONNX )\n"
    "            outputDims = shiftDims(outputDims);  // change NCHW to CHW if EXPLICIT_BATCH set\n"
    "\t#elif NV_TENSORRT_MAJOR > 1",
    required=False,
)

replace_once(
    "const size_t bindingSize = sizeDims(validateDims(engine->getTensorShape(output_blobs[n].c_str()))) * mMaxBatchSize * sizeof(float);",
    "const char* bindingName = engine->getIOTensorName(n);\n"
    "        const size_t bindingSize = sizeDims(validateDims(engine->getTensorShape(bindingName))) * mMaxBatchSize * sizeof(float);",
    required=False,
)

replace_once(
    "\t\tLogVerbose(LOG_TRT \"allocated %zu bytes for unused binding %u\\n\", bindingSize, n);",
    "    #if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !context->setTensorAddress(bindingName, mBindings[n]) )\n"
    "        {\n"
    "            LogError(LOG_TRT \"failed to set tensor address for unused binding %s (%zu bytes)\\n\", bindingName, bindingSize);\n"
    "            return false;\n"
    "        }\n"
    "    #endif\n"
    "\t\tLogVerbose(LOG_TRT \"allocated %zu bytes for unused binding %u\\n\", bindingSize, n);",
    required=False,
)

replace_once(
    "if( !mContext->setTensorAddress(bindingName, mBindings[n]) )",
    "if( !context->setTensorAddress(bindingName, mBindings[n]) )",
    required=False,
)

text = text.replace(
    "if( !mContext->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    "if( !context->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
)
text = text.replace(
    "if( !mContext->setTensorAddress(output_blobs[n].c_str(), outputCUDA) )",
    "if( !context->setTensorAddress(output_blobs[n].c_str(), outputCUDA) )",
)

required_fragments = [
    "#if NV_TENSORRT_MAJOR >= 10\n"
    "        if( !context->setInputTensorAddress(input_blobs[n].c_str(), inputCUDA) )",
    "if( !context->setTensorAddress(output_blobs[n].c_str(), outputCUDA) )",
    "const char* bindingName = engine->getIOTensorName(n);",
    "if( !context->setTensorAddress(bindingName, mBindings[n]) )",
    "outputDims = shiftDims(outputDims);",
]

for fragment in required_fragments:
    if fragment not in text:
        raise SystemExit(f"Patch verification failed for {path}: missing {fragment!r}")

path.write_text(text)
print("Applied TensorRT 10 input tensor address patch to c/tensorNet.cpp")
