*** Settings ***
Documentation     s5 pipeline runtime: DATA FLOWS across the cross-package
...               connect — filter's received count is nonzero and GROWS.
Library           ${CURDIR}/user_flow_lib.py

*** Test Cases ***
Samples Flow Producer To Consumer
    ${a}=    Theia Call    ${WS}    FilterCtrl    GetStats
    Sleep    1.5s
    ${b}=    Theia Call    ${WS}    FilterCtrl    GetStats
    Should Be True    ${b}[received] > 0          no samples arrived at filter
    Should Be True    ${b}[received] > ${a}[received]    count is not growing
    Should Be True    ${b}[last_value] > 0
