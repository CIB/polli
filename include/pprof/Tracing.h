#ifndef PPROF_TRACING_H
#define PPROF_TRACING_H

#include "string.h"
#include "pprof/pprof.h"
#include "pprof/Config.h"
#include "polli/Options.h"

#include <memory>

#ifdef POLLI_ENABLE_TRACING

#define PJIT_REGION_MAIN -1
#define PJIT_REGION_CODEGEN -2
#define PJIT_REGION_GET_PROTOTYPE -3
#define PJIT_REGION_SELECT_PARAMS -4

#define LIKWID_PERFMON

#include <likwid.h>
#define POLLI_TRACING_INIT polliTracingInit()
#define POLLI_TRACING_FINALIZE polliTracingFinalize()
#define POLLI_TRACING_REGION_START(ID, NAME) polliTracingRegionStart(ID, NAME)
#define POLLI_TRACING_REGION_STOP(ID, NAME) polliTracingRegionStop(ID, NAME)
#define POLLI_TRACING_SCOP_START(ID, NAME) polliTracingScopStart(ID, NAME)
#define POLLI_TRACING_SCOP_STOP(ID, NAME) polliTracingScopStop(ID, NAME)
namespace polli {
struct Tracer {
  virtual void init() const {}
  virtual void finalize() const {}
  virtual void regionStart(uint64_t Id, const char *Name) const {}
  virtual void regionStop(uint64_t Id, const char *Name) const {}
  virtual void scopStart(uint64_t Id, const char *Name) const {}
  virtual void scopStop(uint64_t Id, const char *Name) const {}
  virtual ~Tracer() = default;
};

struct LikwidTracer : public Tracer {
  void init() const override {
    likwid_markerInit();
    likwid_markerThreadInit();
  }
  void finalize() const override { likwid_markerClose(); }
  void regionStart(uint64_t Id, const char *Name) const override {
    likwid_markerStartRegion(Name);
  }
  void regionStop(uint64_t Id, const char *Name) const override {
    likwid_markerStopRegion(Name);
  }
  void scopStart(uint64_t Id, const char *Name) const override {
    likwid_markerStartRegion(Name);
  }
  void scopStop(uint64_t Id, const char *Name) const override {
    likwid_markerStopRegion(Name);
  }
};

struct PapiTracer : public Tracer {
  void init() const override { papi_region_setup(); }
  void finalize() const override {}
  void regionStart(uint64_t Id, const char *Name) const override {
    papi_region_enter(Id, Name);
  }
  void regionStop(uint64_t Id, const char *Name) const override {
    papi_region_exit(Id, Name);
  }
  void scopStart(uint64_t Id, const char *Name) const override {
    papi_region_enter_scop(Id, Name);
  }
  void scopStop(uint64_t Id, const char *Name) const override {
    papi_region_exit_scop(Id, Name);
  }
};

using TracerTy = std::unique_ptr<polli::Tracer>;
TracerTy getOrCreateActiveTracer();
}

#ifdef __cplusplus
extern "C" {
#endif
void polliTracingInit();
void polliTracingFinalize();
void polliTracingRegionStart(uint64_t Id, const char *Name);
void polliTracingRegionStop(uint64_t Id, const char *Name);
void polliTracingScopStart(uint64_t Id, const char *Name);
void polliTracingScopStop(uint64_t Id, const char *Name);
#ifdef __cplusplus
}
#endif
#else
#define POLLI_TRACING_INIT
#define POLLI_TRACING_FINALIZE
#define POLLI_TRACING_REGION_START(ID, NAME)
#define POLLI_TRACING_REGION_STOP(ID, NAME)
#define POLLI_TRACING_SCOP_START(ID, NAME)
#define POLLI_TRACING_SCOP_STOP(ID, NAME)
#endif
#endif //PPROF_TRACING_H
