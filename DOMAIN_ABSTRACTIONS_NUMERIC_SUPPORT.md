# Domain Abstractions - Numeric Variables Support

## Overview
This document tracks the implementation of numeric variable support in domain abstractions for the numeric-fd planner.

## Key Concept
Domain abstractions discretize numeric variables into partitions, unlike pattern databases which keep continuous values. Each partition becomes a discrete "value" that can be treated like a propositional variable value.

## Implementation Status

### ✅ Completed Components

#### 1. Type Definitions (`types.h`, `types.cc`)
- **NumericRange**: Represents a range `[lower, upper)` with partition index
- **NumericDomainMapping**: Manages partitioning of numeric variables
  - `split_at(n)`: Splits a range at value `n` to create finer partitions
  - `get_partition_index(value)`: Returns which partition contains a value
  - `get_num_partitions()`: Returns total number of partitions
- **NumericDomainMappingType**: Typedef for `vector<NumericDomainMapping>`
- **DomainAbstractionState**: Simplified state representation
  - `state_hash`: Single combined hash for both propositional and numeric variables
  - Since numeric variables are discretized, we treat them uniformly with propositional vars
- **DomainAbstractionStateHash**: Hash function for state registry (simply returns state_hash)

#### 2. State Registry (`domain_abstraction_state_registry.h`, `domain_abstraction_state_registry.cc`)
- **DomainAbstractionStateRegistry**: Manages unique abstract states
  - Uses `unordered_set` with custom hash/equality for efficient lookup
  - `insert_state()`: Adds new state, returns ID
  - `get_id()`: Looks up existing state, returns `size_t::max()` if not found
  - `lookup_state()`: Retrieves state by ID
  - Stores states in `SegmentedVector` for memory efficiency

#### 3. Domain Abstraction Class (`domain_abstraction.h`, `domain_abstraction.cc`)
- **Simplified constructor**: No need for pattern_vars/numeric_vars
  - `domain_mapping` already tells us which propositional variables are in the abstraction (non-empty entries)
  - `numeric_domain_mapping` tells us which numeric variables are included
  - `has_numeric_variables` = `!numeric_domain_mapping.empty()`
- **Updated `get_value()` method**:
  - **Purely propositional case**: Direct hash indexing (no registry needed)
  - **Mixed prop/numeric case**:
    1. Compute combined hash for all variables:
       - Add propositional variables using `hash_multipliers[i] * abstract_val`
       - Add numeric partition indices using `hash_multipliers[domain_mapping.size() + i] * partition`
    2. Create `DomainAbstractionState` with single combined hash
    3. Look up state ID in registry
    4. Return `distances[state_id]`

#### 4. Build System (`DownwardFiles.cmake`)
- Added `domain_abstraction_state_registry` to build sources

#### 5. Type System Fixes
- Added TypeNamer specializations for enum types used in options:
  - `FlawTreatment` (in `cegar.cc`)
  - `InitSplitMethod` (in `cegar.cc`)
  - `InitSplitOptions` (in `domain_abstraction_generator_cegar.cc`)
  - `VariableSubset` (in `domain_abstraction_collection_generator_multiple.cc`)
  - `InitSplitQuantity` (in `domain_abstraction_collection_generator_multiple.cc`)

## Architecture Decisions

### Why Discretization?
- Numeric variables have infinite domains (continuous real numbers)
- Abstractions require finite state spaces for tractability
- Partitioning creates finite discrete "buckets" that can be reasoned about
- Each partition is treated like a propositional variable value

### Propositional vs Numeric State Representation
**Pattern Databases (PDBs):**
- Work with a **subset** of variables (pattern)
- Use `NumericState` with `prop_hash` + continuous `ap_float` values
- Store exact numeric values for pattern variables
- Need to distinguish between pattern and non-pattern variables

**Domain Abstractions:**
- Work with **all** variables, but abstracted
- Use `DomainAbstractionState` with single combined hash
- Discretize numeric variables into partitions (finite values)
- Treat discretized numeric variables uniformly with propositional variables
- More abstract, allows finite representation of infinite domains
- No need to separate pattern_vars/numeric_vars - mappings tell us everything

### State Registry Design
- Simpler than PDB `NumericStateRegistry` - just uses a single hash value
- Since partitions are discrete, we can compute a perfect/near-perfect hash
- Uses semantic hashing (single combined hash value) for equality
- Enables efficient lookup during heuristic computation
- Memory-efficient storage with `SegmentedVector`

## Usage Example

```cpp
// Create domain mapping for propositional variables
DomainMapping prop_mapping = {...};

// Create numeric domain mappings (one per numeric variable)
NumericDomainMappingType numeric_mapping;
numeric_mapping.push_back(NumericDomainMapping()); // var 0: initially [-inf, inf) → partition 0
numeric_mapping[0].split_at(10.0);  // Now: [-inf, 10) → partition 0, [10, inf) → partition 1
numeric_mapping[0].split_at(5.0);   // Now: [-inf, 5) → 0, [5, 10) → 1, [10, inf) → 2

// Create state registry
auto registry = make_unique<DomainAbstractionStateRegistry>();

// Build domain abstraction
// Note: No need to pass pattern_vars/numeric_vars explicitly
// - prop_mapping tells us which propositional variables are abstracted (non-empty entries)
// - numeric_mapping tells us which numeric variables are abstracted (presence in vector)
DomainAbstraction abs(
    move(prop_mapping),
    move(numeric_mapping),
    move(hash_multipliers),  // Should include multipliers for both prop and numeric vars
    move(distances),
    move(wildcard_plan),
    move(registry)
);

// Query heuristic value
int h = abs.get_value(state);
```

## Future Work / TODOs

### 🔲 Domain Abstraction Factory
- ✅ Added `numeric_domain_mapping` member variable
- ✅ Updated hash_multipliers to support numeric variables (with TODO for implementation)
- 🔲 Build `NumericDomainMappingType` during CEGAR refinement (currently empty)
- 🔲 Populate state registry during abstraction construction
- 🔲 Implement numeric variable support in abstract operators
- 🔲 Update regression search to handle numeric variables

### 🔲 CEGAR Refinement for Numeric Variables
- Implement splitting logic in CEGAR loop
- Determine when/where to split numeric ranges
- Integrate with flaw detection

### 🔲 Testing & Validation
- Unit tests for `NumericDomainMapping::split_at()`
- Integration tests with numeric planning domains
- Performance benchmarks vs pattern databases

### 🔲 Optimization Opportunities
- Pre-compute hash multipliers for numeric partitions
- Investigate perfect hashing for small partition counts
- Memory profiling for large state registries

## Implementation Notes

### Accessing Numeric Variables from State
```cpp
// Get numeric variable value from state
ap_float value = state.nval(numeric_var_id);

// Get partition index
int partition = numeric_domain_mapping[i].get_partition_index(value);
```

### Hash Computation
- Single combined hash for both propositional and numeric variables
- Propositional variables: `state_hash += hash_multipliers[i] * abstract_val`
- Numeric partitions: `state_hash += hash_multipliers[domain_mapping.size() + i] * partition`
- Since partitions are discrete integers, we can use the same hashing approach as propositional variables
- This enables potential perfect hashing if abstraction size is known upfront

### Error Handling
- `get_partition_index()` returns `-1` if value not in any range (shouldn't happen)
- `get_id()` returns `size_t::max()` if state not found in registry
- `get_value()` returns `INT_MAX` for unknown states (safe fallback)

## References
- Pattern Database implementation: `src/search/numeric_pdbs/pattern_database.{h,cc}`
- Numeric State Registry: `src/search/numeric_pdbs/numeric_state_registry.{h,cc}`
- Fast Downward CEGAR: `src/search/cegar/` (classical planning reference)

## Current Implementation Status

### What Works Now
- ✅ **Purely propositional domain abstractions**: Full support for classical planning
- ✅ **Type system**: All data structures for numeric support are in place
- ✅ **State registry**: Ready to handle mixed propositional/numeric states
- ✅ **Hash computation**: `get_value()` can compute combined hashes
- ✅ **Factory infrastructure**: `numeric_domain_mapping` member added, hash_multipliers prepared

### What's Missing for Full Numeric Support
1. **CEGAR Refinement**: Need to populate `numeric_domain_mapping` during refinement
   - Decide when/how to split numeric ranges
   - Implement flaw detection for numeric variables
   
2. **Abstract Operators**: Need to handle numeric effects
   - Currently only propositional preconditions/effects are abstracted
   - Need to abstract numeric assignment effects (e.g., `x += 2`)
   
3. **Regression Search**: Need to compute predecessors for numeric variables
   - Similar to numeric PDB approach
   - Cannot use simple perfect hashing (infinite domains)
   - Need state registry from the start

4. **Goal States**: Need to abstract numeric goals
   - Currently only propositional goals supported

## Build Status
✅ Compiles successfully with gcc-14.3.0
✅ All files added to CMake build system
✅ Factory infrastructure ready for numeric variables
⚠️ Runtime testing pending (requires CEGAR implementation for numeric variables)

---
**Last Updated**: 2025-10-06
**Author**: GitHub Copilot + User
**Branch**: domain-abstractions
