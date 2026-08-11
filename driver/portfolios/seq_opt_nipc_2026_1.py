# -*- coding: utf-8 -*-

OPTIMAL = True

CONFIGS = [
    (1, ["--search",
           "astar(canonical_heuristic([domain_abstractions(multiple_domain_abstractions_cegar(blacklist_trigger_percentage=0.6, total_max_time=300, flaw_treatment=max_refined_single_atom, numeric_split_strategy=STANDARD, use_progress_weighted_flaw_selection=false, use_threshold_aware_numeric_splits=false, exec_entire_plan=execute_entire_plan, max_abstraction_size=500000, max_collection_size=5000000), combine_labels=true)]))"]),
    (1, ["--search",
           "astar(numeric_ipdb(cache_estimates=false, max_time=300, keep_parent_pointers=false, extend_abstract_state_space=false, collection_max_size=1000000, max_pdb_size=1000000, max_number_pdb_states=10000, exploration_heuristic=LMCUT, frontier_heuristic=LMCUT, failed_lookup_heuristic=BLIND))"]),
    (1, ["--search",
           "astar(lmcutnumeric())"]),
     ]
