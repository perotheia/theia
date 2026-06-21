"""AUTO-GENERATED from system/services/cluster.art, DO NOT EDIT (regen via gen-manifest).

A base :class:`DeploymentLayer` on the orthogonal-ARA engine
(:mod:`artheia.manifest.deployment`). Each cluster member maps to one
EXECUTION-axis process; provided interfaces map to SERVICE-axis instances.

``machine`` is intentionally LEFT OPEN on every process: this is a BASE
manifest — a deploy variant binds each process to a machine (see
``manifest/demo/single.py`` for the override idiom). ``validate()`` of THIS
base therefore reports ``machine`` Undefined; that is expected — the variant
makes it consistent.

Authoring style: inline + literal. The process / service rows ARE the table.
"""
from __future__ import annotations

from artheia.manifest.algebra import Default, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer,
    ApplicationSetLayer,
    DeploymentLayer,
    ExecutionLayer,
    ProcessLayer,
    ServiceInstanceLayer,
    ServiceLayer,
)

DEPLOYMENT = DeploymentLayer(
    execution=ExecutionLayer(processes={
        ProcessLayer(
            name='com', executable=Explicit('//services/com/main:com'),
            start_cmd=Explicit('bin/com'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='crypto', executable=Explicit('//services/crypto/main:crypto'),
            start_cmd=Explicit('bin/crypto'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='diag', executable=Explicit('//services/diag/main:diag'),
            start_cmd=Explicit('bin/diag'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='fw', executable=Explicit('//services/fw/main:fw'),
            start_cmd=Explicit('bin/fw'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='idsm', executable=Explicit('//services/idsm/main:idsm'),
            start_cmd=Explicit('bin/idsm'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='log', executable=Explicit('//services/log/main:log'),
            start_cmd=Explicit('bin/log'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='nm', executable=Explicit('//services/nm/main:nm'),
            start_cmd=Explicit('bin/nm'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='osi', executable=Explicit('//services/osi/main:osi'),
            start_cmd=Explicit('bin/osi'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='per', executable=Explicit('//services/per/main:per'),
            start_cmd=Explicit('bin/per'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='phm', executable=Explicit('//services/phm/main:phm'),
            start_cmd=Explicit('bin/phm'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='rds', executable=Explicit('//services/rds/main:rds'),
            start_cmd=Explicit('bin/rds'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='sm', executable=Explicit('//services/sm/main:sm'),
            start_cmd=Explicit('bin/sm'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='tsync', executable=Explicit('//services/tsync/main:tsync'),
            start_cmd=Explicit('bin/tsync'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='ucm', executable=Explicit('//services/ucm/main:ucm'),
            start_cmd=Explicit('bin/ucm'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='shwa', executable=Explicit('//services/shwa/main:shwa'),
            start_cmd=Explicit('bin/shwa'), function_group=Explicit('services'),
            fg_states={"Startup", "Running"},
        ),
    }),
    applications=ApplicationSetLayer(applications={
        # one AA bundling every process; host bound by the variant.
        ApplicationLayer(name='services', processes={'com', 'crypto', 'diag', 'fw', 'idsm', 'log', 'nm', 'osi', 'per', 'phm', 'rds', 'sm', 'tsync', 'ucm', 'shwa'}),
    }),
)
