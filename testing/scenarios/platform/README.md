# Platform regression tests

Tests of the platform layer — supervisor, runtime, gateway. These are
SUT regression tests (theia is the system under test).

  executor/   — supervisor + restart strategy + lifecycle
  runtime/    — platform/runtime: GenServer, GenStateM, Tracer, ...
  gateway/    — bus routing, signal translation
