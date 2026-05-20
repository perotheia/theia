// platform::runtime::LifecycleInterface
//
// The base class generated apps inherit from. The runtime calls OnCreate
// after constructing the app, OnStart when subscriptions go live, and
// OnStop on shutdown. All three are noexcept — an Adaptive AUTOSAR-ish
// constraint that lets the runtime keep the call sites lockfree.

#pragma once

namespace platform {
namespace runtime {

class LifecycleInterface {
 public:
  virtual ~LifecycleInterface() = default;

  // Called once after construction. Wire up signal subscriptions here.
  virtual void OnCreate() noexcept = 0;

  // Called once after OnCreate returns, when the runtime's I/O loop is
  // ready to deliver events. Start periodic work here.
  virtual void OnStart() noexcept = 0;

  // Called once at shutdown, before destruction. Cancel periodic work
  // and release resources held by the app.
  virtual void OnStop() noexcept = 0;
};

}  // namespace runtime
}  // namespace platform
