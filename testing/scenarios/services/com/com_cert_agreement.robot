*** Settings ***
Documentation     com/crypto certificate agreement — consistent reporting.
...
...               com's gRPC TLS slot mode (THEIA_COM_TLS_SLOT) does NOT read
...               the certificate from disk or guess. It ASKS crypto via
...               GetCert(slot) over TIPC and acts on the authoritative answer:
...
...                 - crypto: 'no certificate' (UNKNOWN_IDENTIFIER / empty PEM)
...                     → com runs INSECURE *by agreement* (the cert-not-deployed
...                       case the user called out),
...                 - crypto: OK + PEM → TLS (engine key, cert from crypto),
...                 - crypto unreachable / other error → FAIL-CLOSED (com aborts
...                   for a supervisor restart rather than open an insecure port).
...
...               So insecure mode is enabled ONLY on crypto's explicit
...               'no cert', never on a crypto outage. This suite runs the
...               cc_test //services/com/test:test_cert_agreement (which calls
...               com_tls.hpp's crypto_get_cert against the live crypto FC) and
...               reports its verdict into Robot output.
...
...               Needs crypto listening (`theia start`); SKIPs cleanly otherwise.
Library           ${CURDIR}/com_cert_agreement_lib.py

Force Tags        services-com    services-crypto    live    tls


*** Test Cases ***
Com Runs Insecure Only On Crypto's No-Cert Reply
    [Documentation]    Against the live crypto FC, an undeployed slot yields the
    ...                NO_CERT agreement (crypto UNKNOWN_IDENTIFIER) — so com
    ...                would serve insecure by agreement, not silently on a
    ...                crypto outage. A regression that downgrades on outage (the
    ...                security hole) fails this case.
    Require Crypto Listening
    Run Cert Agreement Test
