"""Erlang-style supervisor daemon.

Reads a YAML supervisor manifest (emitted by ``artheia executor emit``)
and fork/exec's the declared children, applying OTP-style restart
strategy semantics:

- ``one_for_one`` — restart only the failed child.
- ``one_for_all`` — restart every child of the failing supervisor.
- ``rest_for_one`` — restart the failed child and every child declared
  *after* it; leave earlier siblings running.
- ``simple_one_for_one`` — not implemented at runtime (dynamic children
  aren't part of the executor today).

References:
- https://erlang.org/documentation/doc-4.9.1/doc/design_principles/sup_princ.html
- https://www.erlang.org/docs/20/man/supervisor
"""
