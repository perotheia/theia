---
name: bazel-build
description: Build common pero_theia targets with the correct --config flags. Args: firmware | gw-firmware | host | libs | psp | demo | package | test | all
disable-model-invocation: true
---

Run the bazel build command for the requested target group.

## Usage

`/bazel-build <target>`

## Targets

### `firmware`
```sh
bazel build //gateway/firmware/pero_cmp_ti:pero_cmp_ti.elf --config=ti_arm_cgt_18
bazel build //gateway/firmware/pero_cmp_ti:pero_cmp_ti.bin --config=ti_arm_cgt_18
```
Requires `/opt/ti/cgt_arm_18.1.1.LTS/` installed.

### `gw-firmware`
```sh
bazel build //gateway/firmware/pero_cmp_ti_gw:pero_cmp_ti_gw.elf --config=ti_arm_cgt_18
```
Requires `/opt/ti/cgt_arm_18.1.1.LTS/` installed.

### `host`
```sh
bazel build //gateway/libs/pero_cmp_lnx/demo:all \
            //services/pero_cmp_gw_svc:cmp_gw \
            //gateway/demo/pero_cmp_gw_cln_demo:cmp_gw_client
```

### `libs`
```sh
bazel build //gateway/libs/pero_cmp_lnx/lib:cmpdecoder \
            //gateway/libs/pero_cmp_lnx/lib:gw
```

### `psp`
```sh
bazel build //autosar/mlbevo_gen2_cmp_psp:generate  # codegen only
bazel build //autosar/mlbevo_gen2_cmp_psp:codec     # generate + compile (~6000 .c)
bazel build //autosar/mlbevo_gen2_cmp_psp:psp_so    # shared library
```

### `demo`
```sh
bazel build //autosar/demo/mlbevo_gen2_cmp_demo:mlbevo_demo
```

### `package`
```sh
bazel build //packaging:pero-libcmpdecoder \
            //packaging:pero-libpsp \
            //packaging:pero-libgw \
            //packaging:pero-gw-svc \
            //packaging:pero-gw-client \
            //packaging:pero-gw-stack
bash bazel-bin/packaging/dist.sh /opt/pero-gw/dist
```

### `test`
```sh
bazel test //gateway/libs/pero_cmp_lnx/... --test_output=errors
```

### `all`
Runs `host`, `libs`, `psp`, then `firmware` and `gw-firmware` (firmware requires TI CGT).
```sh
bazel build //gateway/libs/pero_cmp_lnx/demo:all \
            //gateway/libs/pero_cmp_lnx/lib:cmpdecoder \
            //gateway/libs/pero_cmp_lnx/lib:gw \
            //services/pero_cmp_gw_svc:cmp_gw \
            //gateway/demo/pero_cmp_gw_cln_demo:cmp_gw_client \
            //autosar/mlbevo_gen2_cmp_psp:psp_so
bazel build //gateway/firmware/pero_cmp_ti:pero_cmp_ti.elf       --config=ti_arm_cgt_18
bazel build //gateway/firmware/pero_cmp_ti_gw:pero_cmp_ti_gw.elf --config=ti_arm_cgt_18
```

## Notes

- `--config=ti_arm_cgt_18` runs without Bazel sandbox; armcl must be at `/opt/ti/cgt_arm_18.1.1.LTS/`
- Add `--config=ci` for CI-style output (no progress animation, errors-only test output)
- `MODULE.bazel.lock` is auto-managed — never edit by hand
- PSP codegen invokes `artheia gen-platform-protos` inside the Bazel action; the workspace venv must be on PATH (`.bazelrc` has `build --action_env=PATH`, but the calling shell still needs to put `.venv/bin` first)
