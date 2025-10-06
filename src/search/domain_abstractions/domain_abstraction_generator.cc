#include "domain_abstraction_generator.h"

#include "domain_abstraction_factory.h"

#include "../plugin.h"
#include "../option_parser.h"

#include "../utils/rng.h"
#include "../utils/rng_options.h"

using namespace std;

namespace domain_abstractions {
DomainAbstractionGenerator::DomainAbstractionGenerator(
    const options::Options &opts)
    : rng(utils::parse_rng_from_options(opts)) {
}

DomainAbstractionGeneratorIdentity::DomainAbstractionGeneratorIdentity(
    const options::Options &opts)
    : DomainAbstractionGenerator(opts) {
}

DomainAbstraction DomainAbstractionGeneratorIdentity::build_abstraction(
    const TaskProxy &task_proxy) {
    const int num_variables = task_proxy.get_variables().size();
    DomainMapping domain_mapping;
    domain_mapping.reserve(task_proxy.get_variables().size());
    vector<int> domain_sizes;
    domain_sizes.reserve(num_variables);
    for (VariableProxy var : task_proxy.get_variables()) {
        vector<int> identity(var.get_domain_size());
        iota(identity.begin(), identity.end(), 0);
        domain_mapping.push_back(move(identity));
        domain_sizes.push_back(var.get_domain_size());
    }

    DomainAbstractionFactory factory(task_proxy, domain_mapping, domain_sizes,
                                     true, rng, true);
    return factory.generate();
}

void add_domain_abstraction_generator_options_to_parser(
    options::OptionParser &parser) {
    //utils::add_log_options_to_parser(parser);
    utils::add_rng_options(parser);
}

static PluginTypePlugin<DomainAbstractionGenerator> _type_plugin_collection(
    "DomainAbstractionGenerator",
    "Factory for domain abstraction");
}
