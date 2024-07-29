/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jni_layer_constants.h"

#include <executorch/examples/models/llama2/runner/runner.h>
#include <executorch/extension/module/module.h>
#include <executorch/extension/runner_util/managed_tensor.h>
#include <executorch/runtime/core/portable_type/tensor_impl.h>
#include <executorch/runtime/platform/log.h>
#include <executorch/runtime/platform/platform.h>
#include <executorch/runtime/platform/runtime.h>

#if defined(ET_USE_THREADPOOL)
#include <executorch/backends/xnnpack/threadpool/cpuinfo_utils.h>
#include <executorch/backends/xnnpack/threadpool/threadpool.h>
#endif

#include <fbjni/ByteBuffer.h>
#include <fbjni/fbjni.h>

#ifdef __ANDROID__
#include <android/log.h>

// For Android, write to logcat
void et_pal_emit_log_message(
    et_timestamp_t timestamp,
    et_pal_log_level_t level,
    const char* filename,
    const char* function,
    size_t line,
    const char* message,
    size_t length) {
  int android_log_level = ANDROID_LOG_UNKNOWN;
  if (level == 'D') {
    android_log_level = ANDROID_LOG_DEBUG;
  } else if (level == 'I') {
    android_log_level = ANDROID_LOG_INFO;
  } else if (level == 'E') {
    android_log_level = ANDROID_LOG_ERROR;
  } else if (level == 'F') {
    android_log_level = ANDROID_LOG_FATAL;
  }

  __android_log_print(android_log_level, "ExecuTorch", "%s", message);
}
#endif

using namespace torch::executor;

namespace executorch_jni {
class TensorHybrid : public facebook::jni::HybridClass<TensorHybrid> {
 public:
  constexpr static const char* kJavaDescriptor =
      "Lorg/pytorch/executorch/Tensor;";

  explicit TensorHybrid(exec_aten::Tensor tensor) {}

  static facebook::jni::local_ref<TensorHybrid::javaobject>
  newJTensorFromTensor(const exec_aten::Tensor& tensor) {
    // Java wrapper currently only supports contiguous tensors.

    const auto scalarType = tensor.scalar_type();

    if (scalar_type_to_java_dtype.count(scalarType) == 0) {
      facebook::jni::throwNewJavaException(
          facebook::jni::gJavaLangIllegalArgumentException,
          "exec_aten::Tensor scalar type %d is not supported on java side",
          scalarType);
    }
    int jdtype = scalar_type_to_java_dtype.at(scalarType);

    const auto& tensor_shape = tensor.sizes();
    std::vector<jlong> tensor_shape_vec;
    for (const auto& s : tensor_shape) {
      tensor_shape_vec.push_back(s);
    }
    facebook::jni::local_ref<jlongArray> jTensorShape =
        facebook::jni::make_long_array(tensor_shape_vec.size());
    jTensorShape->setRegion(
        0, tensor_shape_vec.size(), tensor_shape_vec.data());

    static auto cls = TensorHybrid::javaClassStatic();
    // Note: this is safe as long as the data stored in tensor is valid; the
    // data won't go out of scope as long as the Method for the inference is
    // valid and there is no other inference call. Java layer picks up this
    // value immediately so the data is valid.
    facebook::jni::local_ref<facebook::jni::JByteBuffer> jTensorBuffer =
        facebook::jni::JByteBuffer::wrapBytes(
            (uint8_t*)tensor.data_ptr(), tensor.nbytes());
    jTensorBuffer->order(facebook::jni::JByteOrder::nativeOrder());

    static const auto jMethodNewTensor =
        cls->getStaticMethod<facebook::jni::local_ref<TensorHybrid::javaobject>(
            facebook::jni::alias_ref<facebook::jni::JByteBuffer>,
            facebook::jni::alias_ref<jlongArray>,
            jint,
            facebook::jni::alias_ref<jhybriddata>)>("nativeNewTensor");
    return jMethodNewTensor(
        cls, jTensorBuffer, jTensorShape, jdtype, makeCxxInstance(tensor));
  }

 private:
  friend HybridBase;
};

class JEValue : public facebook::jni::JavaClass<JEValue> {
 public:
  constexpr static const char* kJavaDescriptor =
      "Lorg/pytorch/executorch/EValue;";

  constexpr static int kTypeCodeTensor = 1;
  constexpr static int kTypeCodeString = 2;
  constexpr static int kTypeCodeDouble = 3;
  constexpr static int kTypeCodeInt = 4;
  constexpr static int kTypeCodeBool = 5;

  static facebook::jni::local_ref<JEValue> newJEValueFromEValue(EValue evalue) {
    if (evalue.isTensor()) {
      static auto jMethodTensor =
          JEValue::javaClassStatic()
              ->getStaticMethod<facebook::jni::local_ref<JEValue>(
                  facebook::jni::local_ref<TensorHybrid::javaobject>)>("from");
      return jMethodTensor(
          JEValue::javaClassStatic(),
          TensorHybrid::newJTensorFromTensor(evalue.toTensor()));
    } else if (evalue.isInt()) {
      static auto jMethodTensor =
          JEValue::javaClassStatic()
              ->getStaticMethod<facebook::jni::local_ref<JEValue>(jlong)>(
                  "from");
      return jMethodTensor(JEValue::javaClassStatic(), evalue.toInt());
    } else if (evalue.isDouble()) {
      static auto jMethodTensor =
          JEValue::javaClassStatic()
              ->getStaticMethod<facebook::jni::local_ref<JEValue>(jdouble)>(
                  "from");
      return jMethodTensor(JEValue::javaClassStatic(), evalue.toDouble());
    } else if (evalue.isBool()) {
      static auto jMethodTensor =
          JEValue::javaClassStatic()
              ->getStaticMethod<facebook::jni::local_ref<JEValue>(jboolean)>(
                  "from");
      return jMethodTensor(JEValue::javaClassStatic(), evalue.toBool());
    } else if (evalue.isString()) {
      static auto jMethodTensor =
          JEValue::javaClassStatic()
              ->getStaticMethod<facebook::jni::local_ref<JEValue>(
                  facebook::jni::local_ref<jstring>)>("from");
      std::string str =
          std::string(evalue.toString().begin(), evalue.toString().end());
      return jMethodTensor(
          JEValue::javaClassStatic(), facebook::jni::make_jstring(str));
    }
    facebook::jni::throwNewJavaException(
        facebook::jni::gJavaLangIllegalArgumentException,
        "Unsupported EValue type: %d",
        evalue.tag);
  }

  static ManagedTensor JEValueToTensorImpl(
      facebook::jni::alias_ref<JEValue> JEValue) {
    static const auto typeCodeField =
        JEValue::javaClassStatic()->getField<jint>("mTypeCode");
    const auto typeCode = JEValue->getFieldValue(typeCodeField);
    if (JEValue::kTypeCodeTensor == typeCode) {
      static const auto jMethodGetTensor =
          JEValue::javaClassStatic()
              ->getMethod<facebook::jni::alias_ref<TensorHybrid::javaobject>()>(
                  "toTensor");
      auto jtensor = jMethodGetTensor(JEValue);

      static auto cls = TensorHybrid::javaClassStatic();
      static const auto dtypeMethod = cls->getMethod<jint()>("dtypeJniCode");
      jint jdtype = dtypeMethod(jtensor);

      static const auto shapeField = cls->getField<jlongArray>("shape");
      auto jshape = jtensor->getFieldValue(shapeField);

      static auto dataBufferMethod = cls->getMethod<
          facebook::jni::local_ref<facebook::jni::JBuffer::javaobject>()>(
          "getRawDataBuffer");
      facebook::jni::local_ref<facebook::jni::JBuffer> jbuffer =
          dataBufferMethod(jtensor);

      const auto rank = jshape->size();

      const auto shapeArr = jshape->getRegion(0, rank);
      std::vector<exec_aten::SizesType> shape_vec;
      shape_vec.reserve(rank);

      auto numel = 1;
      for (int i = 0; i < rank; i++) {
        shape_vec.push_back(shapeArr[i]);
      }
      for (int i = rank - 1; i >= 0; --i) {
        numel *= shapeArr[i];
      }
      JNIEnv* jni = facebook::jni::Environment::current();
      if (java_dtype_to_scalar_type.count(jdtype) == 0) {
        facebook::jni::throwNewJavaException(
            facebook::jni::gJavaLangIllegalArgumentException,
            "Unknown Tensor jdtype %d",
            jdtype);
      }
      ScalarType scalar_type = java_dtype_to_scalar_type.at(jdtype);
      const auto dataCapacity = jni->GetDirectBufferCapacity(jbuffer.get());
      if (dataCapacity != numel) {
        facebook::jni::throwNewJavaException(
            facebook::jni::gJavaLangIllegalArgumentException,
            "Tensor dimensions(elements number:%d inconsistent with buffer capacity(%d)",
            numel,
            dataCapacity);
      }
      return ManagedTensor(
          jni->GetDirectBufferAddress(jbuffer.get()), shape_vec, scalar_type);
    }
    facebook::jni::throwNewJavaException(
        facebook::jni::gJavaLangIllegalArgumentException,
        "Unknown EValue typeCode %d",
        typeCode);
  }
};

class ExecuTorchJni : public facebook::jni::HybridClass<ExecuTorchJni> {
 private:
  friend HybridBase;
  std::unique_ptr<Module> module_;

 public:
  constexpr static auto kJavaDescriptor = "Lorg/pytorch/executorch/NativePeer;";

  static facebook::jni::local_ref<jhybriddata> initHybrid(
      facebook::jni::alias_ref<jclass>,
      facebook::jni::alias_ref<jstring> modelPath,
      facebook::jni::alias_ref<
          facebook::jni::JMap<facebook::jni::JString, facebook::jni::JString>>
          extraFiles) {
    return makeCxxInstance(modelPath, extraFiles);
  }

  ExecuTorchJni(
      facebook::jni::alias_ref<jstring> modelPath,
      facebook::jni::alias_ref<
          facebook::jni::JMap<facebook::jni::JString, facebook::jni::JString>>
          extraFiles) {
    module_ = std::make_unique<Module>(
        modelPath->toStdString(), Module::LoadMode::Mmap);
  }

  facebook::jni::local_ref<facebook::jni::JArrayClass<JEValue>> forward(
      facebook::jni::alias_ref<
          facebook::jni::JArrayClass<JEValue::javaobject>::javaobject>
          jinputs) {
    return execute_method("forward", jinputs);
  }

  facebook::jni::local_ref<facebook::jni::JArrayClass<JEValue>> execute(
      facebook::jni::alias_ref<jstring> methodName,
      facebook::jni::alias_ref<
          facebook::jni::JArrayClass<JEValue::javaobject>::javaobject>
          jinputs) {
    return execute_method(methodName->toStdString(), jinputs);
  }

  jint load_method(facebook::jni::alias_ref<jstring> methodName) {
    return static_cast<jint>(module_->load_method(methodName->toStdString()));
  }

  facebook::jni::local_ref<facebook::jni::JArrayClass<JEValue>> execute_method(
      std::string method,
      facebook::jni::alias_ref<
          facebook::jni::JArrayClass<JEValue::javaobject>::javaobject>
          jinputs) {
    std::vector<EValue> evalues = {};

    std::vector<ManagedTensor> managed_tensors = {};

    static const auto typeCodeField =
        JEValue::javaClassStatic()->getField<jint>("mTypeCode");

    for (int i = 0; i < jinputs->size(); i++) {
      auto jevalue = jinputs->getElement(i);
      const auto typeCode = jevalue->getFieldValue(typeCodeField);
      if (typeCode == JEValue::kTypeCodeTensor) {
        managed_tensors.emplace_back(JEValue::JEValueToTensorImpl(jevalue));
        evalues.emplace_back(
            EValue(managed_tensors.back().get_aliasing_tensor()));
      } else if (typeCode == JEValue::kTypeCodeInt) {
        int64_t value = jevalue->getFieldValue(typeCodeField);
        evalues.emplace_back(EValue(value));
      } else if (typeCode == JEValue::kTypeCodeDouble) {
        double value = jevalue->getFieldValue(typeCodeField);
        evalues.emplace_back(EValue(value));
      } else if (typeCode == JEValue::kTypeCodeBool) {
        bool value = jevalue->getFieldValue(typeCodeField);
        evalues.emplace_back(EValue(value));
      }
    }

#ifdef EXECUTORCH_ANDROID_PROFILING
    auto start = std::chrono::high_resolution_clock::now();
    auto result = module_->execute(method, evalues);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();
    ET_LOG(Debug, "Execution time: %lld ms.", duration);

#else
    auto result = module_->execute(method, evalues);

#endif

    if (!result.ok()) {
      facebook::jni::throwNewJavaException(
          "java/lang/Exception",
          "Execution of method %s failed with status 0x%" PRIx32,
          method.c_str(),
          static_cast<error_code_t>(result.error()));
      return {};
    }

    facebook::jni::local_ref<facebook::jni::JArrayClass<JEValue>> jresult =
        facebook::jni::JArrayClass<JEValue>::newArray(result.get().size());

    for (int i = 0; i < result.get().size(); i++) {
      auto jevalue = JEValue::newJEValueFromEValue(result.get()[i]);
      jresult->setElement(i, *jevalue);
    }

    return jresult;
  }

  static void registerNatives() {
    registerHybrid({
        makeNativeMethod("initHybrid", ExecuTorchJni::initHybrid),
        makeNativeMethod("forward", ExecuTorchJni::forward),
        makeNativeMethod("execute", ExecuTorchJni::execute),
        makeNativeMethod("loadMethod", ExecuTorchJni::load_method),
    });
  }
};

class ExecuTorchLlamaCallbackJni
    : public facebook::jni::JavaClass<ExecuTorchLlamaCallbackJni> {
 public:
  constexpr static const char* kJavaDescriptor =
      "Lorg/pytorch/executorch/LlamaCallback;";

  void onResult(std::string result) const {
    static auto cls = ExecuTorchLlamaCallbackJni::javaClassStatic();
    static const auto method =
        cls->getMethod<void(facebook::jni::local_ref<jstring>)>("onResult");
    facebook::jni::local_ref<jstring> s = facebook::jni::make_jstring(result);
    method(self(), s);
  }

  void onStats(const Runner::Stats& result) const {
    static auto cls = ExecuTorchLlamaCallbackJni::javaClassStatic();
    static const auto method = cls->getMethod<void(jfloat)>("onStats");
    double eval_time =
        (double)(result.inference_end_ms - result.prompt_eval_end_ms);

    float tps = result.num_generated_tokens / eval_time *
        result.SCALING_FACTOR_UNITS_PER_SECOND;

    method(self(), tps);
  }
};

class ExecuTorchLlamaJni
    : public facebook::jni::HybridClass<ExecuTorchLlamaJni> {
 private:
  friend HybridBase;
  std::unique_ptr<Runner> runner_;

 public:
  constexpr static auto kJavaDescriptor =
      "Lorg/pytorch/executorch/LlamaModule;";

  static facebook::jni::local_ref<jhybriddata> initHybrid(
      facebook::jni::alias_ref<jclass>,
      facebook::jni::alias_ref<jstring> model_path,
      facebook::jni::alias_ref<jstring> tokenizer_path,
      jfloat temperature) {
    return makeCxxInstance(model_path, tokenizer_path, temperature);
  }

  ExecuTorchLlamaJni(
      facebook::jni::alias_ref<jstring> model_path,
      facebook::jni::alias_ref<jstring> tokenizer_path,
      jfloat temperature) {
#if defined(ET_USE_THREADPOOL)
    // Reserve 1 thread for the main thread.
    uint32_t num_performant_cores =
        torch::executorch::cpuinfo::get_num_performant_cores() - 1;
    if (num_performant_cores > 0) {
      ET_LOG(Info, "Resetting threadpool to %d threads", num_performant_cores);
      torch::executorch::threadpool::get_threadpool()->_unsafe_reset_threadpool(
          num_performant_cores);
    }
#endif

    runner_ = std::make_unique<Runner>(
        model_path->toStdString().c_str(),
        tokenizer_path->toStdString().c_str(),
        temperature);
  }

  jint generate(
      facebook::jni::alias_ref<jstring> prompt,
      facebook::jni::alias_ref<ExecuTorchLlamaCallbackJni> callback) {
    runner_->generate(
        prompt->toStdString(),
        128,
        [callback](std::string result) { callback->onResult(result); },
        [callback](const Runner::Stats& result) { callback->onStats(result); });
    return 0;
  }

  void stop() {
    runner_->stop();
  }

  jint load() {
    return static_cast<jint>(runner_->load());
  }

  static void registerNatives() {
    registerHybrid({
        makeNativeMethod("initHybrid", ExecuTorchLlamaJni::initHybrid),
        makeNativeMethod("generate", ExecuTorchLlamaJni::generate),
        makeNativeMethod("stop", ExecuTorchLlamaJni::stop),
        makeNativeMethod("load", ExecuTorchLlamaJni::load),
    });
  }
};

} // namespace executorch_jni

static std::vector<void (*)(void)> jni_init_function_registry;
int register_jni_init_function(void (*f)(void)) {
  jni_init_function_registry.push_back(f);
  return 0;
}

static auto register_jni_init_function_success = register_jni_init_function(executorch_jni::ExecuTorchJni::registerNatives);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::jni::initialize(
      vm, [] {
        for (auto& f : jni_init_function_registry) {
          f();
        }
      });
}
