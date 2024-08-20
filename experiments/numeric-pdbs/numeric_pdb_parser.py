#! /usr/bin/env python

import re
import sys

from lab.parser import Parser



def add_initial_h_values(content, props):
    """
    Add a mapping from heuristic names to initial h values.

    If exactly one initial heuristic value was reported, add it to the
    properties under the name "initial_h_value".

    """
    initial_h_values = {}
    matches = re.findall(
        r"Initial heuristic value for (.+): ([-]?\d+|infinity)$", content, flags=re.M
    )
    for heuristic, init_h in matches:
        if init_h == "infinity":
            init_h = sys.maxsize
        else:
            init_h = float(init_h)
        if heuristic in initial_h_values:
            props.add_unexplained_error(
                f"multiple initial h values found for {heuristic}"
            )
        initial_h_values[heuristic] = init_h

    props["initial_h_values_float"] = initial_h_values

    if len(initial_h_values) == 1:
        props["initial_h_value_float"] = list(initial_h_values.values())[0]

def add_pdb_constructed(content, props):
    props["pdb_constructed"] = 0
    if "ipdb_hillclimbing_time" in props:
        props["pdb_constructed"] = 1
    elif "pdb_collection_construction_time" in props:
        props["pdb_constructed"] = 1
    elif "pdb_construction_time" in props:
        props["pdb_constructed"] = 1


class NumericPDBParser(Parser):
    def __init__(self):
        Parser.__init__(self)
        self.add_pattern('number_reached_goal_states', 'Reached abstract goal states: (.+)', required=False, type=int)
        self.add_pattern('number_abstract_states', 'Generated abstract states: (.+)', required=False, type=int)
        self.add_pattern('pdb_construction_time', 'PDB construction time: (.+)s', required=False, type=float)
        self.add_pattern('number_sga_patterns', 'Found (.+) SGA patterns.', required=False, type=int)
        self.add_pattern('number_interesting_patterns', 'Found (.+) interesting patterns.', required=False, type=int)
        self.add_pattern('pdb_collection_construction_time', 'PDB collection construction time: (.+)s', required=False, type=float)
        self.add_pattern('pdb_dominance_pruning_time', 'Dominance pruning took (.+)s', required=False, type=float)
        self.add_pattern('ipdb_hillclimbing_time', 'iPDB: hill climbing time: (.+)s', required=False, type=float)
        self.add_pattern('dominance_pruning_time', 'Dominance pruning took (.+)s', required=False, type=float)
        self.add_pattern('number_pruned_subsets', 'Pruned (.+) of [.+] maximal additive subsets', required=False, type=int)
        self.add_pattern('number_pruned_pdbs', 'Pruned (.+) of [.+] PDBs', required=False, type=int)
        self.add_pattern('res_task_construction_time', 'Time to build restricted numeric task: (.+)s', required=False, type=float)


        self.add_function(add_initial_h_values)
        self.add_function(add_pdb_constructed)


def get_parser():
    return NumericPDBParser()
