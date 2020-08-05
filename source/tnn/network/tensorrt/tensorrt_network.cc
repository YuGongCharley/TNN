// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <memory>

#include "tnn/network/tensorrt/tensorrt_network.h"
#include "tnn/interpreter/default_model_interpreter.h"
#include "tnn/network/tensorrt/exclusive_file.h"
#include "tnn/extern_wrapper/foreign_blob.h"
#include "tnn/device/cuda/cuda_context.h"

namespace TNN_NS {

#define MAX_SCRATCH_MEMORY (1<<25 - 1)

NetworkImplFactoryRegister<NetworkImplFactory<TensorRTNetwork>> g_network_impl_tensorrt_factory_register(NETWORK_TYPE_TENSORRT);

TensorRTNetwork::TensorRTNetwork() : m_plugin_factory(this) {
    m_trt_builder = nullptr;
    m_trt_network = nullptr;
    m_trt_engine = nullptr;
    m_trt_context = nullptr;
}

Status TensorRTNetwork::Init(NetworkConfig &net_config, ModelConfig &model_config, AbstractModelInterpreter* interpreter,
        InputShapesMap inputs_shape) {
    DefaultModelInterpreter *default_interpreter = dynamic_cast<DefaultModelInterpreter *>(interpreter);
    CHECK_PARAM_NULL(default_interpreter);

    NetStructure *net_structure = default_interpreter->GetNetStructure();
    NetResource *net_resource   = default_interpreter->GetNetResource();

    if (net_structure == NULL || net_resource == NULL) {
        LOGE("ERROR: network_ is nil, network_type may not support\n");
        return Status(TNNERR_NULL_PARAM, "network_ is nil, network_type may not support");
    }
    device_ = GetDevice(net_config.device_type);
    if (device_ == NULL) {
        return TNNERR_DEVICE_NOT_SUPPORT;
    }

    blob_manager_ = new TensorRTBlobManager(device_);
    Status ret = blob_manager_->Init(net_config, net_structure, inputs_shape, GetNetResourceDataType(net_resource));
    if (ret != TNN_OK) {
        return ret;
    }

    ret = InitLayers(net_structure, net_resource);
    if (ret != TNN_OK) {
        return ret;
    }

    ret = blob_manager_->AllocateBlobMemory();
    if (ret != TNN_OK) {
        return ret;
    }

    this->m_max_batchsize = 1;

    BlobMap inputs;
    ret = blob_manager_->GetAllInputBlobs(inputs);
    if (ret != TNN_OK) {
        LOGE("ERROR: get input blobs failed");
        return ret;
    }

    for (auto iter : inputs) {
        if (iter.second->GetBlobDesc().dims[0] > this->m_max_batchsize) {
            this->m_max_batchsize = iter.second->GetBlobDesc().dims[0];
        }
    }

    // TODO(johnzlli) need generat file name with md5sum, device_id and some other params.
    std::string file_name = GetCacheFileName();
    ExclFile *file_lock = new ExclFile(file_name);

    BlobMap outputs;
    ret = blob_manager_->GetAllOutputBlobs(outputs);
    if (ret != TNN_OK) {
        LOGE("ERROR: get output blobs failed");
        return ret;
    }

    std::string cache_file_name = GetCacheFileName();

    if (false == file_lock->Ready()) {
        this->m_trt_builder = nvinfer1::createInferBuilder(m_trt_logger);
        this->m_trt_network = m_trt_builder->createNetwork();

        for (auto input : inputs) {
            auto desc = input.second->GetBlobDesc();
            nvinfer1::DimsCHW in_dim(desc.dims[1], desc.dims[2], desc.dims[3]);

            nvinfer1::ITensor* in_tensor = this->m_trt_network->addInput(desc.name.c_str(),
                nvinfer1::DataType::kFLOAT, in_dim);
            auto foreign_blob = new ForeignBlob(input.second);
            auto tensorrtTensor = std::make_shared<TensorRTTensor>();

            tensorrtTensor->SetTensor(in_tensor);
            foreign_blob->SetForeignTensor(tensorrtTensor);
            blob_manager_->ReplaceBlob(desc.name, foreign_blob);
        }

        for (int layer_id = 0; layer_id < this->layers_.size(); layer_id++) {
            BaseLayer* cur_layer = this->layers_[layer_id];
            nvinfer1::ILayer *cur_trt_layer = dynamic_cast<TensorRTBaseLayerBuilder*>(cur_layer)->AddToNetwork(this->m_trt_network);
            for (int out_id = 0; out_id < cur_layer->GetOutputBlobs().size(); out_id++) {
                auto output = cur_layer->GetOutputBlobs()[out_id];
                nvinfer1::ITensor* out_tensor = cur_trt_layer->getOutput(out_id);
                auto foreign_blob = new ForeignBlob(output);
                auto tensorrtTensor = std::make_shared<TensorRTTensor>();
                tensorrtTensor->SetTensor(out_tensor);
                foreign_blob->SetForeignTensor(tensorrtTensor);
                blob_manager_->ReplaceBlob(output->GetBlobDesc().name, foreign_blob);
            }
        }

        for (auto output : outputs) {
            auto foreign_tensor = dynamic_cast<ForeignBlob*>(output.second)->GetForeignTensor();
            auto tensor = std::dynamic_pointer_cast<TensorRTTensor>(foreign_tensor)->GetTensor();
            this->m_trt_network->markOutput(*tensor);
        }

        this->m_trt_builder->setMaxBatchSize(m_max_batchsize);
        m_trt_engine = m_trt_builder->buildCudaEngine(*m_trt_network);
        CreateExecuteContext();

        IHostMemory *model_stream = nullptr;
        model_stream = m_trt_engine->serialize();

        std::ofstream deploy_output(cache_file_name);
        char *model_stream_ptr = reinterpret_cast<char*>(model_stream->data());
        deploy_output.write(model_stream_ptr, model_stream->size());
        deploy_output.close();
        delete model_stream_ptr;
    } else {
        size_t size = 0;
        std::ifstream deploy_input(cache_file_name, std::ios::binary);
        deploy_input.seekg(0, deploy_input.end);
        size = deploy_input.tellg();
        deploy_input.seekg(0, deploy_input.beg);
        char *model_stream = new char[size];
        deploy_input.read(model_stream, size);
        IRuntime* runtime = createInferRuntime(m_trt_logger);
        m_trt_engine = runtime->deserializeCudaEngine(model_stream, size, &m_plugin_factory);
        CreateExecuteContext();
        runtime->destroy();
        delete[] model_stream;
        deploy_input.close();
    }

    delete file_lock;
    int bind_num = m_trt_engine->getNbBindings();
    this->m_trt_bindings = new void*[bind_num];

    for (auto iter : inputs) {
        int index = m_trt_engine->getBindingIndex(iter.second->GetBlobDesc().name.c_str());
        this->m_trt_bindings[index] = iter.second->GetHandle().base;
    }

    for (auto iter : outputs) {
        int index = m_trt_engine->getBindingIndex(iter.second->GetBlobDesc().name.c_str());
        this->m_trt_bindings[index] = iter.second->GetHandle().base;
    }

    return TNN_OK;
}

Status TensorRTNetwork::Forward() {
    bool ret = this->m_trt_context->enqueue(this->m_max_batchsize, this->m_trt_bindings,
        dynamic_cast<CudaContext*>(context_)->GetStream(), nullptr);
    if (ret != true) {
        return TNNERR_CUDA_TENSORRT_ERROR;
    }
    ret = context_->Synchronize();
    if (ret != TNN_OK) {
        return ret;
    }
    return TNN_OK;
}

Status TensorRTNetwork::ForwardAsync(Callback call_back) {
     bool ret = this->m_trt_context->enqueue(this->m_max_batchsize, this->m_trt_bindings,
        dynamic_cast<CudaContext*>(context_)->GetStream(), nullptr);
    if (ret != true) {
        return TNNERR_CUDA_TENSORRT_ERROR;
    }
    return TNN_OK;
}

std::unordered_map<std::string, TensorRTPluginLayerBuilder*> TensorRTNetwork::GetPluginLayerNameMap() {
    return m_plugin_layer_name_map;
}

Status TensorRTNetwork::InitLayers(NetStructure *net_structure, NetResource *net_resource) {
    Status ret = TNN_OK;

    for (auto layer_info : net_structure->layers) {
        LayerType type = layer_info->type;
        TensorRTBaseLayerBuilder *cur_layer = CreateTensorRTBaseLayerBuilder(type);
        if (nullptr == cur_layer) {
            LOGE("Error: CreateLayer failed, type:%d\n", type);
            return Status(TNNERR_PARAM_ERR, "CreateLayer failed");
        }
        std::string layer_name = layer_info->name;
        cur_layer->SetLayerName(layer_name);
        // set layer nodes
        std::vector<Blob *> inputs;
        std::vector<std::string> &input_names = layer_info->inputs;
        // get input nodes
        for (auto name : input_names) {
            auto blob = blob_manager_->GetBlob(name);
            inputs.push_back(blob);
        }
        std::vector<Blob *> outputs;
        std::vector<std::string> &output_names = layer_info->outputs;

        for (auto name : output_names) {
            auto blob = blob_manager_->GetBlob(name);
            outputs.push_back(blob);
        }

        LayerResource *layer_resource = net_resource->resource_map[layer_name].get();
        ret = cur_layer->Init(context_, layer_info->param.get(), layer_resource, inputs, outputs, device_);

        if (ret != TNN_OK) {
            LOGE("Error Init layer %s (err: %d or 0x%X)\n", cur_layer->GetLayerName().c_str(), (int)ret, (int)ret);
            return ret;
        }

        layers_.push_back(cur_layer);
    }
    return ret;
}


void TensorRTNetwork::CreateExecuteContext() {
    m_trt_context = m_trt_engine->createExecutionContextWithoutDeviceMemory();
    void* device_memory = dynamic_cast<TensorRTBlobManager*>(blob_manager_)->GetContextMemory();
    m_trt_context->setDeviceMemory(device_memory);
}

std::string TensorRTNetwork::GetCacheFileName() {
    return ".cache";
}

}  //  TNN_NS