

unite ART common type declaration system/package.art to be used in proto messages and in c++ type aliases

candidates for comont types:
up/mosaic-eng-ref
❯ vi ./vehicle_os/onboard/signals/common_types/si_units.proto
❯ vi ./vehicle_os/onboard/signals/common_types/vector.proto
❯ vi ./vehicle_os/onboard/signals/common_types/vehicle_state.proto


we already have messages in ART, for this part we need type aliases declaration.
generate c++ headers with package as namespace with type aliases to message.
