# Abstract Domain Computation Reference

## Core Principle: Abstract Values are RANGES

**Critical Understanding**: In domain abstractions with numeric variables, abstract values represent RANGES of concrete values, not single values.

### Notation:
- `a` = concrete value (single number)
- `a'` = abstract value (range/partition)
- `partition k` = the k-th discrete range in the abstraction

## Variable Types in Numeric Domain Abstractions

### 1. Regular Numeric Variables

**Definition**: User-defined numeric variables from the planning task. Each has an explicit abstraction with defined partitions.

**Example**:
```
Variable: x
Partitions:
  - partition 0: (-inf, 3)
  - partition 1: [3, 10)
  - partition 2: [10, inf)
```

**Properties**:
- Explicitly abstracted (partitions defined upfront)
- Part of the abstract state representation
- Can be affected by operator effects

### 2. Assignment Axioms (Derived Numeric Variables)

**Definition**: Numeric variables computed from other variables via arithmetic expressions.

**Syntax**: `derived_var := expression`

**Examples**:
```
sum := x + y
diff := x - y
scaled := x * 2
```

**Properties**:
- NOT part of the abstract state (only regular numeric and propositional variables are)
- Computed on-the-fly during abstract operator evaluation
- Can have implicit abstraction (derived from source variable ranges)
- Can cascade to comparison axioms

**Abstract Computation**:
When sources are abstract ranges, result is also a range:
```
x' = (-inf, 3), y' = (-inf, 1)
sum' = x' + y' = (-inf, 4)
```

### 3. Comparison Axioms (Derived Propositional Variables)

**Definition**: Boolean variables computed from comparison expressions over numeric variables.

**Syntax**: `comparison_var := (numeric_expr op threshold)`

**Examples**:
```
c := (b > 4)
ready := (x >= 10)
safe := (distance > threshold + 2)
```

**Properties**:
- Stored as propositional variables in the task
- PART of the abstract state representation (as boolean facts)
- Can be true, false, or BOTH (when range spans threshold)
- Computed during cascade evaluation

**Abstract Evaluation**:
```
b' = (-inf, 5)
c := (b' > 4)

Since b' contains values ≤ 4 AND values > 4:
c' = {true, false}  // Both values possible!
```

**Special Case - Range Fully on One Side**:
```
b' = [5, 10)
c := (b' > 4)

Since ALL values in b' are > 4:
c' = true  // Only true is possible
```

## Operator Components

### Preconditions

**Propositional Preconditions**:
```
pre: (door_open = true) ∧ (at_location = room1)
```
- Must match exactly in abstract state

**Numeric Preconditions**:
```
pre: (x >= 5)
```
- In abstract domain: checks if partition contains values satisfying condition
- Conservative: if partition partially satisfies, operator may or may not be applicable

### Effects

**Propositional Effects**:
```
eff: door_open := false
```
- Simple value change
- Single abstract successor for this variable

**Numeric Effects (Assignment Effects)**:
```
eff: x += 2
eff: y := x + 3
```
- Apply to abstract ranges
- Can cause partition transitions
- **Key**: One effect can lead to MULTIPLE abstract successors

## Abstract Operator Computation

### Example 1: Simple Numeric Effect with Comparison Axiom

**Setup**:
```
Regular variable: a
  - partition 0: (-inf, 3)
  - partition 1: [3, inf)

Derived variable: b := a + 2
Comparison axiom: c := (b > 4)

Operator: op[a] += 2
```

**Initial State**:
```
Concrete: a = 0
Abstract: a' = 0 (partition 0: (-inf, 3))
Derived:  b = a + 2 = 2
          c = (b > 4) = false
Abstract State: (a'=0, c'=false)
```

**Apply Operator op[a] += 2**:

1. **Compute affected range**:
   - Source: `a' ∈ (-inf, 3)`
   - Effect: `+= 2`
   - Result: `a' ∈ (-inf, 5)`

2. **Determine partition transitions**:
   - `(-inf, 5)` overlaps with:
     - Partition 0: `(-inf, 3)` ✓
     - Partition 1: `[3, inf)` ✓ (specifically [3, 5))
   
3. **Compute derived variable b' for each partition**:
   
   **Case 1: a' stays in partition 0**
   - `a' ∈ (-inf, 3)` after effect
   - `b' = a' + 2 ∈ (-inf, 5)`
   - `c' = (b' > 4)`: Range `(-inf, 5)` spans threshold 4
     - Some values ≤ 4 → c can be false
     - Some values > 4 → c can be true
   - **Possible states**: `(a'=0, c'=false)` AND `(a'=0, c'=true)`
   
   **Case 2: a' moves to partition 1**
   - `a' ∈ [3, 5)` after effect
   - `b' = a' + 2 ∈ [5, 7)`
   - `c' = (b' > 4)`: ALL values in `[5, 7)` are `> 4`
     - c must be true
   - **Possible state**: `(a'=1, c'=true)`

4. **Final Abstract Successor States**:
   From `(a'=0, c'=false)` via `op[a] += 2`:
   - `(a'=0, c'=false)`
   - `(a'=0, c'=true)`
   - `(a'=1, c'=true)`

**Three abstract successors from one concrete operator!**

### Example 2: Derived Variable with Multiple Source Ranges

**Setup**:
```
Regular variables:
  x: partition 0 = (-inf, 0), partition 1 = [0, inf)
  y: partition 0 = (-inf, 1), partition 1 = [1, inf)

Derived variable: z := x + y
```

**Question**: What are the possible abstract values of z?

**Computation**:

| x' | x range | y' | y range | z' = x' + y' | z range |
|----|---------|----|---------|--------------|---------| 
| 0 | (-inf, 0) | 0 | (-inf, 1) | x + y | (-inf, 1) |
| 0 | (-inf, 0) | 1 | [1, inf) | x + y | (-inf, inf) |
| 1 | [0, inf) | 0 | (-inf, 1) | x + y | (-inf, inf) |
| 1 | [0, inf) | 1 | [1, inf) | x + y | [1, inf) |

**Possible ranges for z'**:
- `(-inf, 1)` - when both x and y are negative/small
- `(-inf, inf)` - when one is large and one is small (full range!)
- `[1, inf)` - when both x and y are large

**Key Insight**: Derived variables have implicit abstraction based on source ranges.

### Example 3: Cascade Computation

**Setup**:
```
Regular variable: a
  - partition 0: (-inf, 5)
  - partition 1: [5, inf)

Assignment axiom: b := a + 3
Comparison axiom: c := (b > 10)

Operator: op[a] += 2
```

**Cascade Flow**:
```
op affects a → a changes partition → 
  b := a + 3 is re-evaluated → b range changes →
    c := (b > 10) is re-evaluated → c truth value changes
```

**From state (a'=0, c'=false)**:

1. **Direct effect on a**:
   - `a' ∈ (-inf, 5)`
   - After `+= 2`: `a' ∈ (-inf, 7)`
   - Partitions reached: 0 and 1

2. **Cascade to b (assignment axiom)**:
   - Case 1: `a' ∈ (-inf, 5)` → `b' ∈ (-inf, 8)`
   - Case 2: `a' ∈ [5, 7)` → `b' ∈ [8, 10)`

3. **Cascade to c (comparison axiom)**:
   - Case 1: `b' ∈ (-inf, 8)` → `c' = (b' > 10)` → `c' = false` (all values < 10)
   - Case 2: `b' ∈ [8, 10)` → `c' = (b' > 10)` → `c' = false` (all values < 10)

**Result**: 
- `(a'=0, c'=false)`
- `(a'=1, c'=false)`

Both successors have `c'=false` because the cascade shows b never exceeds 10.

## Hash Effect Computation

### Purpose
The hash effect encodes the change from predecessor state to successor state as a single integer, used for efficient state lookup.

### Formula

For a state transition changing multiple variables:

```
hash_effect = Σ(Δprop_i × mult_i) + Σ(Δnum_j × mult_j) + Σ(Δcomp_k × mult_k)
```

Where:
- `Δprop_i` = change in propositional variable i (value_new - value_old)
- `Δnum_j` = change in numeric partition j (partition_new - partition_old)
- `Δcomp_k` = change in comparison variable k (value_new - value_old)
- `mult_*` = hash multiplier for each variable (ensures unique hashes)

### Example Calculation

**State transition**: `(a'=0, c'=false)` → `(a'=1, c'=true)`

Assume hash multipliers:
- `mult[a] = 100`
- `mult[c] = 1`

**Computation**:
```
hash_effect = (1 - 0) × 100 + (1 - 0) × 1
           = 100 + 1
           = 101
```

**Multiple Hash Effects per Operator**:

Since one operator can have multiple abstract successors, each gets its own hash effect:

```
From (a'=0, c'=false):
  → (a'=0, c'=false): hash = 0 + 0 = 0
  → (a'=0, c'=true):  hash = 0 + 1 = 1
  → (a'=1, c'=true):  hash = 100 + 1 = 101
```

The AbstractOperator stores: `hash_effects = [0, 1, 101]`

## Critical Implementation Points

### 1. Enumerate All Partition Transitions

For each numeric variable affected by an operator:
- Compute the result range after applying the effect
- Determine which partitions this range overlaps
- Create separate abstract transitions for each overlapping partition

### 2. Compute Cascades for Each Transition

For each partition transition:
- Evaluate all assignment axioms with new partition ranges
- Evaluate all comparison axioms with derived numeric ranges
- Determine which comparison axioms can change truth value
- Include both true and false when range spans threshold

### 3. Conservative Over-Approximation

When uncertain about a comparison result:
- **Conservative approach**: Include both true and false
- **Exact approach** (better): Use partition bounds to determine exact truth values

Example:
```
b' ∈ [5, 10), comparison: (b > 4)
Conservative: c' = {true, false}
Exact: c' = true (all values in range are > 4)
```

### 4. State Representation

**Abstract state consists of**:
- Regular numeric variables (as partition indices)
- Propositional variables (including comparison axiom results)

**NOT in abstract state**:
- Assignment axiom results (derived numeric variables)
- Intermediate computation values

**Reason**: Assignment axioms are deterministic given the state, so they don't need to be stored.

## Summary Table: Variable Types

| Type | Example | In State? | How Computed | Can Cascade? |
|------|---------|-----------|--------------|--------------|
| Regular Numeric | `x` | ✓ (partition) | Operator effects | ✓ To derived vars |
| Assignment Axiom | `z := x + y` | ✗ | From sources | ✓ To comparisons |
| Comparison Axiom | `c := (b > 4)` | ✓ (boolean) | From numeric ranges | ✗ (terminal) |
| Regular Propositional | `door_open` | ✓ (value) | Operator effects | ✗ |

## Key Takeaways

1. **Abstract = Range**: Never forget that abstract values are ranges, not points
2. **Multiple Successors**: One operator can create many abstract successors
3. **Cascades Matter**: Changes propagate through assignment → comparison axioms
4. **Conservative Safety**: When in doubt, include all possibilities (over-approximate)
5. **State Composition**: Only regular vars and comparison results are in the state
6. **Hash Effects**: Store one hash effect per possible abstract successor

This reference should guide all implementation decisions for abstract operator construction with numeric variables and cascading effects.
