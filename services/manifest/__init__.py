"""Adaptive Platform Functional Cluster manifest (explicit, hand-authored).

:data:`FcLayer` is the L0 :class:`artheia.manifest.Layer` carrying all 18
Adaptive Platform Functional Clusters as :class:`SwComponent`,
:class:`Executable`, and :class:`Process` (Execution Manifest) entries.

Upper layers (e.g. ``demo/manifest/rig.py``) compose on top via
:func:`artheia.manifest.merge_layers`.
"""

from services.manifest.fc import FcLayer  # noqa: F401
