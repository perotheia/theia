"""FIRST-TIME-ONLY scaffold (regenerated with --force).

source: services/system/com/package.art

Execution Manifest entries for the com FC. The rig
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
    name="com",
    executable="com",
    function_cluster_affiliation="com",
    start_cmd=["bazel-bin/services/system/com/main/com"],
    state_dependent_startup_config=[
        StateDependentStartupConfig(
            function_group_state=["Default.Running"],
            startup_config=StartupConfig(
                name="com_startup",
                scheduling_policy=SchedulingPolicy.SCHED_OTHER,
                scheduling_priority=0,
                termination_behavior=(
                    TerminationBehaviorEnum.PROCESS_IS_NOT_SELF_TERMINATING
                ),
            ),
        ),
    ],
)
