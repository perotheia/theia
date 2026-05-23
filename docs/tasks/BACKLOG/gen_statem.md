platform/runtime/ need FSM implementation to be used in applications ans services


proposa: reuse erlang gen_statem from up/otp as user facing API
otp/lib/stdlib/src/gen_statem.erl
otp/lib/stdlib/test/gen_statem_SUITE_data
otp/lib/stdlib/test/gen_statem_SUITE_data/oc_statem.erl
otp/lib/stdlib/test/gen_statem_SUITE_data/format_status_statem.erl
otp/lib/stdlib/test/gen_statem_SUITE.erl
otp/lib/ssl/src/ssl_gen_statem.erl

reuse up/hsmcpp/ c++ FSM as FSM definition syntax

creatively combine both. Keep it in same style as signal handling in platform/runtime/
