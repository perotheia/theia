"""AUTO-GENERATED from system/apps/component.art, DO NOT EDIT (regen via gen-manifest).

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
            name='p1', executable=Explicit('//apps/Demo3WayP1/main:apps'),
            start_cmd=Explicit('bin/p1'), function_group=Explicit('applications'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='p2', executable=Explicit('//apps/Demo3WayP2/main:apps'),
            start_cmd=Explicit('bin/p2'), function_group=Explicit('applications'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='p3', executable=Explicit('//apps/Demo3WayP3/main:apps'),
            start_cmd=Explicit('bin/p3'), function_group=Explicit('applications'),
            fg_states={"Startup", "Running"},
        ),
        ProcessLayer(
            name='p4', executable=Explicit('//apps/Demo3WayP4/main:apps'),
            start_cmd=Explicit('bin/p4'), function_group=Explicit('applications'),
            fg_states={"Startup", "Running"},
        ),
    }),
    service=ServiceLayer(instances={
        ServiceInstanceLayer(
            name='p1_countersrv', interface=Explicit('system.apps.CounterSrv'),
            instance_id=Explicit(1),
            provided_by=Explicit('p1'),
        ),
        ServiceInstanceLayer(
            name='p1_inciface', interface=Explicit('system.apps.IncIface'),
            instance_id=Explicit(2),
            provided_by=Explicit('p1'),
        ),
        ServiceInstanceLayer(
            name='p3_inciface', interface=Explicit('system.apps.IncIface'),
            instance_id=Explicit(3),
            provided_by=Explicit('p3'),
        ),
    }),
    applications=ApplicationSetLayer(applications={
        # one AA bundling every process; host bound by the variant.
        ApplicationLayer(name='apps', processes={'p1', 'p2', 'p3', 'p4'}),
    }),
)


# Per-process supervisor metadata (modules + nodes) resolved from
# the .art at gen-manifest time. serialize-manifest folds this into the
# executor.json worker leaves. DeploymentLayer stays transport-free.
PROCESS_NODES = {   'p1': {   'modules': ['apps/Demo3WayP1'],
              'nodes': [   {   'name': 'counter',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010001'},
                           {   'name': 'driver',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010002'},
                           {   'name': 'ticker',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010003'}]},
    'p2': {   'modules': ['apps/Demo3WayP2'],
              'nodes': [   {   'name': 'observer',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010004'}]},
    'p3': {   'modules': ['apps/Demo3WayP3'],
              'nodes': [   {   'name': 'incrementer',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010005'}]},
    'p4': {   'modules': ['apps/Demo3WayP4'],
              'nodes': [   {   'name': 'demo_fsm',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010006'},
                           {   'name': 'demo_gate',
                               'reporting': True,
                               'tipc_instance': '0',
                               'tipc_type': '0xd0010007'}]}}
