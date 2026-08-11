# Count Downward & Count Downward Together

Several planners that are based on this repository participated in the International Planning Competition 2026. 

You can run its ``Count Downward'' configurations using:
```
fast-downward.py --alias nipc26-opt-pdbs domain.pddl problem.pddl                   // opt-1
fast-downward.py --alias nipc26-opt-domain-abstractions domain.pddl problem.pddl    // opt-2
fast-downward.py --alias nipc26-sat-hff domain.pddl problem.pddl                    // sat
fast-downward.py --alias nipc26-agl-hff domain.pddl problem.pddl                    // agl
```

The ``Count Downward Together'' configurations are portfolios, which can be run as follows (the time limit can be adapted, but portfolios need a limit):
```
fast-downward.py --overall-time-limit 30m --portfolio driver/portfolios/seq_opt_nipc_2026_1.py ...  // opt-1
fast-downward.py --overall-time-limit 30m --portfolio driver/portfolios/seq_opt_nipc_2026_2.py ...  // opt-2
```

The repository will only get very rudimentary maintenance, if at all. We recommend switching to PlanForge, a reimplementation in Rust:

https://github.com/mrlab-ai/PlanForge

# Numeric Fast Downward 

This repository is an extension of Numeric Fast Downward that was initiated by Chiara Piacentini and Ryo Kuroiwa. 

Numeric Fast Downward (NFD), an extension of Fast Downward, has been originally developed by Johannes Aldinger and Bernhard Nebel. 

The planner has been extended with two variants of numeric abstraction heuristics: PDBs and domain abstractions.

# ORIGINAL README (Fast Downward) 

Fast Downward is a domain-independent planning system.

The following directories are not part of Fast Downward as covered by this
license:
./src/search/ext

For the rest, the following license applies:

Copyright (C) 2003-2016 Malte Helmert
Copyright (C) 2008-2016 Gabriele Roeger
Copyright (C) 2012-2016 Florian Pommerening
Copyright (C) 2010-2015 Jendrik Seipp
Copyright (C) 2010, 2011, 2013-2015 Silvan Sievers
Copyright (C) 2013, 2015 Salome Simon
Copyright (C) 2014, 2015 Patrick von Reth
Copyright (C) 2015 Manuel Heusner, Thomas Keller
Copyright (C) 2009-2014 Erez Karpas
Copyright (C) 2014 Robert P. Goldman
Copyright (C) 2010-2012 Andrew Coles
Copyright (C) 2010, 2012 Patrik Haslum
Copyright (C) 2003-2011 Silvia Richter
Copyright (C) 2009-2011 Emil Keyder
Copyright (C) 2010, 2011 Moritz Gronbach, Manuela Ortlieb
Copyright (C) 2011 Vidal Alcázar Saiz, Michael Katz, Raz Nissim
Copyright (C) 2010 Moritz Goebelbecker
Copyright (C) 2007-2009 Matthias Westphal
Copyright (C) 2009 Christian Muise

Fast Downward is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

Fast Downward is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

For contact information see http://www.fast-downward.org/.
