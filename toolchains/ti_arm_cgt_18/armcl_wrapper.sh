#!/bin/bash
# armcl_wrapper.sh — adapts Bazel GCC-style invocation to TI ARM CGT 18.1.1.LTS
#
# Key adaptations:
#  1. Expands Bazel @params_file response files inline
#  2. Filters GCC-only compile flags
#  3. Translates -MD/-MF <file> → creates empty GNU Make .d file
#  4. Translates -o file.o → --output_file=file.o (armcl compile-only output)
#  5. Adds TI CGT standard include dir

ARMCL=/opt/ti/cgt_arm_18.1.1.LTS/bin/armcl
TI_INC=/opt/ti/cgt_arm_18.1.1.LTS/include

# ── Expand @params_file arguments ────────────────────────────────────────
expanded=()
for arg in "$@"; do
    if [[ "$arg" == @* ]]; then
        params_file="${arg#@}"
        if [[ -f "$params_file" ]]; then
            while IFS= read -r line; do
                # Shell-split each line to handle quoted args
                eval "expanded+=($line)"
            done < "$params_file"
        else
            expanded+=("$arg")
        fi
    else
        expanded+=("$arg")
    fi
done

# ── Filter and translate ──────────────────────────────────────────────────
filtered=()
dep_file=""
obj_file=""
skip_next=false
next_is_mf=false
next_is_o=false

for arg in "${expanded[@]}"; do
    if $skip_next; then
        skip_next=false
        continue
    fi
    if $next_is_mf; then
        dep_file="$arg"
        next_is_mf=false
        continue
    fi
    if $next_is_o; then
        obj_file="$arg"
        next_is_o=false
        filtered+=("--output_file=$arg")   # armcl uses --output_file, not -o
        continue
    fi
    case "$arg" in
        # GCC dep-tracking flags
        -MD|-MQ|-MP)            continue ;;
        -MF)                    next_is_mf=true; continue ;;
        -MF*)                   dep_file="${arg#-MF}"; continue ;;
        -MT|-MQ)                skip_next=true; continue ;;
        # GCC code-gen flags
        -frandom-seed=*)        continue ;;
        -fno-*|-fstack-*)       continue ;;
        -fomit-frame-pointer|-fno-omit-frame-pointer|-fPIC) continue ;;
        -ffunction-sections|-fdata-sections) continue ;;
        # GCC warning flags Bazel injects
        -Wunused-but-set-parameter|-Wno-free-nonheap-object) continue ;;
        --diag_warning=*)       continue ;;
        # GCC quote-style include
        -iquote)                skip_next=true; continue ;;
        -iquote*)               continue ;;
        # GCC target selection (toolchain config handles this for armcl)
        -std=c99|-std=c11)      continue ;;
        -mcpu=*|-mfpu=*|-mfloat-abi=*|-mbig-endian|-mthumb) continue ;;
        # GCC warning flags
        -Wextra|-Wall|-W[A-Za-z]*) continue ;;
        # GCC linker flags (passed during -z link step)
        -Wl,--gc-sections|-Wl,*)    continue ;;
        -specs=*)               continue ;;
        -lm|-lc|-lgcc)          continue ;;
        # GCC-only output flag (convert to TI form)
        -o)
            next_is_o=true
            continue ;;
        --output_file=*)        obj_file="${arg#--output_file=}"; filtered+=("$arg") ;;
        *)                      filtered+=("$arg") ;;
    esac
done

# Write GNU Make .d file so Bazel can parse it
if [[ -n "$dep_file" ]]; then
    target="${obj_file:-${dep_file%.d}.o}"
    chmod +w "$dep_file" 2>/dev/null || true
    printf '%s:\n' "$target" > "$dep_file" 2>/dev/null || true
fi

# Detect link step: if -c is not in filtered args, prepend -z (linker mode)
is_compile=false
for arg in "${filtered[@]}"; do
    [[ "$arg" == "-c" ]] && is_compile=true && break
done

if ! $is_compile; then
    # Link mode: prepend -z and silicon flags
    exec "$ARMCL" -z "${filtered[@]}"
else
    exec "$ARMCL" "-I$TI_INC" "${filtered[@]}"
fi
