/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/event_context.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/track.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/track_event/track_event_config.gen.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

#include <type_traits>

namespace perfetto {

struct TraceTimestamp {
  protos::pbzero::BuiltinClock clock_id;
  uint64_t nanoseconds;
};

// A function for converting an abstract timestamp into the trace clock timebase
// in nanoseconds. By overriding this template the user can register additional
// timestamp types. The return value should specify the clock used by the
// timestamp as well as its value in nanoseconds.
template <typename T>
TraceTimestamp ConvertTimestampToTraceTimeNs(const T&);

// A pass-through implementation for raw uint64_t nanosecond timestamps.
template <>
TraceTimestamp inline ConvertTimestampToTraceTimeNs(const uint64_t& timestamp) {
  return {internal::TrackEventInternal::GetClockId(), timestamp};
}

namespace internal {
namespace {

// A template helper for determining whether a type can be used as a track event
// lambda, i.e., it has the signature "void(EventContext)". This is achieved by
// checking that we can pass an EventContext value (the inner declval) into a T
// instance (the outer declval). If this is a valid expression, the result
// evaluates to sizeof(0), i.e., true.
// TODO(skyostil): Replace this with std::is_convertible<std::function<...>>
// once we have C++14.
template <typename T>
static constexpr bool IsValidTraceLambdaImpl(
    typename std::enable_if<static_cast<bool>(
        sizeof(std::declval<T>()(std::declval<EventContext>()), 0))>::type* =
        nullptr) {
  return true;
}

template <typename T>
static constexpr bool IsValidTraceLambdaImpl(...) {
  return false;
}

template <typename T>
static constexpr bool IsValidTraceLambda() {
  return IsValidTraceLambdaImpl<T>(nullptr);
}

// Because the user can use arbitrary timestamp types, we can't compare against
// any known base type here. Instead, we check that a track or a trace lambda
// isn't being interpreted as a timestamp.
template <typename T,
          typename CanBeConvertedToNsCheck = decltype(
              ::perfetto::ConvertTimestampToTraceTimeNs(std::declval<T>())),
          typename NotTrackCheck = typename std::enable_if<
              !std::is_convertible<T, Track>::value>::type,
          typename NotLambdaCheck =
              typename std::enable_if<!IsValidTraceLambda<T>()>::type>
static constexpr bool IsValidTimestamp() {
  return true;
}

}  // namespace

// Traits for dynamic categories.
template <typename CategoryType>
struct CategoryTraits {
  static constexpr bool kIsDynamic = true;
  static constexpr const Category* GetStaticCategory(
      const TrackEventCategoryRegistry*,
      const CategoryType&) {
    return nullptr;
  }
  static size_t GetStaticIndex(const CategoryType&) {
    PERFETTO_DCHECK(false);  // Not reached.
    return TrackEventCategoryRegistry::kDynamicCategoryIndex;
  }
  static DynamicCategory GetDynamicCategory(const CategoryType& category) {
    return DynamicCategory{category};
  }
};

// Traits for static categories.
template <>
struct CategoryTraits<size_t> {
  static constexpr bool kIsDynamic = false;
  static const Category* GetStaticCategory(
      const TrackEventCategoryRegistry* registry,
      size_t category_index) {
    return registry->GetCategory(category_index);
  }
  static constexpr size_t GetStaticIndex(size_t category_index) {
    return category_index;
  }
  static DynamicCategory GetDynamicCategory(size_t) {
    PERFETTO_DCHECK(false);  // Not reached.
    return DynamicCategory();
  }
};

struct TrackEventDataSourceTraits : public perfetto::DefaultDataSourceTraits {
  using IncrementalStateType = TrackEventIncrementalState;

  // Use a one shared TLS slot so that all track event data sources write into
  // the same sequence and share interning dictionaries.
  static DataSourceThreadLocalState* GetDataSourceTLS(DataSourceStaticState*,
                                                      TracingTLS* root_tls) {
    return &root_tls->track_event_tls;
  }
};

// A helper that ensures movable debug annotations are passed by value to
// minimize binary size at the call site, while allowing non-movable and
// non-copiable arguments to be passed by reference.
// TODO(skyostil): Remove this with C++17.
template <typename T>
struct DebugAnnotationArg {
  using type = typename std::
      conditional<std::is_move_constructible<T>::value, T, T&&>::type;
};

// A generic track event data source which is instantiated once per track event
// category namespace.
template <typename DataSourceType, const TrackEventCategoryRegistry* Registry>
class TrackEventDataSource
    : public DataSource<DataSourceType, TrackEventDataSourceTraits> {
  using Base = DataSource<DataSourceType, TrackEventDataSourceTraits>;

 public:
  // Add or remove a session observer for this track event data source. The
  // observer will be notified about started and stopped tracing sessions.
  // Returns |true| if the observer was succesfully added (i.e., the maximum
  // number of observers wasn't exceeded).
  static bool AddSessionObserver(TrackEventSessionObserver* observer) {
    return TrackEventInternal::AddSessionObserver(observer);
  }

  static void RemoveSessionObserver(TrackEventSessionObserver* observer) {
    TrackEventInternal::RemoveSessionObserver(observer);
  }

  // DataSource implementation.
  void OnSetup(const DataSourceBase::SetupArgs& args) override {
    auto config_raw = args.config->track_event_config_raw();
    bool ok = config_.ParseFromArray(config_raw.data(), config_raw.size());
    PERFETTO_DCHECK(ok);
    TrackEventInternal::EnableTracing(*Registry, config_, args);
  }

  void OnStart(const DataSourceBase::StartArgs& args) override {
    TrackEventInternal::OnStart(args);
  }

  void OnStop(const DataSourceBase::StopArgs& args) override {
    TrackEventInternal::DisableTracing(*Registry, args);
  }

  static void Flush() {
    Base::template Trace([](typename Base::TraceContext ctx) { ctx.Flush(); });
  }

  // Determine if *any* tracing category is enabled.
  static bool IsEnabled() {
    bool enabled = false;
    Base::template CallIfEnabled(
        [&](uint32_t /*instances*/) { enabled = true; });
    return enabled;
  }

  // Determine if tracing for the given static category is enabled.
  static bool IsCategoryEnabled(size_t category_index) {
    return Registry->GetCategoryState(category_index)
        ->load(std::memory_order_relaxed);
  }

  // Determine if tracing for the given dynamic category is enabled.
  static bool IsDynamicCategoryEnabled(
      const DynamicCategory& dynamic_category) {
    bool enabled = false;
    Base::template Trace([&](typename Base::TraceContext ctx) {
      enabled = IsDynamicCategoryEnabled(&ctx, dynamic_category);
    });
    return enabled;
  }

  // This is the inlined entrypoint for all track event trace points. It tries
  // to be as lightweight as possible in terms of instructions and aims to
  // compile down to an unlikely conditional jump to the actual trace writing
  // function.
  template <typename Callback>
  static void CallIfCategoryEnabled(size_t category_index,
                                    Callback callback) PERFETTO_ALWAYS_INLINE {
    Base::template CallIfEnabled<CategoryTracePointTraits>(
        [&callback](uint32_t instances) { callback(instances); },
        {category_index});
  }

  // Once we've determined tracing to be enabled for this category, actually
  // write a trace event onto this thread's default track. Outlined to avoid
  // bloating code (mostly stack depth) at the actual trace point.
  //
  // To minimize call overhead at each trace point, we provide the following
  // trace point argument variants:
  //
  // - None
  // - Lambda
  // - Lambda + timestamp
  // - One debug annotation
  // - Two debug annotations
  // - Track
  // - Track + Lambda
  // - Track + timestamp
  // - Track + Lambda + timestamp
  // - Track + one debug annotation
  // - Track + two debug annotations

  // Trace point which takes no arguments.
  template <typename CategoryType>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type);
  }

  // Trace point which takes a lambda function argument.
  template <typename CategoryType,
            typename ArgumentFunction = void (*)(EventContext),
            typename ArgumentFunctionCheck = typename std::enable_if<
                IsValidTraceLambda<ArgumentFunction>()>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               ArgumentFunction arg_function)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type,
                         TrackEventInternal::kDefaultTrack,
                         TrackEventInternal::GetTimeNs(),
                         std::move(arg_function));
  }

  // Trace point which takes a lambda function argument and an overridden
  // timestamp. |timestamp| must be in nanoseconds in the trace clock timebase.
  template <
      typename CategoryType,
      typename TimestampType = uint64_t,
      typename TimestampTypeCheck =
          typename std::enable_if<IsValidTimestamp<TimestampType>()>::type,
      typename ArgumentFunction = void (*)(EventContext),
      typename ArgumentFunctionCheck =
          typename std::enable_if<IsValidTraceLambda<ArgumentFunction>()>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               TimestampType timestamp,
                               ArgumentFunction arg_function)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type,
                         TrackEventInternal::kDefaultTrack, timestamp,
                         std::move(arg_function));
  }

  // This variant of the inner trace point takes a Track argument which can be
  // used to emit events on a non-default track.
  template <typename CategoryType,
            typename TrackType,
            typename TrackTypeCheck = typename std::enable_if<
                std::is_convertible<TrackType, Track>::value>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type, track);
  }

  // Trace point with a track and a lambda function.
  template <typename TrackType,
            typename CategoryType,
            typename ArgumentFunction = void (*)(EventContext),
            typename ArgumentFunctionCheck = typename std::enable_if<
                IsValidTraceLambda<ArgumentFunction>()>::type,
            typename TrackTypeCheck = typename std::enable_if<
                std::is_convertible<TrackType, Track>::value>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track,
                               ArgumentFunction arg_function)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type, track,
                         TrackEventInternal::GetTimeNs(),
                         std::move(arg_function));
  }

  // Trace point with a track and overridden timestamp.
  template <typename CategoryType,
            typename TrackType,
            typename TimestampType = uint64_t,
            typename TimestampTypeCheck = typename std::enable_if<
                IsValidTimestamp<TimestampType>()>::type,
            typename TrackTypeCheck = typename std::enable_if<
                std::is_convertible<TrackType, Track>::value>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track,
                               TimestampType timestamp) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type, track,
                         timestamp);
  }

  // Trace point with a track, a lambda function and an overridden timestamp.
  // |timestamp| must be in nanoseconds in the trace clock timebase.
  template <
      typename TrackType,
      typename CategoryType,
      typename TimestampType = uint64_t,
      typename TimestampTypeCheck =
          typename std::enable_if<IsValidTimestamp<TimestampType>()>::type,
      typename ArgumentFunction = void (*)(EventContext),
      typename ArgumentFunctionCheck =
          typename std::enable_if<IsValidTraceLambda<ArgumentFunction>()>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track,
                               TimestampType timestamp,
                               ArgumentFunction arg_function)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type, track,
                         timestamp, std::move(arg_function));
  }

  // Trace point with one debug annotation.
  //
  // This type of trace point is implemented with an inner helper function which
  // ensures |arg_value| is only passed by reference when required (i.e., with a
  // custom DebugAnnotation type). This avoids the binary and runtime overhead
  // of unnecessarily passing all types debug annotations by reference.
  //
  // Note that for this to work well, the _outer_ function (this function) has
  // to be inlined at the call site while the _inner_ function
  // (TraceForCategoryWithDebugAnnotations) is still outlined to minimize
  // overall binary size.
  template <typename CategoryType, typename ArgType>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const char* arg_name,
                               ArgType&& arg_value) PERFETTO_ALWAYS_INLINE {
    TraceForCategoryWithDebugAnnotations<CategoryType, Track, ArgType>(
        instances, category, event_name, type,
        TrackEventInternal::kDefaultTrack, arg_name,
        std::forward<ArgType>(arg_value));
  }

  // A one argument trace point which takes an explicit track.
  template <typename CategoryType, typename TrackType, typename ArgType>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track,
                               const char* arg_name,
                               ArgType&& arg_value) PERFETTO_ALWAYS_INLINE {
    PERFETTO_DCHECK(track);
    TraceForCategoryWithDebugAnnotations<CategoryType, TrackType, ArgType>(
        instances, category, event_name, type, track, arg_name,
        std::forward<ArgType>(arg_value));
  }

  template <typename CategoryType, typename TrackType, typename ArgType>
  static void TraceForCategoryWithDebugAnnotations(
      uint32_t instances,
      const CategoryType& category,
      const char* event_name,
      perfetto::protos::pbzero::TrackEvent::Type type,
      const TrackType& track,
      const char* arg_name,
      typename internal::DebugAnnotationArg<ArgType>::type arg_value)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(
        instances, category, event_name, type, track,
        TrackEventInternal::GetTimeNs(), [&](EventContext event_ctx) {
          TrackEventInternal::AddDebugAnnotation(&event_ctx, arg_name,
                                                 arg_value);
        });
  }

  // Trace point with two debug annotations. Note that we only support up to two
  // direct debug annotations. For more complicated arguments, you should
  // define your own argument type in track_event.proto and use a lambda to fill
  // it in your trace point.
  template <typename CategoryType, typename ArgType, typename ArgType2>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const char* arg_name,
                               ArgType&& arg_value,
                               const char* arg_name2,
                               ArgType2&& arg_value2) PERFETTO_ALWAYS_INLINE {
    TraceForCategoryWithDebugAnnotations<CategoryType, Track, ArgType,
                                         ArgType2>(
        instances, category, event_name, type,
        TrackEventInternal::kDefaultTrack, arg_name,
        std::forward<ArgType>(arg_value), arg_name2,
        std::forward<ArgType2>(arg_value2));
  }

  // A two argument trace point which takes an explicit track.
  template <typename CategoryType,
            typename TrackType,
            typename ArgType,
            typename ArgType2>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               const TrackType& track,
                               const char* arg_name,
                               ArgType&& arg_value,
                               const char* arg_name2,
                               ArgType2&& arg_value2) PERFETTO_ALWAYS_INLINE {
    PERFETTO_DCHECK(track);
    TraceForCategoryWithDebugAnnotations<CategoryType, TrackType, ArgType,
                                         ArgType2>(
        instances, category, event_name, type, track, arg_name,
        std::forward<ArgType>(arg_value), arg_name2,
        std::forward<ArgType2>(arg_value2));
  }

  template <typename CategoryType,
            typename TrackType,
            typename ArgType,
            typename ArgType2>
  static void TraceForCategoryWithDebugAnnotations(
      uint32_t instances,
      const CategoryType& category,
      const char* event_name,
      perfetto::protos::pbzero::TrackEvent::Type type,
      const TrackType& track,
      const char* arg_name,
      typename internal::DebugAnnotationArg<ArgType>::type arg_value,
      const char* arg_name2,
      typename internal::DebugAnnotationArg<ArgType2>::type arg_value2)
      PERFETTO_NO_INLINE {
    TraceForCategoryImpl(
        instances, category, event_name, type, track,
        TrackEventInternal::GetTimeNs(), [&](EventContext event_ctx) {
          TrackEventInternal::AddDebugAnnotation(&event_ctx, arg_name,
                                                 arg_value);
          TrackEventInternal::AddDebugAnnotation(&event_ctx, arg_name2,
                                                 arg_value2);
        });
  }

  // Initialize the track event library. Should be called before tracing is
  // enabled.
  static bool Register() {
    // Registration is performed out-of-line so users don't need to depend on
    // DataSourceDescriptor C++ bindings.
    return TrackEventInternal::Initialize(
        *Registry,
        [](const DataSourceDescriptor& dsd) { return Base::Register(dsd); });
  }

  // Record metadata about different types of timeline tracks. See Track.
  static void SetTrackDescriptor(const Track& track,
                                 const protos::gen::TrackDescriptor& desc) {
    PERFETTO_DCHECK(track.uuid == desc.uuid());
    TrackRegistry::Get()->UpdateTrack(track, desc.SerializeAsString());
    Base::template Trace([&](typename Base::TraceContext ctx) {
      TrackEventInternal::WriteTrackDescriptor(
          track, ctx.tls_inst_->trace_writer.get());
    });
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetTrackDescriptor(
      const Track& track,
      std::function<void(protos::pbzero::TrackDescriptor*)> callback) {
    SetTrackDescriptorImpl(track, std::move(callback));
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetProcessDescriptor(
      std::function<void(protos::pbzero::TrackDescriptor*)> callback,
      const ProcessTrack& track = ProcessTrack::Current()) {
    SetTrackDescriptorImpl(std::move(track), std::move(callback));
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetThreadDescriptor(
      std::function<void(protos::pbzero::TrackDescriptor*)> callback,
      const ThreadTrack& track = ThreadTrack::Current()) {
    SetTrackDescriptorImpl(std::move(track), std::move(callback));
  }

  static void EraseTrackDescriptor(const Track& track) {
    TrackRegistry::Get()->EraseTrack(track);
  }

  // Returns the current trace timestamp in nanoseconds. Note the returned
  // timebase may vary depending on the platform, but will always match the
  // timestamps recorded by track events (see GetTraceClockId).
  static uint64_t GetTraceTimeNs() { return TrackEventInternal::GetTimeNs(); }

  // Returns the type of clock used by GetTraceTimeNs().
  static constexpr protos::pbzero::BuiltinClock GetTraceClockId() {
    return TrackEventInternal::GetClockId();
  }

 private:
  // Each category has its own enabled/disabled state, stored in the category
  // registry.
  struct CategoryTracePointTraits {
    // Each trace point with a static category has an associated category index.
    struct TracePointData {
      size_t category_index;
    };
    // Called to get the enabled state bitmap of a given category.
    // |data| is the trace point data structure given to
    // DataSource::TraceWithInstances.
    static constexpr std::atomic<uint8_t>* GetActiveInstances(
        TracePointData data) {
      return Registry->GetCategoryState(data.category_index);
    }
  };

  template <
      typename CategoryType,
      typename TrackType = Track,
      typename TimestampType = uint64_t,
      typename TimestampTypeCheck =
          typename std::enable_if<IsValidTimestamp<TimestampType>()>::type,
      typename ArgumentFunction = void (*)(EventContext),
      typename ArgumentFunctionCheck =
          typename std::enable_if<IsValidTraceLambda<ArgumentFunction>()>::type,
      typename TrackTypeCheck = typename std::enable_if<
          std::is_convertible<TrackType, Track>::value>::type>
  static void TraceForCategoryImpl(
      uint32_t instances,
      const CategoryType& category,
      const char* event_name,
      perfetto::protos::pbzero::TrackEvent::Type type,
      const TrackType& track = TrackEventInternal::kDefaultTrack,
      TimestampType timestamp = TrackEventInternal::GetTimeNs(),
      ArgumentFunction arg_function = [](EventContext) {
      }) PERFETTO_ALWAYS_INLINE {
    using CatTraits = CategoryTraits<CategoryType>;
    const Category* static_category =
        CatTraits::GetStaticCategory(Registry, category);
    TraceWithInstances(
        instances, category, [&](typename Base::TraceContext ctx) {
          // If this category is dynamic, first check whether it's enabled.
          if (CatTraits::kIsDynamic &&
              !IsDynamicCategoryEnabled(
                  &ctx, CatTraits::GetDynamicCategory(category))) {
            return;
          }

          // TODO(skyostil): Support additional clock ids.
          TraceTimestamp trace_timestamp =
              ::perfetto::ConvertTimestampToTraceTimeNs(timestamp);
          PERFETTO_DCHECK(trace_timestamp.clock_id ==
                          TrackEventInternal::GetClockId());

          // Make sure incremental state is valid.
          TraceWriterBase* trace_writer = ctx.tls_inst_->trace_writer.get();
          TrackEventIncrementalState* incr_state = ctx.GetIncrementalState();
          if (incr_state->was_cleared) {
            incr_state->was_cleared = false;
            TrackEventInternal::ResetIncrementalState(
                trace_writer, trace_timestamp.nanoseconds);
          }

          // Write the track descriptor before any event on the track.
          if (track) {
            TrackEventInternal::WriteTrackDescriptorIfNeeded(
                track, trace_writer, incr_state);
          }

          // Write the event itself.
          {
            auto event_ctx = TrackEventInternal::WriteEvent(
                trace_writer, incr_state, static_category, event_name, type,
                trace_timestamp.nanoseconds);
            if (CatTraits::kIsDynamic) {
              DynamicCategory dynamic_category =
                  CatTraits::GetDynamicCategory(category);
              Category cat = Category::FromDynamicCategory(dynamic_category);
              cat.ForEachGroupMember(
                  [&](const char* member_name, size_t name_size) {
                    event_ctx.event()->add_categories(member_name, name_size);
                    return true;
                  });
            }
            if (&track != &TrackEventInternal::kDefaultTrack)
              event_ctx.event()->set_track_uuid(track.uuid);
            arg_function(std::move(event_ctx));
          }  // event_ctx
        });
  }

  template <typename CategoryType, typename Lambda>
  static void TraceWithInstances(uint32_t instances,
                                 const CategoryType& category,
                                 Lambda lambda) PERFETTO_ALWAYS_INLINE {
    using CatTraits = CategoryTraits<CategoryType>;
    if (CatTraits::kIsDynamic) {
      Base::template TraceWithInstances(instances, std::move(lambda));
    } else {
      Base::template TraceWithInstances<CategoryTracePointTraits>(
          instances, std::move(lambda), {CatTraits::GetStaticIndex(category)});
    }
  }

  // Records a track descriptor into the track descriptor registry and, if we
  // are tracing, also mirrors the descriptor into the trace.
  template <typename TrackType>
  static void SetTrackDescriptorImpl(
      const TrackType& track,
      std::function<void(protos::pbzero::TrackDescriptor*)> callback) {
    TrackRegistry::Get()->UpdateTrack(track, std::move(callback));
    Base::template Trace([&](typename Base::TraceContext ctx) {
      TrackEventInternal::WriteTrackDescriptor(
          track, ctx.tls_inst_->trace_writer.get());
    });
  }

  // Determines if the given dynamic category is enabled, first by checking the
  // per-trace writer cache or by falling back to computing it based on the
  // trace config for the given session.
  static bool IsDynamicCategoryEnabled(
      typename Base::TraceContext* ctx,
      const DynamicCategory& dynamic_category) {
    auto incr_state = ctx->GetIncrementalState();
    auto it = incr_state->dynamic_categories.find(dynamic_category.name);
    if (it == incr_state->dynamic_categories.end()) {
      // We haven't seen this category before. Let's figure out if it's enabled.
      // This requires grabbing a lock to read the session's trace config.
      auto ds = ctx->GetDataSourceLocked();
      Category category{Category::FromDynamicCategory(dynamic_category)};
      bool enabled = TrackEventInternal::IsCategoryEnabled(
          *Registry, ds->config_, category);
      // TODO(skyostil): Cap the size of |dynamic_categories|.
      incr_state->dynamic_categories[dynamic_category.name] = enabled;
      return enabled;
    }
    return it->second;
  }

  // Config for the current tracing session.
  protos::gen::TrackEventConfig config_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
