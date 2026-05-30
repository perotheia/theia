// theia::runtime::RuntimeContext
//
// The bundle passed into every app's Inputs. Holds shared resources
// the app didn't construct itself: the Logger, the Clock, the
// TimerFactory. Generated app code reads members directly; tests
// inject mocks via the same struct.
//
// `Runtime` (separate header) builds + owns this bundle, then
// instantiates the app's Inputs from it.

#pragma once

#include <memory>

#include "Clock.hh"
#include "Logger.hh"
#include "Timer.hh"

namespace theia {
namespace runtime {

struct Context {
    std::shared_ptr<Logger>                 logger;
    std::shared_ptr<Clock>                  clock;
    std::shared_ptr<TimerFactoryInterface>  timer_factory;
};

}  // namespace runtime
}  // namespace theia
