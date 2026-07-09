*** Settings ***
Documentation     s1 ws-bare runtime: the scaffolded placeholder node is bound
...               and answers its call surface through the live supervisor.
Library           ${CURDIR}/user_flow_lib.py

*** Test Cases ***
Placeholder Node Is Bound
    Tipc Bound    ${TIPC}

Placeholder Node Answers Its Call Surface
    ${rep}=    Theia Call    ${WS}    ${NODE}    ${OP}
    Log    reply: ${rep}
