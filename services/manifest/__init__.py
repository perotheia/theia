"""Adaptive Platform Functional Cluster manifest (generated from .art).

:data:`ServicesLayer` is the L0 :class:`artheia.manifest.Layer` carrying
the ``cluster Services`` FCs (from ``services/system/system.art``) as
:class:`SwComponent`, :class:`Executable`, and :class:`Process`
(Execution Manifest) entries.

Upper layers (e.g. ``demo/manifest/rig.py``) compose on top via
:func:`artheia.manifest.merge_layers`.
"""

from services.manifest.service import ServicesLayer  # noqa: F401
