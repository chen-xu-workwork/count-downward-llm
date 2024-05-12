#! /usr/bin/env python

import re

from lab.parser import Parser




class DecouplingParser(Parser):
    def __init__(self):
        Parser.__init__(self)
        self.add_pattern('number_reached_goal_states', 'Reached abstract goal states: (.+)', required=False, type=int)
        self.add_pattern('number_abstract_states', 'Generated abstract states: (.+)', required=False, type=int)
        self.add_pattern('pdb_construction_time', 'PDB construction time: (.+)s', required=False, type=float)

          

def get_parser():
    return DecouplingParser()
