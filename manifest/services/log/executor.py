"""FIRST-TIME-ONLY scaffold (regenerated with --force).

source: services/log/system/package.art

Execution Manifest entries for the log FC. The rig
integrator owns this file after first emit — supervision strategy,
restart policy, and start_cmd are deployment choices, not .art
projections.

This is the file the rig.py overlay touches when it wants to
override scheduling policy or restart_attempts per machine.
"""
from artheia.manifest.execution import (
    Process,
    SchedulingPolicy,
    StartupConfig,
    StateDependentStartupConfig,
    TerminationBehaviorEnum,
)


PROCESS = Process(
    name="log",
    executable="log",
    function_cluster_affiliation="log",
    start_cmd=["bazel-bin/services/system/log/main/log"],
    state_dependent_startup_config=[
        StateDependentStartupConfig(
            function_group_state=["Default.Running"],
            startup_config=StartupConfig(
                name="log_startup",
                scheduling_policy=SchedulingPolicy.SCHED_OTHER,
                scheduling_priority=0,
                termination_behavior=(
                    TerminationBehaviorEnum.PROCESS_IS_NOT_SELF_TERMINATING
                ),
            ),
        ),
    ],
)
