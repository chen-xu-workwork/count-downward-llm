#ifndef DOMAIN_ABSTRACTIONS_TYPES_H
#define DOMAIN_ABSTRACTIONS_TYPES_H

#include <vector>

namespace domain_abstractions {
class DomainAbstraction;

using DomainMapping = std::vector<std::vector<int>>;
using DomainAbstractionCollection = std::vector<DomainAbstraction>;
}

#endif