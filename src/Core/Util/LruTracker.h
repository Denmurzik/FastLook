#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace fastlook::core
{
    // Tracks the `capacity` most recently touched keys (small capacities: linear search). Touch makes a key the most recent and
    // returns the key it pushed out, if any. Capacity 0 holds nothing: Touch hands the key straight back.
    template <class K>
    class LruTracker
    {
    public:
        explicit LruTracker(size_t capacity) : m_capacity(capacity) {}

        std::optional<K> Touch(K const& key)
        {
            return TouchProtected(key, [](K const&) { return false; });
        }

        // Like Touch, but a key for which isProtected returns true is never pushed out: when every candidate is protected the
        // tracker grows past its capacity instead.
        template <class Pred>
        std::optional<K> TouchProtected(K const& key, Pred isProtected)
        {
            if (m_capacity == 0)
            {
                return key;
            }
            auto it = std::find(m_keys.begin(), m_keys.end(), key);
            if (it != m_keys.end())
            {
                m_keys.erase(it);
                m_keys.push_back(key);
                return std::nullopt;
            }
            std::optional<K> evicted;
            if (m_keys.size() >= m_capacity)
            {
                auto victim = std::find_if(m_keys.begin(), m_keys.end(), [&](K const& k) { return !isProtected(k); });
                if (victim != m_keys.end())
                {
                    evicted = std::move(*victim);
                    m_keys.erase(victim);
                }
            }
            m_keys.push_back(key);
            return evicted;
        }

        // The least recent key for which pred returns true, without removing it.
        template <class Pred>
        std::optional<K> OldestIf(Pred pred) const
        {
            auto it = std::find_if(m_keys.begin(), m_keys.end(), pred);
            return it == m_keys.end() ? std::nullopt : std::optional<K>{*it};
        }

        bool Erase(K const& key)
        {
            auto it = std::find(m_keys.begin(), m_keys.end(), key);
            if (it == m_keys.end())
            {
                return false;
            }
            m_keys.erase(it);
            return true;
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
