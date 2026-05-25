There is no officially published formal EBNF/ANTLR grammar for TPT itself because TPT is primarily:

```text id="z9db1t"
graphical
model-based
hybrid automata based
```

rather than a textual DSL.

But from the documentation and papers, you can reconstruct the effective semantic structure fairly accurately.

The core formal model behind TPT is essentially:

```text id="jlwm5a"
hierarchical timed hybrid finite state machine
+
signal algebra
+
reactive transitions
+
variant expansion
```

---

# Canonical TPT conceptual structure

From the docs:

```text id="jlwm6b"
Project
 └── Test Model
      ├── Automata
      ├── States
      ├── Transitions
      ├── Variants
      ├── Signal Definitions
      ├── Assessments
      └── Execution Config
```

TPT explicitly models tests as:

```text id="jlwm7c"
hybrid automata
```

with:

* states
* temporal phases
* triggers
* signal generators
* reactive guards
* hierarchical substates
* parallel regions

([Wikipedia][1])

---

# Formalized conceptual grammar

A reasonably faithful abstract grammar would look like:

```text id="jlwm8d"
TestProject
    ::= { TestModel }

TestModel
    ::= Automaton
        | Assessment
        | ExecutionConfig
        | VariantSet

Automaton
    ::= StateMachine

StateMachine
    ::= State+
        Transition+

State
    ::= BasicState
        | ParallelState
        | HierarchicalState

BasicState
    ::= name
        Actions
        Duration?
        Variants?

ParallelState
    ::= region+

HierarchicalState
    ::= submachine

Transition
    ::= source
        target
        Trigger
        Guard?
        Action?

Trigger
    ::= EventTrigger
        | SignalCondition
        | Timeout
        | ExternalReaction

SignalDefinition
    ::= SetSignal
        | RampSignal
        | ExpressionSignal
        | RecordedSignal

Assessment
    ::= Assertion+
        | TemporalConstraint+
        | SignalConstraint+

Variant
    ::= parameterized alternative
```

This matches the published semantics surprisingly closely.

---

# Actual semantic layers in TPT

TPT is effectively composed of multiple orthogonal languages.

---

# 1. Structural FSM layer

Defines:

```text id="jlwm9e"
states
transitions
hierarchy
parallelism
```

This is the core hybrid automaton.

---

# 2. Temporal layer

Defines:

```text id="jlwm0f"
durations
timeouts
timed guards
sampling
synchronization
```

Example semantics:

```text id="jlwm1g"
stay in Idle for 5s
```

or:

```text id="jlwm2h"
transition when RPM > threshold
```

---

# 3. Signal algebra layer

Defines signal trajectories:

```text id="jlwm3i"
speed(t)
rpm(t)
voltage(t)
```

Operations include:

| Operation | Meaning |
| --- | --- |
| set | immediate value |
| ramp | continuous transition |
| hold | constant |
| waveform | arbitrary signal |
| expression | computed signal |

([Wikipedia][1])

---

# 4. Reactive/event layer

Transitions triggered by:

```text id="jlwm4j"
signal predicates
events
timeouts
system feedback
```

Example:

```text id="jlwm5k"
RPM > 1200
```

or:

```text id="jlwm6l"
ABS_Active == TRUE
```

---

# 5. Variant generation layer

One of TPT’s major ideas.

States/transitions may contain:

```text id="jlwm7m"
variants
```

which generate combinatorial test cases.

Example conceptual structure:

```text id="jlwm8n"
State "Driving"

Variants:
    speed = [30, 50, 120]
    road = [dry, wet]
    brake = [soft, hard]
```

Generates multiple concrete executions.

([Wikipedia][1])

---

# Closest theoretical model

Academically, TPT most closely resembles:

```text id="jlwm9o"
Hierarchical Timed Extended Finite State Machines
```

or:

```text id="jlwm0p"
Hybrid Automata
```

because it combines:

* discrete transitions
* continuous signals
* temporal semantics

---

# Minimal textual pseudo-TPT

A hypothetical textual representation might look like:

```text id="jlwm1q"
test EngineStart {

  state Init {
      set Ignition = OFF;
      wait 2s;
  }

  state Crank {
      set Starter = ON;
      ramp RPM from 0 to 800 in 1s;
  }

  transition Init -> Crank
      when Ignition == ON;

  state Idle {
      hold RPM = 800;
  }

  transition Crank -> Idle
      when RPM >= 800;

  assess {
      assert RPM < 1200;
      assert OilPressure > MIN;
  }
}
```

This is NOT actual TPT syntax.

But semantically it is very close.

---

# Real internal model likely resembles

Internally, TPT almost certainly stores models as:

```text id="jlwm2r"
graph structures
+
timed transitions
+
signal definitions
+
variant metadata
```

probably serialized as XML/project metadata.

---

# Strong relation to statecharts

TPT behavior is much closer to:

* Harel Statecharts
* UML state machines
* SCXML
* hybrid automata

than to ordinary FSMs.

Because it supports:

| Feature | TPT |
| --- | --- |
| hierarchy | yes |
| parallel regions | yes |
| continuous time | yes |
| signal algebra | yes |
| reactivity | yes |
| timed transitions | yes |

([eeNews Europe][2])

---

# Closest formal grammar approximation

A more formal EBNF-ish approximation:

```text id="jlwm3s"
machine      ::= state_machine

state_machine ::= state+
                  transition+

state        ::= "state" IDENT
                 state_body

state_body   ::= action*
                 submachine*
                 variant*
                 timeout*

transition   ::= source "->" target
                 [guard]
                 [trigger]
                 [action]

guard        ::= expression

trigger      ::= event
               | signal_condition
               | timeout

action       ::= signal_assignment
               | ramp_definition
               | function_call

signal_assignment
              ::= IDENT "=" expression

ramp_definition
              ::= "ramp"
                  IDENT
                  value
                  duration

variant      ::= "variant"
                 IDENT
                 value_set
```

---

# Important insight

TPT is not merely:

```text id="jlwm4t"
FSM testing
```

It is really:

```text id="jlwm5u"
continuous-time reactive hybrid-system testing
```

with FSM structure used as the organizational backbone.

That distinction matters a lot in automotive/embedded validation.

References:

* [TPT overview](https://en.wikipedia.org/wiki/TPT_%28software%29?utm_source=chatgpt.com)
* [Synopsys TPT methodology](https://www.synopsys.com/verification/virtual-prototyping/tpt/test-methodology.html?utm_source=chatgpt.com)
* [TPT tutorial mirror](https://docslib.org/doc/2160369/tpt-tutorial?utm_source=chatgpt.com)
* [Hybrid automata discussion](https://www.eenewseurope.com/en/model-based-testing-on-production-hardware/?utm_source=chatgpt.com)

[1]: https://en.wikipedia.org/wiki/TPT\_%28software%29?utm_source=chatgpt.com "TPT (software)"
[2]: https://www.eenewseurope.com/en/model-based-testing-on-production-hardware/?utm_source=chatgpt.com "Model-based testing on production hardware ..."