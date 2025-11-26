#include "abstraction_generator.h"

#include "../plugin.h"

using namespace std;

namespace cost_saturation {
AbstractionGenerator::AbstractionGenerator(const options::Options &)
    : log() {
}

static PluginTypePlugin<AbstractionGenerator> _type_plugin(
    "AbstractionGenerator",
    "");
}
