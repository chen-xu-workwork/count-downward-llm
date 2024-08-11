#ifndef NUMERIC_PDBS_PATTERN_GENERATOR_MANUAL_H
#define NUMERIC_PDBS_PATTERN_GENERATOR_MANUAL_H

#include "pattern_generator.h"
#include "types.h"

namespace options {
class Options;
}

namespace numeric_pdbs {
class PatternGeneratorManual : public PatternGenerator {
    Pattern pattern;
public:
    explicit PatternGeneratorManual(const options::Options &opts);
    virtual ~PatternGeneratorManual() = default;

    virtual Pattern generate(std::shared_ptr<AbstractTask> task,
                             std::shared_ptr<numeric_pdb_helper::NumericTaskProxy> task_proxy) override;
};
}

#endif
