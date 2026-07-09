*** Settings ***
Documentation     s2 ws-demo runtime: all four demo processes run under the
...               supervisor; the driver's 10x Inc{5} landed (counter == 50).
Library           ${CURDIR}/user_flow_lib.py

*** Test Cases ***
All Demo Processes Are Up
    ${n}=    Supervised Process Count    ${WS}
    Should Be True    ${n} >= 4    expected 4 demo processes, saw ${n}

Counter Accumulated The Driver Increments
    # P1's driver contributes exactly 50 (10 x Inc{5}); P3's incrementer keeps
    # adding on its own tick, so the live value only GROWS past that floor.
    ${rep}=    Theia Call    ${WS}    CounterNode    Get
    Should Be True    ${rep}[value] >= 50    driver increments missing (value=${rep}[value])
