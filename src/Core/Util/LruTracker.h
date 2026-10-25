#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace fastlook::core
{
    // Tracks the `capacity` most recently touched keys (small capacities: linear search). Touch makes a key the most recent and
    // returns the key it pushed out, if any.
    template <class K>
    class LruTracker
    {
    public:
        explicit LruTracker(size_t capacity) : m_capacity(capacity) {}

        std::optional<K> Touch(K const& key)
        {
            auto it = std::find(m_keys.begin(), m_keys.end(), key);
            if (it != m_keys.end())
            {
                m_keys.erase(it);
                m_keys.push_back(key);
                return std::nullopt;
            }
            std::optional<K> evicted;
            if (m_capacity > 0 && m_keys.size() >= m_capacity)
            {
                evicted = std::move(m_keys.front());
                m_keys.erase(m_keys.begin());
            }
            m_keys.push_back(key);
            return evicted;
        }

        bool Contains(K const& key) const
        {
            return std::find(m_keys.begin(), m_keys.end(), key) != m_keys.end();
        }

        void Clear() { m_keys.clear(); }
        size_t Size() const { return m_keys.size(); }

    private:
        size_t m_capacity;
        std::vector<K> m_keys; // least recent first
    };
}
