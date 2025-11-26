#include "domain_abstraction_generator.h"

#include "domain_abstraction.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../domain_abstractions/domain_abstraction.h"
#include "../domain_abstractions/domain_abstraction_collection_generator.h"
#include "../task_utils/task_properties.h"

#include <memory>

using namespace std;

namespace cost_saturation {
DomainAbstractionGenerator::DomainAbstractionGenerator(const options::Options &opts)
    : AbstractionGenerator(opts),
      generator(
          opts.get<shared_ptr<domain_abstractions::DomainAbstractionCollectionGenerator>>(
              "domain_abstraction_collection_generator")),
      combine_labels(opts.get<bool>("combine_labels")) {
}

Abstractions DomainAbstractionGenerator::generate_abstractions(
    const shared_ptr<AbstractTask> &task,
    DeadEnds *) {
    utils::Timer domain_abstractions_timer;
    TaskProxy task_proxy(*task);

    task_properties::verify_no_axioms(task_proxy);
    if (task_properties::has_conditional_effects(task_proxy)) {
        cerr << "Error: configuration doesn't support conditional effects. "
             << endl;
        utils::exit_with(utils::ExitCode::SEARCH_UNSUPPORTED);
    }

    log << "Compute domain abstractions" << endl;
    domain_abstractions::DomainAbstractionCollection domain_abstractions =
        generator->generate(task_proxy);

    int max_size = 0;
    for (const domain_abstractions::DomainAbstraction &domain_abstraction : domain_abstractions) {
        max_size = max(max_size, static_cast<int>(domain_abstraction.size()));
    }

    log << "Number of domain abstractions: " << domain_abstractions.size() << endl;
    log << "Maximum size: " << max_size << endl;
    log << "Time for computing domain abstractions: " << domain_abstractions_timer << endl;

    log << "Build domain abstractions" << endl;
    utils::Timer wrapper_timer;
    shared_ptr<TaskInfo> task_info = make_shared<TaskInfo>(task_proxy);
    Abstractions abstractions;
    for (domain_abstractions::DomainAbstraction &domain_abstraction : domain_abstractions) {
        const domain_abstractions::DomainMapping &domain_mapping = domain_abstraction.get_domain_mapping();
        unique_ptr<Abstraction> wrapped_domain_abstraction =
            utils::make_unique_ptr<DomainAbstraction>(
                task_proxy, task_info, domain_abstraction, combine_labels, log);

        if (log.is_at_least_debug()) {
            log << "Domain abstraction " << abstractions.size() + 1 << ": "
                << domain_mapping << endl;
            wrapped_domain_abstraction->dump();
        }
        abstractions.push_back(move(wrapped_domain_abstraction));
    }

    int collection_size = 0;
    for (auto &abstraction : abstractions) {
        collection_size += abstraction->get_num_states();
    }

    log << "Time for wrapping domain abstractions: " << wrapper_timer << endl;
    log << "Number of states in collection: " << collection_size << endl;
    return abstractions;
}

static shared_ptr<AbstractionGenerator> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "DomainAbstraction generator",
        "");

    parser.add_option<shared_ptr<domain_abstractions::DomainAbstractionCollectionGenerator>>(
        "domain_abstraction_collection_generator",
        "domain abstractions generation method",
        OptionParser::NONE);
    parser.add_option<bool>(
        "combine_labels",
        "group labels that only induce parallel transitions",
        "true");
    utils::add_log_options_to_parser(parser);

    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    return make_shared<DomainAbstractionGenerator>(opts);
}

static Plugin<AbstractionGenerator> _plugin("domain_abstractions", _parse);
}
