
system definition - spec is mixed with implementation files.
services/system/sm/component.art
services/system/sm/main/main.cc

implementation should stay on :
services/com
services/sm
services/...

THings to fix
1. two com implementation to merge/replace with most recent.
services/com
services/system/com/main

2. move others
services/system
./ucm/main
./per/main
./sm/main

3. remove implementation

./exec/main
./exec/main/BUILD.bazel
./exec/main/main.cc

exec FC implemented by supervisor
