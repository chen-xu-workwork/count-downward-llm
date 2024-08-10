#ifndef NUMERIC_PDBS_NUMERIC_TASK_PROXY_H
#define NUMERIC_PDBS_NUMERIC_TASK_PROXY_H

#include "numeric_condition.h"

#include "../task_proxy.h"

namespace numeric_pdb_helper {

class NumericTaskProxy;

// Basic iterator support for proxy classes.

template<class ProxyCollection>
class ProxyIterator {
    const ProxyCollection &collection;
    std::size_t pos;
public:
    ProxyIterator(const ProxyCollection &collection, std::size_t pos)
            : collection(collection), pos(pos) {}
    ~ProxyIterator() = default;

    typename ProxyCollection::ItemType operator*() const {
        return collection[pos];
    }

    ProxyIterator &operator++() {
        ++pos;
        return *this;
    }

    bool operator==(const ProxyIterator &other) const {
        return pos == other.pos;
    }

    bool operator!=(const ProxyIterator &other) const {
        return !(*this == other);
    }
};

template<class ProxyCollection>
inline ProxyIterator<ProxyCollection> begin(ProxyCollection &collection) {
    return ProxyIterator<ProxyCollection>(collection, 0);
}

template<class ProxyCollection>
inline ProxyIterator<ProxyCollection> end(ProxyCollection &collection) {
    return ProxyIterator<ProxyCollection>(collection, collection.size());
}

class PropositionalPreconditionsProxy {
    const NumericTaskProxy *task;
    int op_index;
public:
    using ItemType = FactProxy;
    PropositionalPreconditionsProxy(const NumericTaskProxy &task, int op_index)
            : task(&task), op_index(op_index) {}
    ~PropositionalPreconditionsProxy() = default;

    std::size_t size() const;

    FactProxy operator[](std::size_t fact_index) const;
};

class NumericPreconditionsProxy {
    const NumericTaskProxy *task;
    int op_index;
public:
    using ItemType = std::shared_ptr<numeric_condition::RegularNumericCondition>;
    NumericPreconditionsProxy(const NumericTaskProxy &task, int op_index)
            : task(&task), op_index(op_index) {}
    ~NumericPreconditionsProxy() = default;

    std::size_t size() const;

    std::shared_ptr<numeric_condition::RegularNumericCondition> operator[](std::size_t fact_index) const;
};

class AssignEffectsProxy {
    const NumericTaskProxy *task;
    int op_index;
public:
    using ItemType = const std::pair<int, ap_float> &;
    AssignEffectsProxy(const NumericTaskProxy &task, int op_index)
            : task(&task), op_index(op_index) {}
    ~AssignEffectsProxy() = default;

    std::size_t size() const;

    const std::pair<int, ap_float> &operator[](std::size_t fact_index) const;
};

class AdditiveEffectsProxy {
    const NumericTaskProxy *task;
    int op_index;
public:
    using ItemType = std::pair<int, ap_float>;
    AdditiveEffectsProxy(const NumericTaskProxy &task, int op_index)
            : task(&task), op_index(op_index) {}
    ~AdditiveEffectsProxy() = default;

    std::size_t size() const;

    std::pair<int, ap_float> operator[](std::size_t fact_index) const;
};

class NumericOperatorProxy {
    const NumericTaskProxy *task;
    int index;
public:
    NumericOperatorProxy(const NumericTaskProxy &task, int index)
            : task(&task), index(index) {}

    ~NumericOperatorProxy() = default;

    bool operator==(const NumericOperatorProxy &other) const {
        assert(task == other.task);
        return index == other.index;
    }

    bool operator!=(const NumericOperatorProxy &other) const {
        return !(*this == other);
    }

    PropositionalPreconditionsProxy get_propositional_preconditions() const {
        return {*task, index};
    }

    NumericPreconditionsProxy get_numeric_preconditions() const {
        return {*task, index};
    }

    EffectsProxy get_propositional_effects() const;

    AssignEffectsProxy get_assign_effects() const {
        return {*task, index};
    }

    AdditiveEffectsProxy get_additive_effects() const {
        return {*task, index};
    }

    ap_float get_cost() const;

    const std::string &get_name() const;

    int get_id() const {
        return index;
    }
};

class NumericOperatorsProxy {
    const NumericTaskProxy *task;
public:
    using ItemType = NumericOperatorProxy;

    explicit NumericOperatorsProxy(const NumericTaskProxy &task)
            : task(&task) {}

    ~NumericOperatorsProxy() = default;

    std::size_t size() const;

    bool empty() const {
        return size() == 0;
    }

    NumericOperatorProxy operator[](std::size_t index) const {
        assert(index < size());
        return {*task, static_cast<int>(index)};
    }
};
}
#endif
