*** Settings ***
Documentation     s3 --with-services runtime: the platform tree comes up under
...               one supervisor (per/nm held down: no etcd/netadmin in CI).
Library           ${CURDIR}/user_flow_lib.py

*** Test Cases ***
Service Tree Is Up
    ${n}=    Supervised Process Count    ${WS}
    Should Be True    ${n} >= 8    expected the FC tree (>=8 rows), saw ${n}
