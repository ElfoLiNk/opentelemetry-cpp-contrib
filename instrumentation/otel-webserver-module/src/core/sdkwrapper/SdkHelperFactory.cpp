/*
* Copyright 2022, OpenTelemetry Authors. 
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "sdkwrapper/SdkHelperFactory.h"
#include "sdkwrapper/SdkConstants.h"
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/simple_processor.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/trace/samplers/always_on.h"
#include "opentelemetry/sdk/trace/samplers/always_off.h"
#include "opentelemetry/sdk/trace/samplers/parent.h"
#include "opentelemetry/sdk/trace/samplers/trace_id_ratio.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/baggage/propagation/baggage_propagator.h"
#include <module_version.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <chrono>
#include <fstream>
#include <iostream>

namespace otel {
namespace core {
namespace sdkwrapper {

// NOTE : all the validation checks related to otel inputs are done in the helper factory.
// So, these constants are not required outside of this execution unit
namespace {
  constexpr const char* OTLP_EXPORTER_TYPE = "otlp";
  constexpr const char* OSSTREAM_EXPORTER_TYPE = "osstream";
  constexpr const char* SIMPLE_PROCESSOR = "simple";
  constexpr const char* BATCH_PROCESSOR = "batch";
  constexpr const char* ALWAYS_ON_SAMPLER = "always_on";
  constexpr const char* ALWAYS_OFF_SAMPLER = "always_off";
  constexpr const char* PARENT_BASED_SAMPLER = "parent";
  constexpr const char* PARENT_BASED_ALWAYS_ON_SAMPLER = "parentbased_always_on";
  constexpr const char* PARENT_BASED_ALWAYS_OFF_SAMPLER = "parentbased_always_off";
  constexpr const char* TRACE_ID_RATIO_BASED_SAMPLER = "trace_id_ratio";
  constexpr const char* TRACE_ID_RATIO_BASED_SAMPLER_SPEC = "traceidratio";
  constexpr const char* PARENT_BASED_TRACE_ID_RATIO_SAMPLER = "parentbased_traceidratio";
  constexpr const char* kInstrumentationScopeName = "otel-webserver-module";
  constexpr const char* PROPAGATOR_TYPE_B3 = "b3";
  constexpr const char* PROTOCOL_GRPC = "grpc";
  constexpr const char* PROTOCOL_HTTP_PROTOBUF = "http/protobuf";
  constexpr const char* PROTOCOL_HTTP_JSON = "http/json";

  // The README and the sample opentelemetry_module.conf document the sampler and
  // processor values as "AlwaysOn", "AlwaysOff" and "Batch", while the constants
  // above are "always_on", "always_off" and "batch". An exact comparison makes the
  // documented values fall through to the default, so "AlwaysOff" silently samples
  // everything. Compare ignoring case, '_' and '-' so both spellings are accepted.
  std::string normalizeType(const std::string& value)
  {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
      if (c == '_' || c == '-') {
        continue;
      }
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
  }

  bool isType(const std::string& value, const char* type)
  {
    return normalizeType(value) == normalizeType(type);
  }
}

namespace {
  std::mutex& providersMutex()
  {
    static std::mutex m;
    return m;
  }

  std::vector<std::shared_ptr<opentelemetry::sdk::trace::TracerProvider>>& providers()
  {
    static std::vector<std::shared_ptr<opentelemetry::sdk::trace::TracerProvider>> v;
    return v;
  }

  void registerProvider(std::shared_ptr<opentelemetry::sdk::trace::TracerProvider> provider)
  {
    std::lock_guard<std::mutex> lock(providersMutex());
    providers().push_back(std::move(provider));
  }
}

void SdkHelperFactory::ShutdownAll(const AgentLogger& logger)
{
    std::vector<std::shared_ptr<opentelemetry::sdk::trace::TracerProvider>> toShutdown;
    {
        std::lock_guard<std::mutex> lock(providersMutex());
        toShutdown.swap(providers());
    }
    for (auto& provider : toShutdown) {
        // Shutdown() drains the batch processor queue and exports it before stopping.
        // Bound it: the default timeout is unlimited, and an unreachable collector
        // must not keep an exiting Apache child alive.
        bool ok = provider->Shutdown(std::chrono::seconds(5));
        if (logger) {
            LOG4CXX_INFO(logger, "TracerProvider shutdown " << (ok ? "completed" : "failed"));
        }
    }
}

SdkHelperFactory::SdkHelperFactory(
    std::shared_ptr<TenantConfig> config,
    const AgentLogger& logger) :
    mLogger(logger)
{
    // Create exporter, processor and provider.

    // TODO:: Use constant expressions
    LOG4CXX_INFO(mLogger, "ServiceNamespace: " << config->getServiceNamespace() <<
        " ServiceName: " << config->getServiceName() <<
        " ServiceInstanceId: " << config->getServiceInstanceId());
    sdk::resource::ResourceAttributes attributes;
    attributes[kServiceName] = config->getServiceName();
    attributes[kServiceNamespace] = config->getServiceNamespace();
    attributes[kServiceInstanceId] = config->getServiceInstanceId();

    // NOTE : resource attribute values are nostd::variant and so we need to explicitely set it to std::string
    std::string libraryVersion = MODULE_VERSION;
    std::string cppSDKVersion = CPP_SDK_VERSION;

    // NOTE : InstrumentationLibrary code is incomplete for the otlp exporter in sdk.
    // So, we need to pass libraryName and libraryVersion as resource attributes.
    // Ref : https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/src/otlp_recordable.cc
    
    //Library was changed to webengine to comply with specs https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/resource/semantic_conventions/webengine.md
    attributes[kOtelWebEngineName] = config->getOtelLibraryName();
    //attributes[kOtelWebEngineVersion] = libraryVersion;

    //attributes[kOtelWebEngineDescription] = config->getOtelLibraryName() + " Instrumentation";

    auto exporter = GetExporter(config);
    auto processor = GetSpanProcessor(config, std::move(exporter));
    auto sampler = GetSampler(config);

    mSdkTracerProvider = std::make_shared<opentelemetry::sdk::trace::TracerProvider>(
            std::move(processor),
            opentelemetry::sdk::resource::Resource::Create(attributes),
            std::move(sampler));
    mTracerProvider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(
        mSdkTracerProvider);
    registerProvider(mSdkTracerProvider);

    // The instrumentation scope is this module, not the C++ SDK it is built on (the
    // SDK already reports itself through the telemetry.sdk.* resource attributes).
    mTracer = mTracerProvider->GetTracer(kInstrumentationScopeName, libraryVersion);
    LOG4CXX_INFO(mLogger,
        "Tracer created with LibraryName: " << kInstrumentationScopeName <<
        " and LibraryVersion " << libraryVersion << " (opentelemetry-cpp " << cppSDKVersion << ")");

    // Adding trace propagator
    auto propagatorType = config->getOtelPropagatorType();
    LOG4CXX_INFO(mLogger, "Propagator type used to read and export Trace and Span info : " << propagatorType);
    if(propagatorType == PROPAGATOR_TYPE_B3){
        mPropagators.push_back(
            std::unique_ptr<opentelemetry::trace::propagation::B3PropagatorMultiHeader>(
                new opentelemetry::trace::propagation::B3PropagatorMultiHeader()));
    }
    else{
        mPropagators.push_back(
            std::unique_ptr<opentelemetry::trace::propagation::HttpTraceContext>(
                new opentelemetry::trace::propagation::HttpTraceContext()));
    }
    
}

OtelTracer SdkHelperFactory::GetTracer()
{
    return mTracer;
}

OtelPropagators& SdkHelperFactory::GetPropagators()
{
  return mPropagators;
}

OtelSpanExporter SdkHelperFactory::GetExporter(
    std::shared_ptr<TenantConfig> config)
{
    auto exporter = std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>{};
    auto type = config->getOtelExporterType();

    if (type == OSSTREAM_EXPORTER_TYPE) {
        exporter.reset(new opentelemetry::exporter::trace::OStreamSpanExporter);
    } else {
        if (type != OTLP_EXPORTER_TYPE) {
          LOG4CXX_WARN(mLogger, "Received unknown exporter type: " << type << ". Will create default(otlp) exporter");
          type = OTLP_EXPORTER_TYPE;
        }

        auto protocol = config->getOtelExporterOtlpProtocol();
        // Default to gRPC, the only protocol earlier releases supported, so an
        // existing configuration keeps exporting after an upgrade. Defaulting to
        // http/protobuf would send the same "host:port" endpoint to the HTTP
        // exporter, which expects a full URL, and silently stop the export.
        if (protocol.empty()) {
            protocol = PROTOCOL_GRPC;
        }

        LOG4CXX_INFO(mLogger, "OTLP exporter protocol: " << protocol);

        if (protocol == PROTOCOL_GRPC) {
            opentelemetry::exporter::otlp::OtlpGrpcExporterOptions opts;
            opts.endpoint = config->getOtelExporterEndpoint();
            if (config->getOtelExportTimeoutMillis() > 0) {
                opts.timeout = std::chrono::milliseconds(config->getOtelExportTimeoutMillis());
            }
            if (config->getOtelSslEnabled()) {
                opts.use_ssl_credentials = config->getOtelSslEnabled();
                opts.ssl_credentials_cacert_path = config->getOtelSslCertPath();
                LOG4CXX_TRACE(mLogger, "Ssl Credentials are enabled for exporter, path: "
                    << opts.ssl_credentials_cacert_path);
            }

            opentelemetry::common::KeyValueStringTokenizer tokenizer{config->getOtelExporterOtlpHeaders()};
            opentelemetry::nostd::string_view header_key;
            opentelemetry::nostd::string_view header_value;
            bool header_valid = true;
            std::unordered_set<std::string> remove_cache;

            while (tokenizer.next(header_valid, header_key, header_value))
            {
                if (header_valid)
                {
                    std::string key = static_cast<std::string>(header_key);
                    if (remove_cache.end() == remove_cache.find(key))
                    {
                        remove_cache.insert(key);
                        auto range = opts.metadata.equal_range(key);
                        if (range.first != range.second)
                        {
                            opts.metadata.erase(range.first, range.second);
                        }
                    }

                    opts.metadata.emplace(std::make_pair(std::move(key), static_cast<std::string>(header_value)));
                }
            }

            exporter.reset(new opentelemetry::exporter::otlp::OtlpGrpcExporter(opts));
        } else {
            if (protocol != PROTOCOL_HTTP_PROTOBUF && protocol != PROTOCOL_HTTP_JSON) {
                LOG4CXX_WARN(mLogger, "Received unknown OTLP protocol: " << protocol
                    << ". Will use default(http/protobuf)");
            }

            opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
            opts.url = config->getOtelExporterEndpoint();
            if (config->getOtelExportTimeoutMillis() > 0) {
                opts.timeout = std::chrono::milliseconds(config->getOtelExportTimeoutMillis());
            }

            if (protocol == PROTOCOL_HTTP_JSON) {
                opts.content_type = opentelemetry::exporter::otlp::HttpRequestContentType::kJson;
            }

            if (config->getOtelSslEnabled()) {
                opts.ssl_ca_cert_path = config->getOtelSslCertPath();
                LOG4CXX_TRACE(mLogger, "Ssl Credentials are enabled for exporter, path: "
                    << opts.ssl_ca_cert_path);
            }

            opentelemetry::common::KeyValueStringTokenizer tokenizer{config->getOtelExporterOtlpHeaders()};
            opentelemetry::nostd::string_view header_key;
            opentelemetry::nostd::string_view header_value;
            bool header_valid = true;
            std::unordered_set<std::string> remove_cache;

            while (tokenizer.next(header_valid, header_key, header_value))
            {
                if (header_valid)
                {
                    std::string key = static_cast<std::string>(header_key);
                    if (remove_cache.end() == remove_cache.find(key))
                    {
                        remove_cache.insert(key);
                        auto range = opts.http_headers.equal_range(key);
                        if (range.first != range.second)
                        {
                            opts.http_headers.erase(range.first, range.second);
                        }
                    }

                    opts.http_headers.emplace(std::make_pair(std::move(key), static_cast<std::string>(header_value)));
                }
            }

            exporter.reset(new opentelemetry::exporter::otlp::OtlpHttpExporter(opts));
        }
    }

    LOG4CXX_INFO(mLogger, "Exporter created with ExporterType: "
        << type);
    return exporter;
}

OtelSpanProcessor SdkHelperFactory::GetSpanProcessor(
    std::shared_ptr<TenantConfig> config,
    OtelSpanExporter exporter)
{
    auto processor = OtelSpanProcessor{};
    auto type = config->getOtelProcessorType();
    if (isType(type, SIMPLE_PROCESSOR)) {
        processor.reset(
                new opentelemetry::sdk::trace::SimpleSpanProcessor(std::move(exporter)));
    } else {
        if (!isType(type, BATCH_PROCESSOR)) {
           // default is batch processor
           LOG4CXX_WARN(mLogger, "Received unknown processor type: " << type << ". Will create default(batch) processor");
           type = BATCH_PROCESSOR;
        }
        // ApacheModuleOtelMaxQueueSize, ...ScheduledDelay and ...MaxExportBatchSize are
        // parsed into the TenantConfig; 0 means unset, so keep the SDK default then.
        opentelemetry::sdk::trace::BatchSpanProcessorOptions options;
        if (config->getOtelMaxQueueSize() > 0) {
            options.max_queue_size = config->getOtelMaxQueueSize();
        }
        if (config->getOtelScheduledDelayMillis() > 0) {
            options.schedule_delay_millis =
                std::chrono::milliseconds(config->getOtelScheduledDelayMillis());
        }
        if (config->getOtelMaxExportBatchSize() > 0) {
            options.max_export_batch_size = config->getOtelMaxExportBatchSize();
        }
        // The SDK rejects nothing here, but a batch larger than the queue can never fill.
        if (options.max_export_batch_size > options.max_queue_size) {
            LOG4CXX_WARN(mLogger, "MaxExportBatchSize " << options.max_export_batch_size
                << " is larger than MaxQueueSize " << options.max_queue_size
                << ", using MaxQueueSize");
            options.max_export_batch_size = options.max_queue_size;
        }
        LOG4CXX_INFO(mLogger, "Batch processor options: max_queue_size=" << options.max_queue_size
            << " schedule_delay_millis=" << options.schedule_delay_millis.count()
            << " max_export_batch_size=" << options.max_export_batch_size);
        processor.reset(
            new opentelemetry::sdk::trace::BatchSpanProcessor(std::move(exporter), options));
    }

    LOG4CXX_INFO(mLogger, "Processor created with ProcessorType: "
        << type);
    return processor;
}

OtelSampler SdkHelperFactory::GetSampler(
    std::shared_ptr<TenantConfig> config)
{
    auto type = config->getOtelSamplerType();

    // Ratio for the trace id ratio samplers, from the standard OTEL_TRACES_SAMPLER_ARG
    // (the module has no directive for it). Defaults to 1.0, out of range clamps.
    auto ratio = [this]() {
        double value = 1.0;
        const char* arg = std::getenv("OTEL_TRACES_SAMPLER_ARG");
        if (arg && *arg) {
            try {
                value = std::stod(arg);
            } catch (const std::exception&) {
                LOG4CXX_WARN(mLogger, "Invalid OTEL_TRACES_SAMPLER_ARG: " << arg << ". Using 1.0");
                value = 1.0;
            }
        }
        return std::min(1.0, std::max(0.0, value));
    };

    auto parentBased = [](std::shared_ptr<sdk::trace::Sampler> root) {
        return OtelSampler(new sdk::trace::ParentBasedSampler(std::move(root)));
    };

    auto sampler = OtelSampler{};
    std::string description;
    if (type.empty()) {
        // The SDK specification default.
        sampler = parentBased(std::make_shared<sdk::trace::AlwaysOnSampler>());
        description = "parentbased_always_on (default)";
    } else if (isType(type, ALWAYS_ON_SAMPLER)) {
        sampler.reset(new sdk::trace::AlwaysOnSampler);
        description = ALWAYS_ON_SAMPLER;
    } else if (isType(type, ALWAYS_OFF_SAMPLER)) {
        sampler.reset(new sdk::trace::AlwaysOffSampler);
        description = ALWAYS_OFF_SAMPLER;
    } else if (isType(type, PARENT_BASED_SAMPLER) || isType(type, PARENT_BASED_ALWAYS_ON_SAMPLER)) {
        // These two used to be a TODO that left the sampler null.
        sampler = parentBased(std::make_shared<sdk::trace::AlwaysOnSampler>());
        description = PARENT_BASED_ALWAYS_ON_SAMPLER;
    } else if (isType(type, PARENT_BASED_ALWAYS_OFF_SAMPLER)) {
        sampler = parentBased(std::make_shared<sdk::trace::AlwaysOffSampler>());
        description = PARENT_BASED_ALWAYS_OFF_SAMPLER;
    } else if (isType(type, TRACE_ID_RATIO_BASED_SAMPLER) || isType(type, TRACE_ID_RATIO_BASED_SAMPLER_SPEC)) {
        sampler.reset(new sdk::trace::TraceIdRatioBasedSampler(ratio()));
        description = TRACE_ID_RATIO_BASED_SAMPLER_SPEC;
    } else if (isType(type, PARENT_BASED_TRACE_ID_RATIO_SAMPLER)) {
        sampler = parentBased(std::make_shared<sdk::trace::TraceIdRatioBasedSampler>(ratio()));
        description = PARENT_BASED_TRACE_ID_RATIO_SAMPLER;
    } else {
        LOG4CXX_WARN(mLogger, "Received unknown sampler type: " << type
            << ". Will create default(parentbased_always_on) sampler");
        sampler = parentBased(std::make_shared<sdk::trace::AlwaysOnSampler>());
        description = "parentbased_always_on (default)";
    }

    LOG4CXX_INFO(mLogger, "Sampler created with SamplerType : " << description);
    return sampler;
}

} //sdkwrapper
} //core
} //otel

