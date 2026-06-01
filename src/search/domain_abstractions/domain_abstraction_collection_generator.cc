#include "domain_abstraction_collection_generator.h"

#include "domain_abstraction.h"
#include "utils.h"

#include "../plugin.h"

#include "../utils/rng.h"
#include "../utils/rng_options.h"

using namespace std;

namespace domain_abstractions {
DomainAbstractionCollectionGenerator::DomainAbstractionCollectionGenerator(
    const options::Options &opts)
    : rng(utils::parse_rng_from_options(opts)) {
}

DomainAbstractionCollection DomainAbstractionCollectionGenerator::generate(
    const TaskProxy &task_proxy) {
    
    utils::Timer timer;
    DomainAbstractionCollection abstractions = compute_abstractions(task_proxy);
    return abstractions;
}

void add_domain_abstraction_collection_generator_options_to_parser(
    options::OptionParser &parser) {
    utils::add_rng_options(parser);
}

static PluginTypePlugin<DomainAbstractionCollectionGenerator> _type_plugin_collection(
    "DomainAbstractionCollectionGenerator",
    "Factory for domain abstraction collections");
}
