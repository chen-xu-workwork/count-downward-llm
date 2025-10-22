# Numeric Effect Flaws in CEGAR

## The Problem

### Example Scenario
```
Initial state: x = 0
Operator o: x += 1, precondition: x ≤ 2
Goal: x > 2
```

### What Happens with Goal-Based Refinement Only

**Iteration 1:**
- Initial partition: `x ∈ (-∞, ∞) → partition 0`
- Abstract plan: `[]` (empty - goal seems reachable immediately)
- Concrete execution: x = 0
- Goal check: x > 2? No! x = 0
- **Goal flaw detected**: Need x > 2
- **Refinement**: Split at threshold 2
  - New partitions: `(-∞, 2) → partition 0`, `[2, ∞) → partition 1`

**Iteration 2:**
- Partitions: `(-∞, 2) → partition 0`, `[2, ∞) → partition 1`
- Abstract plan: `[o]` (one application moves from partition 0 to partition 1)
- Concrete execution:
  - Start: x = 0 (in partition 0: (-∞, 2))
  - Apply o: x = 1 (still in partition 0: (-∞, 2))
  - Final: x = 1
- Goal check: x > 2? No! x = 1
- **Goal flaw detected**: Need x > 2

**Problem**: We want to refine at value 1 (where we actually are), but we **cannot split at the goal threshold (2) again** - that boundary already exists!

### The Core Issue

**The abstract plan is wrong** because:
1. The abstraction thinks one application of `o` moves from partition 0 `(-∞, 2)` to partition 1 `[2, ∞)`
2. But in reality, `o` with `x += 1` starting from x = 0 gives x = 1, which is **still in partition 0**
3. We need **multiple applications** of `o` to cross the threshold

**Why can't we just split at the goal again?**
- The partition boundary at 2 already exists
- `split_at(2)` returns the same number of partitions (no refinement)
- The assertion `refined_any` fails
- CEGAR terminates without finding a solution

## The Solution: Effect Flaws

### Key Insight

We need to detect when an operator's **numeric effects** are not accurately captured by the abstraction. This happens when:

1. The **abstract transition** says: "Applying operator o moves from partition i to partition j"
2. The **concrete execution** shows: "Applying operator o with numeric effects results in a value that's **still in partition i** (or a different partition than expected)"

This is an **effect flaw** - the abstraction's model of how numeric effects change the state is inaccurate.

### Effect Flaw Definition

An **effect flaw** occurs when:
- Operator `o` is applied in abstract state `s_abstract`
- Abstract transition predicts: `apply(o, s_abstract) → s'_abstract`
- Concrete execution: Starting from corresponding `s_concrete`, applying `o` gives `s'_concrete`
- But `abstract_value(s'_concrete) ≠ s'_abstract`

**Specifically for numeric variables:**
- Before: `x_concrete = v`, `x_abstract = partition_i`
- Apply `o` with effect `x += delta`
- After: `x_concrete = v + delta`, but `x_abstract` should be `partition_j`
- **Flaw**: If `abstract_value(v + delta) == partition_i` but abstract plan assumed transition to `partition_j`

### Refinement Strategy for Effect Flaws

When an effect flaw is detected:

**Split at the CONCRETE VALUE after applying the effect:**

```
Before applying o: x_concrete = 0, partition = 0: (-∞, 2)
Apply o with x += 1: x_concrete = 1, partition = 0: (-∞, 2)

Effect flaw: Expected to reach partition 1 [2, ∞), but actually in partition 0
Concrete value after effect: 1

Refinement: Split at value 1
New partitions: (-∞, 1) → 0, [1, 2) → 1, [2, ∞) → 2
```

### Example Walkthrough with Effect Flaws

**Iteration 1:**
- Partitions: `(-∞, ∞) → 0`
- Abstract plan: `[]`
- Execution: x = 0
- **Goal flaw**: x = 0, need x > 2
- **Refinement**: Split at threshold 2
  - New: `(-∞, 2) → 0`, `[2, ∞) → 1`

**Iteration 2:**
- Partitions: `(-∞, 2) → 0`, `[2, ∞) → 1`
- Abstract plan: `[o]` (assumes one application reaches goal)
- Execution:
  - Start: x = 0, partition 0
  - Apply o: x += 1 → x = 1
  - Check: x = 1 is in partition 0 `(-∞, 2)`
- **Effect flaw detected!**
  - Abstract plan expected: partition 0 → partition 1
  - Concrete result: partition 0 → partition 0 (still!)
  - Concrete value after effect: **x = 1**
- **Refinement**: Split at value 1
  - New: `(-∞, 1) → 0`, `[1, 2) → 1`, `[2, ∞) → 2`

**Iteration 3:**
- Partitions: `(-∞, 1) → 0`, `[1, 2) → 1`, `[2, ∞) → 2`
- Abstract plan: `[o, o]` (now knows it needs two steps)
  - Step 1: partition 0 → partition 1
  - Step 2: partition 1 → partition 2
- Execution:
  - Start: x = 0, partition 0
  - Apply o: x = 1, partition 1 ✓
  - Apply o: x = 2, partition 2 ✓
- Goal check: x = 2 is in partition 2 `[2, ∞)`, goal is x > 2
- **Goal flaw**: x = 2, but need x > 2 (strictly greater)
- **Refinement**: Split at value 2
  - New: `(-∞, 1) → 0`, `[1, 2) → 1`, `[2, 2] → 2`, `(2, ∞) → 3`

**Iteration 4:**
- Partitions: `(-∞, 1) → 0`, `[1, 2) → 1`, `[2, 2] → 2`, `(2, ∞) → 3`
- Abstract plan: `[o, o, o]`
  - Step 1: partition 0 → partition 1
  - Step 2: partition 1 → partition 2
  - Step 3: partition 2 → partition 3
- Execution:
  - Start: x = 0
  - Apply o: x = 1
  - Apply o: x = 2
  - Apply o: x = 3
- Goal check: x = 3 is in partition 3 `(2, ∞)`, goal is x > 2 ✓
- **Success!**

## Implementation Requirements - SIMPLIFIED APPROACH

### The One Rule

**Always split at the concrete numeric values observed when executing the abstract plan.**

### 1. Detect Flaws During Plan Execution

When validating the abstract plan in concrete space:

```cpp
vector<NumericFlaw> get_flaws(...) {
    vector<ap_float> numeric_state = initial_numeric_state;
    
    // Execute plan step by step
    for (each operator o in abstract_plan) {
        // Check preconditions
        if (!preconditions_satisfied(o, state, numeric_state)) {
            // Precondition flaw on some comparison axiom
            for (each failed comparison axiom c) {
                // Trace to regular numeric variables
                for (each var v in dependencies[c]) {
                    // Record CURRENT concrete value
                    detected_numeric_flaws.add(v, numeric_state[v]);
                }
            }
            return flaws;
        }
        
        // Apply effects
        apply_effects(state, numeric_state, o);
    }
    
    // Check goals
    if (!goals_satisfied(state, numeric_state)) {
        for (each failed goal comparison axiom c) {
            for (each var v in dependencies[c]) {
                // Record CURRENT concrete value
                detected_numeric_flaws.add(v, numeric_state[v]);
            }
        }
    }
}
```

### 2. Fix Flaws by Splitting at Observed Values

```cpp
bool fix_numeric_flaws(vector<NumericFlaw> &flaws) {
    for (each flaw in flaws) {
        // Simply split at the concrete value we observed
        numeric_domain_mapping[flaw.var_id].split_at(flaw.concrete_value);
    }
}
```

### 3. No Special Cases

- No distinction between "first refinement" vs "subsequent refinement"
- No threshold extraction from comparison axioms
- No boundary checking or special handling
- No effect flaw detection after operator application

### 4. Progressive Refinement Happens Naturally

By always splitting at observed concrete values:
- First iteration: Split at values where goals/preconditions fail
- Next iterations: Split at new concrete values observed
- Gradually builds up fine-grained partitioning
- Converges when abstract plan matches concrete execution

## Why This Wasn't Caught Earlier

The current implementation only checks for:
1. **Precondition flaws**: When operator not applicable
2. **Goal flaws**: When final state doesn't satisfy goals

But it **doesn't check**:
- Whether the numeric effects move variables to the expected abstract partitions
- Whether the abstract plan's predicted state changes match concrete execution

## Comparison with Propositional CEGAR

| Aspect | Propositional | Numeric |
|--------|---------------|---------|
| **State space** | Finite, enumerable | Infinite, continuous |
| **Abstraction** | Merge values | Partition ranges |
| **Effects** | Discrete assignment | Arithmetic operations |
| **Effect flaws?** | Not needed (all values explicit) | **Essential!** (infinite values) |

In propositional CEGAR, if the abstract plan says "set x=5", we know exactly what happens. But in numeric CEGAR, if the abstract plan says "apply x += 1", we don't know the result without tracking concrete values.

## Summary

**Effect flaws are necessary** because:
1. Numeric effects are **relative** (x += 1), not absolute (x = 5)
2. The abstraction must learn **where effects actually take us**
3. Goal-based refinement alone doesn't capture intermediate states
4. We must split at **concrete post-effect values**, not thresholds

**Without effect flaws:**
- Abstract plans can be too short
- Refinement gets stuck (can't split at same threshold twice)
- CEGAR fails to converge

**With effect flaws:**
- Abstraction learns the "granularity" of numeric changes
- Progressive refinement creates intermediate partitions
- CEGAR converges to accurate model of numeric dynamics
